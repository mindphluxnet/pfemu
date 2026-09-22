/* Regression test for the -verify verdict (src/verify.c).
 *
 * The JSON object is an interface now - a service reads it and decides
 * whether a score counts - and the argument for having it at all was that
 * the human-readable output kept changing shape underneath its readers.  An
 * interface with no test would have the same problem one release later, so:
 * this pins the field names, the three statuses, the exit codes, and the
 * eligibility rule that picks "best".
 *
 * It links src/verify.c against stubs, exactly like tests/fuzz does with
 * src/replay.c.  Nothing here boots a guest or opens a .pfr; a case is one
 * call to verify_report() over a hand-built state, so the whole suite runs
 * in milliseconds and needs no installation.  That is also what lets it run
 * on a stock CI runner, which the golden suite still cannot.
 *
 * Build: make verify-test    then ./pfemu-verify-test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../../src/compat.h"
#include "../../src/pfemu.h"

/* ------------------------------------------------------------- the stubs */
/* The state verify.c reads.  Each case fills these in, calls the entry point
 * under test, and reads the file back. */
static ReplayVerify g_rv;
static ScoreAttempt g_at[8];
static int g_nat = 0;
static int g_rewound = 0;
static int g_strict = 0;

int scoredbg_on = 0;

void replay_verify_state(ReplayVerify *out){ *out = g_rv; }
int  replay_is_strict(void){ return g_strict; }
int  fantasies_score_count(void){ return g_nat; }
int  fantasies_score_rewound(void){ return g_rewound; }
int  fantasies_score_get(int i, ScoreAttempt *out){
    if(i < 0 || i >= g_nat) return 0;
    *out = g_at[i];
    return 1;
}

/* ---------------------------------------------------------------- driver */
#define OUT "pfemu-verify-selftest.json"

static char buf[16384];
static int fails = 0, cases = 0;
/* -print: echo every object.  The contract is easier to check by looking
 * at it than by reading the fprintf calls that build it, and the shell
 * extractors in tests/golden/run.sh were written against this output. */
static int printing = 0;

static void reset(void){
    memset(&g_rv, 0, sizeof(g_rv));
    memset(g_at, 0, sizeof(g_at));
    g_nat = 0; g_rewound = 0; g_strict = 0;
    snprintf(g_rv.rec_wav, sizeof(g_rv.rec_wav), "none");
    snprintf(g_rv.act_wav, sizeof(g_rv.act_wav), "none");
    remove(OUT);
    verify_arm(OUT);
}

/* A replayed run that reproduced everything: the baseline each case bends
 * exactly one way, so a failure names the one thing that moved. */
static void good(void){
    reset();
    g_rv.valid           = 1;
    g_rv.release         = "deluxe";
    g_rv.events_total    = 630;
    g_rv.events_injected = 630;
    g_rv.rec_emu         = 295.564838;
    g_rv.act_emu         = 295.564838;
    g_rv.rec_cycles      = 1773389028ULL;
    g_rv.act_cycles      = 1773389028ULL;
    g_rv.have_rec_wav    = 1;
    snprintf(g_rv.rec_wav, sizeof(g_rv.rec_wav), "312aab44211acf2e");
    snprintf(g_rv.act_wav, sizeof(g_rv.act_wav), "312aab44211acf2e");
    g_rv.rec_wav_samples = 6219264;
    g_rv.act_wav_samples = 6219264;
}

static void attempt(int index, int table, unsigned long long score,
                    const char *ended, const char *reason, int rankable){
    ScoreAttempt *a = &g_at[g_nat++];
    a->index = index; a->table = table; a->players = 1;
    a->balls = 3; a->ball_reached = 4; a->launches = 5; a->springflips = 0;
    a->score = score; a->ended = ended; a->reason = reason;
    a->rankable = rankable;
    a->start_emu = 1.0; a->end_emu = 2.0;
    a->start_cycles = 10; a->end_cycles = 20;
}

static void slurp(void){
    FILE *f = fopen(OUT, "rb");
    size_t n = 0;
    buf[0] = 0;
    if(!f) return;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
}

static void check(const char *name, int code_want, const char *const *want,
                  const char *const *unwanted){
    int code = verify_exit_code(), bad = 0;
    int i;
    cases++;
    slurp();
    if(code != code_want){
        printf("  FAIL %-34s exit %d, wanted %d\n", name, code, code_want);
        bad = 1;
    }
    for(i = 0; want && want[i]; i++)
        if(!strstr(buf, want[i])){
            printf("  FAIL %-34s missing: %s\n", name, want[i]);
            bad = 1;
        }
    for(i = 0; unwanted && unwanted[i]; i++)
        if(strstr(buf, unwanted[i])){
            printf("  FAIL %-34s present but should not be: %s\n",
                   name, unwanted[i]);
            bad = 1;
        }
    if(bad){
        fails++;
        printf("  ---- %s ----\n%s----\n", name, buf);
    } else {
        printf("  ok   %s\n", name);
    }
    if(printing && !bad) printf("---- %s ----\n%s", name, buf);
}

int main(int argc, char **argv){
    if(argc > 1 && !strcmp(argv[1], "-print")) printing = 1;
    printf("[verify] selftest\n");

    /* 1. The happy path.  This is the object a service parses, so every
     *    field it reads is pinned here by name. */
    {
        static const char *w[] = {
            "\"pfemu_verify\": 1", "\"build\": \"",
            "\"status\": \"verified\"",
            "\"strict\": false", "\"release\": \"deluxe\"",
            "\"events_total\": 630", "\"events_injected\": 630",
            "\"footer_match\": true", "\"events_match\": true",
            "\"wav_match\": true",
            "\"cycles\": 1773389028", "\"wav_hash\": \"312aab44211acf2e\"",
            "\"wav_samples\": 6219264",
            "\"score\": 20652570", "\"rankable\": true",
            "\"reason\": \"rankable\"", "\"ended\": \"attract\"",
            "\"table\": 1", "\"ball_reached\": 4", "\"launches\": 5",
            "\"warnings\": []", NULL };
        static const char *n[] = { "\"best\": null", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        verify_report();
        check("verified, one rankable attempt", 0, w, n);
    }

    /* 2. The capture hash disagrees.  Nondeterminism or a doctored file -
     *    indistinguishable from here, and both mean do not trust it. */
    {
        static const char *w[] = { "\"status\": \"mismatch\"",
                                   "\"wav_match\": false",
                                   "\"best\": null", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        snprintf(g_rv.act_wav, sizeof(g_rv.act_wav), "0000000000000000");
        verify_report();
        check("wav hash differs", 2, w, NULL);
    }

    /* 3. The footer disagrees.  Cycles are the authoritative stop. */
    {
        static const char *w[] = { "\"status\": \"mismatch\"",
                                   "\"footer_match\": false",
                                   "\"best\": null", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        g_rv.act_cycles = 1773389027ULL;
        verify_report();
        check("footer cycles differ by one", 2, w, NULL);
    }

    /* 4. The replay stopped before the last event.  A short run reproduces
     *    nothing, whatever the numbers it did reach say. */
    {
        static const char *w[] = { "\"status\": \"mismatch\"",
                                   "\"events_match\": false",
                                   "\"best\": null", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        g_rv.events_injected = 629;
        verify_report();
        check("events short of the file", 2, w, NULL);
    }

    /* 5. A file from before the footer carried a hash.  Tolerated for a
     *    player's own old recording, and said out loud. */
    {
        static const char *w[] = { "\"status\": \"verified\"",
                                   "\"wav_match\": null",
                                   "\"warnings\": [\"no_recorded_wav_hash\"]",
                                   NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        g_rv.have_rec_wav = 0;
        verify_report();
        check("no recorded wav hash", 0, w, NULL);
    }

    /* 6. The same file under -strict.  That is what the flag is for: a
     *    verifier refuses what a player may keep. */
    {
        static const char *w[] = { "\"status\": \"mismatch\"",
                                   "\"strict\": true",
                                   "\"wav_match\": null",
                                   "\"best\": null", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        g_rv.have_rec_wav = 0;
        g_strict = 1;
        verify_report();
        check("no recorded wav hash, -strict", 2, w, NULL);
    }

    /* 7. The eligibility rule, which is the reason this field exists at all:
     *    "best" is the highest RANKABLE score, not the highest score.  A
     *    client that reimplemented this would sooner or later return the 99
     *    million from the run that ended mid-table. */
    {
        static const char *w[] = { "\"status\": \"verified\"",
                                   "\"score\": 99000000", /* still listed */
                                   "\"score\": 20652570", NULL };
        static const char *n[] = { "\"best\": null", NULL };
        good();
        attempt(1, 1, 99000000ULL, "unfinished", "no_clean_end", 0);
        attempt(2, 1, 20652570ULL, "attract",    "rankable",     1);
        attempt(3, 1,  5000000ULL, "attract",    "rankable",     1);
        verify_report();
        check("best is the best RANKABLE score", 0, w, n);
        /* and it is the 20.6M, not the 99M or the 5M */
        {
            const char *b = strstr(buf, "\"best\":");
            cases++;
            if(b && strstr(b, "\"index\": 2") && strstr(b, "\"score\": 20652570")){
                printf("  ok   best names attempt 2\n");
            } else {
                printf("  FAIL best names attempt 2\n  ---- %s\n", b ? b : "(no best)");
                fails++;
            }
        }
    }

    /* 8. A clean replay of a session nobody finished.  Verified - the file
     *    is exactly what was recorded - but there is no score to hand out.
     *    "Ein Lauf der mittendrin endet ist nicht abgeschlossen." */
    {
        static const char *w[] = { "\"status\": \"verified\"",
                                   "\"best\": null",
                                   "\"reason\": \"no_clean_end\"", NULL };
        good();
        attempt(1, 1, 8800000ULL, "unfinished", "no_clean_end", 0);
        verify_report();
        check("verified, nothing rankable in it", 0, w, NULL);
    }

    /* 9. No attempts at all is a result too - an empty attract-mode replay. */
    {
        static const char *w[] = { "\"status\": \"verified\"",
                                   "\"attempts\": []", "\"best\": null", NULL };
        good();
        verify_report();
        check("no attempts", 0, w, NULL);
    }

    /* 10. The ball counter went backwards, so a ball came back by a route
     *     the model does not know and every later boundary is suspect. */
    {
        static const char *w[] = { "\"warnings\": [\"ball_counter_rewound\"]",
                                   NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        g_rewound = 1;
        verify_report();
        check("ball counter rewound", 0, w, NULL);
    }

    /* 11. -verify on something that is not a replay. */
    {
        static const char *w[] = { "\"status\": \"refused\"",
                                   "\"build\": \"",
                                   "\"error\": \"not_a_replay\"", NULL };
        reset();
        verify_report();
        check("not a replay", 1, w, NULL);
    }

    /* 12. A refusal, as run.c's fail_msg() raises it. */
    {
        static const char *w[] = { "\"status\": \"refused\"",
                                   "\"error\": \"bad_replay_file\"",
                                   "\"message\": \"[replay] refused", NULL };
        reset();
        verify_code("bad_replay_file");
        verify_fail("[replay] refused: event 12 is stamped past the footer");
        check("refusal carries its code", 1, w, NULL);
    }

    /* 13. A refusal with no code set falls back to something valid. */
    {
        static const char *w[] = { "\"error\": \"refused\"", NULL };
        reset();
        verify_fail("something went wrong");
        check("refusal without a code", 1, w, NULL);
    }

    /* 14. Escaping.  A refusal message carries a Windows path, which is all
     *     backslashes, and an unescaped one makes the object unparseable at
     *     exactly the moment someone needs to read it. */
    {
        static const char *w[] = {
            "C:\\\\Users\\\\x\\\\a.pfr", "\\\"quoted\\\"", "\\n", NULL };
        reset();
        verify_fail("cannot open 'C:\\Users\\x\\a.pfr': \"quoted\"\nsecond line");
        check("json escaping", 1, w, NULL);
    }

    /* 15. The object is written once.  A second refusal after the verdict
     *     must not append a second object and make the file invalid. */
    {
        static const char *w[] = { "\"status\": \"verified\"", NULL };
        static const char *n[] = { "\"status\": \"refused\"", NULL };
        good();
        attempt(1, 1, 20652570ULL, "attract", "rankable", 1);
        verify_report();
        verify_fail("a late failure on the exit path");
        verify_report();
        check("written exactly once", 0, w, n);
    }

    /* 16. -verify arms -scoredbg, because a verdict with no score is not
     *     one and remembering a second flag is how the capture hash used to
     *     come out empty. */
    {
        cases++;
        scoredbg_on = 0;
        verify_arm(OUT);
        if(scoredbg_on){
            printf("  ok   -verify arms -scoredbg\n");
        } else {
            printf("  FAIL -verify arms -scoredbg\n");
            fails++;
        }
    }

    remove(OUT);
    printf("[verify] %d case(s), %d failure(s)\n", cases, fails);
    return fails ? 1 : 0;
}
