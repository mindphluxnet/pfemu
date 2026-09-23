/* verify.c - the machine-readable verdict, for the service in docs/VERIFY.md.
 *
 * Everything else the emulator prints is for a person: -scoredbg lays out a
 * column table, run.c dumps registers and a hex window, replay_report()
 * writes prose.  A server should not be reading any of it.  That text has
 * already changed shape once mid-project - the attempt line grew an `ended`
 * field and tests/golden/run.sh had to accept both widths - and the moment a
 * service parses it, it becomes an interface nobody is tracking as one.
 *
 * So: -verify FILE writes exactly one JSON object and nothing else, into a
 * file of its own.  Not stdout, which by exit time carries several hundred
 * lines of [pfemu] diagnostics; not stderr, which carries the rest.  A file
 * also gives the caller an unambiguous crash signal - if the emulator dies
 * mid-run, the file is simply not there, which no amount of partial stdout
 * can tell you apart from a short run.
 *
 * The object always has "status", one of:
 *
 *   refused   - nothing ran.  Bad .pfr, wrong install, trainer armed.
 *   mismatch  - it ran, and the artifacts disagree with the recording.
 *               Either a nondeterminism bug or a doctored file; from out
 *               here those look identical, and both mean do not trust it.
 *   verified  - it ran and reproduced the recording.
 *
 * and the exit code follows it (0 / 2 / 1), so a caller that only wants the
 * verdict never has to open the file at all.  Exit codes are untouched when
 * -verify is absent, so nothing existing moves.
 *
 * "score" is deliberately not the top-level answer.  "best" is: the
 * highest-scoring attempt that satisfies every eligibility condition in
 * sc_close().  Deriving that here rather than in each caller is the whole
 * point - the rule lives next to the evidence for it, and a client that
 * reimplements it can only get it wrong.
 */
#include "pfemu.h"

/* Which build wrote the object.  The service stores it beside every
 * verdict, because re-verifying a kept .pfr under a newer build is how a
 * determinism regression would be found in production, and that
 * comparison is only worth something if the old answer says which build
 * gave it.  src/build.h is generated from `git describe` by the Makefile
 * and by build.bat, and it is included unconditionally: a compile path
 * that forgot to generate it should fail, not ship a verdict that quietly
 * says "unknown".  A checkout without git history does say "unknown". */
#include "build.h"

int verify_on = 0;
static const char *verify_path = NULL;
static const char *pending_code = NULL;   /* set just before a refusal */
static int emitted = 0;                   /* the object is written once */
static int status_code = 0;               /* process exit code */

void verify_arm(const char *path){
    verify_on = 1;
    verify_path = path;
    /* Arming is a fresh start.  A session only ever arms once, so this is
     * for tests/verify, which drives many cases through one process. */
    emitted = 0;
    status_code = 0;
    pending_code = NULL;
    /* The verdict needs a score, and the score needs the hooks.  Making the
     * caller remember a second flag is the same footgun the capture hash
     * used to have, where a run without -wav silently produced nothing to
     * compare - so -verify turns on what it needs. */
    scoredbg_on = 1;
}

int verify_exit_code(void){ return status_code; }

void verify_code(const char *code){ pending_code = code; }

/* JSON string body - quotes, backslash, and the control range.  Paths and
 * refusal messages both reach this, and a Windows path is all backslashes. */
static void jstr(FILE *f, const char *s){
    fputc('"', f);
    for(; s && *s; s++){
        unsigned char c = (unsigned char)*s;
        switch(c){
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if(c < 0x20) fprintf(f, "\\u%04x", c);
            else fputc(c, f);
        }
    }
    fputc('"', f);
}

/* The opening of every object, refusals included, up to the value of
 * "status".  "build" is additive, so the schema number stays 1. */
static void jhead(FILE *f){
    fputs("{\n  \"pfemu_verify\": 1,\n  \"build\": ", f);
    jstr(f, PFEMU_BUILD);
    fputs(",\n  \"status\": ", f);
}

static FILE *vopen(void){
    FILE *f;
    if(!verify_on || emitted || !verify_path) return NULL;
    f = fopen(verify_path, "wb");
    if(!f){
        fprintf(stderr, "[verify] cannot write '%s'\n", verify_path);
        emitted = 1;          /* do not try again on a later path */
        status_code = 1;
        return NULL;
    }
    emitted = 1;
    return f;
}

/* A refusal: the run never started, so there is nothing but the reason.
 * Called from run.c's fail_msg(), which every refusal already goes through -
 * that is why this needs no changes at twenty return sites. */
void verify_fail(const char *msg){
    FILE *f;
    /* Guarded here as well as in vopen(), because a suppressed emission must
     * not move the exit code either: run.c's exit path can call fail_msg()
     * after a verdict has already been written, and that late failure is not
     * allowed to turn a verified run into a refused one. */
    if(!verify_on || emitted) return;
    f = vopen();
    status_code = 1;
    if(!f) return;
    jhead(f);
    fputs("\"refused\",\n  \"error\": ", f);
    jstr(f, pending_code ? pending_code : "refused");
    fputs(",\n  \"message\": ", f);
    jstr(f, msg ? msg : "");
    fputs("\n}\n", f);
    fclose(f);
}

static void jattempt(FILE *f, const ScoreAttempt *a, const char *ind){
    fprintf(f, "%s{ \"index\": %d, \"table\": %d, \"players\": %d,"
               " \"balls\": %d, \"ball_reached\": %d, \"launches\": %d,"
               " \"springflips\": %d, \"score\": %llu, \"ended\": ",
            ind, a->index, a->table, a->players, a->balls, a->ball_reached,
            a->launches, a->springflips, (unsigned long long)a->score);
    jstr(f, a->ended);
    fputs(", \"rankable\": ", f);
    fputs(a->rankable ? "true" : "false", f);
    fputs(", \"reason\": ", f);
    jstr(f, a->reason);
    fprintf(f, ", \"start_emu\": %.6f, \"end_emu\": %.6f,"
               " \"start_cycles\": %llu, \"end_cycles\": %llu }",
            a->start_emu, a->end_emu,
            (unsigned long long)a->start_cycles,
            (unsigned long long)a->end_cycles);
}

/* The exit verdict.  Called from run.c after replay_report() and
 * fantasies_score_report(), so every number here is the same one those
 * printed - this reformats, it never recomputes. */
void verify_report(void){
    ReplayVerify rv;
    FILE *f;
    int n, i, best = -1;
    unsigned long long best_score = 0;
    int footer_ok, events_ok, wav_state, ok;
    const char *nwarn[8];
    int nw = 0;

    if(!verify_on || emitted) return;
    replay_verify_state(&rv);

    /* Not a replay at all.  -verify on a play or record run is a caller
     * mistake, and saying so in the object beats an empty file. */
    if(!rv.valid){
        f = vopen();
        status_code = 1;
        if(!f) return;
        jhead(f);
        fputs("\"refused\",\n"
              "  \"error\": \"not_a_replay\",\n"
              "  \"message\": \"-verify needs -replay: there is nothing to"
              " verify in a play or record run\"\n}\n", f);
        fclose(f);
        return;
    }

    events_ok = (rv.events_injected == rv.events_total);
    /* Cycles are the authoritative stop; emu_time is cycles/ips and carries
     * only the six decimals the footer was printed with, so it is compared
     * loosely and kept for the humans reading the file. */
    footer_ok = (rv.act_cycles == rv.rec_cycles) &&
                (rv.act_emu - rv.rec_emu < 1e-6) &&
                (rv.rec_emu - rv.act_emu < 1e-6);

    /* -1 unknown (the file predates the footer hash), 0 differs, 1 same. */
    if(!rv.have_rec_wav || !strcmp(rv.rec_wav, "none")) wav_state = -1;
    else if(!strcmp(rv.act_wav, "none"))                wav_state = -1;
    else wav_state = !strcmp(rv.rec_wav, rv.act_wav);

    if(wav_state < 0) nwarn[nw++] = "no_recorded_wav_hash";
    /* The ball counter only ever counts up (docs/VERIFY.md).  If it went
     * backwards, a ball came back by a route the model does not know, and
     * every attempt boundary after that point is suspect - so it travels
     * with the verdict rather than staying in a stderr line. */
    if(fantasies_score_rewound()) nwarn[nw++] = "ball_counter_rewound";

    ok = footer_ok && events_ok && wav_state != 0;
    /* A missing recorded hash is a tolerance for a player's own old file,
     * never for a verifier: -strict is the policy that says so. */
    if(wav_state < 0 && replay_is_strict()) ok = 0;

    n = fantasies_score_count();
    for(i = 0; i < n; i++){
        ScoreAttempt a;
        if(!fantasies_score_get(i, &a) || !a.rankable) continue;
        if(best < 0 || a.score > best_score){ best = i; best_score = a.score; }
    }

    f = vopen();
    status_code = ok ? 0 : 2;
    if(!f) return;

    jhead(f);
    jstr(f, ok ? "verified" : "mismatch");
    fputs(",\n  \"strict\": ", f);
    fputs(replay_is_strict() ? "true" : "false", f);
    fputs(",\n  \"release\": ", f);
    jstr(f, rv.release ? rv.release : "");
    /* Which PFEMU-STATE/ the session ran against: "canonical-1" for a ranked
     * recording, "install" for one made against the player's own.  Only the
     * first is evidence of a score, and -strict refuses the second before it
     * gets here; the field is for a verdict made without -strict. */
    fputs(",\n  \"state\": ", f);
    jstr(f, rv.state ? rv.state : "install");
    fputs(",\n  \"replay\": {\n", f);
    fprintf(f, "    \"events_total\": %d,\n    \"events_injected\": %d,\n",
            rv.events_total, rv.events_injected);
    fprintf(f, "    \"recorded\": { \"emu\": %.6f, \"cycles\": %llu, "
               "\"wav_hash\": ", rv.rec_emu, (unsigned long long)rv.rec_cycles);
    jstr(f, rv.have_rec_wav ? rv.rec_wav : "none");
    fprintf(f, ", \"wav_samples\": %lu },\n", rv.rec_wav_samples);
    fprintf(f, "    \"actual\":   { \"emu\": %.6f, \"cycles\": %llu, "
               "\"wav_hash\": ", rv.act_emu, (unsigned long long)rv.act_cycles);
    jstr(f, rv.act_wav);
    fprintf(f, ", \"wav_samples\": %lu },\n", rv.act_wav_samples);
    fprintf(f, "    \"footer_match\": %s,\n", footer_ok ? "true" : "false");
    fprintf(f, "    \"events_match\": %s,\n", events_ok ? "true" : "false");
    fputs("    \"wav_match\": ", f);
    fputs(wav_state < 0 ? "null" : (wav_state ? "true" : "false"), f);
    fputs("\n  },\n", f);

    fputs("  \"attempts\": [", f);
    for(i = 0; i < n; i++){
        ScoreAttempt a;
        if(!fantasies_score_get(i, &a)) continue;
        fputs(i ? ",\n" : "\n", f);
        jattempt(f, &a, "    ");
    }
    fputs(n ? "\n  ],\n" : "],\n", f);

    /* The one field a leaderboard reads.  null is a real answer: the run
     * contained no attempt that satisfies every condition - most often
     * because it ended mid-table, which does not count. */
    if(best >= 0 && ok){
        ScoreAttempt a;
        fantasies_score_get(best, &a);
        fputs("  \"best\":\n", f);
        jattempt(f, &a, "    ");
    } else {
        fputs("  \"best\": null", f);
    }

    fputs(",\n  \"warnings\": [", f);
    for(i = 0; i < nw; i++){
        if(i) fputs(", ", f);
        jstr(f, nwarn[i]);
    }
    fputs("]\n}\n", f);
    fclose(f);
}
