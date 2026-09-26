/* The launcher's rules, shared by both launchers (see launchcore.h).
 *
 * Moved here from src/launch.c when the GTK launcher arrived.  Nothing in
 * this file opens a window; everything that decides whether a session may
 * start, and everything the Details report says, is here once.
 */
#include <stdarg.h>
#include "compat.h"
#include "launchcore.h"

/* ------------------------------------------------------ combo contents */
/* "Start at" choices.  Index is the program slot the boot loop EXECs,
 * so 0 really is "the menu" and 1-4 line up with Table1-4.Prg. */
const char *const table_labels[5] = {
    "Menu (normal start)",
    "Table 1 - Party Land",
    "Table 2 - Speed Devils",
    "Table 3 - Billion Dollar Gameshow",
    "Table 4 - Stones 'N Bones",
};

/* The six intro options, in PINBALL.CFG's byte order (src/launch.c, the
 * "launcher options I/O" comment, has the layout and how it was found). */
const OptDef launch_opts[6] = {
    { "Balls:",        {"3","5",NULL},                 2 },
    { "Angle:",        {"High","Low",NULL},             2 },
    { "Scrolling:",    {"Hard","Medium","Soft"},         3 },
    { "Ingame Music:", {"On","Off",NULL},                2 },
    { "Resolution:",   {"Normal","High",NULL},           2 },
    { "Color Mode:",   {"Color","Mono",NULL},            2 },
};

/* Quality notch labels.  SETSOUND offered these as five unlabelled steps
 * between "Low" and "High"; the rate is what the driver's table actually
 * selects for each (see cfg_sblaster in src/cfg.c), which is more use than the
 * original wording. */
const char *const quality_labels[5] = {
    "1 - 12000 Hz",
    "2 - 16000 Hz",
    "3 - 20000 Hz",
    "4 - 21000 Hz",
    "5 - 21000 Hz (extended mix)"
};

/* Enhancement combo maps: bass/treble in 3 dB steps, oomph off then up. */
const char *const eq_labels[9] = {
    "-12 dB", "-9 dB", "-6 dB", "-3 dB", "Flat",
    "+3 dB", "+6 dB", "+9 dB", "+12 dB"
};
const char *const oomph_labels[5] = {
    "Off", "+3 dB", "+6 dB", "+9 dB", "+12 dB"
};
int eq_idx_to_db(int idx){
    if(idx < 0) idx = 4;
    if(idx > 8) idx = 8;
    return (idx - 4) * 3;
}
int eq_db_to_idx(int db){
    int i = (db + 12 + 1) / 3;
    if(i < 0) i = 0;
    if(i > 8) i = 8;
    return i;
}
int oomph_idx_to_db(int idx){
    if(idx < 0) idx = 0;
    if(idx > 4) idx = 4;
    return idx * 3;
}
int oomph_db_to_idx(int db){
    int i = (db + 1) / 3;
    if(i < 0) i = 0;
    if(i > 4) i = 4;
    return i;
}

/* ------------------------------------------------- per-install settings */
/* Trainer cheats, ported from trainer/PINTRN.COM and trainer/TRAINER.EXE
 * (see src/fantasies.c for the reverse-engineering writeup and the actual
 * patch logic).  Its own `trainer` key rather than part of the six option
 * bytes above, since it is not part of PINBALL.CFG's layout - just a
 * starting state that src/fantasies.c applies the moment a table loads.
 * One checkbox in the UI ("Enable trainer") drives the lot: infinite balls,
 * ball control and infinite tilts are still three independent patches
 * underneath (and the '1'-'3' hotkeys still toggle them independently
 * in-game), but there is no reason to make the user tick three boxes to
 * turn "the trainer" on.  (Older builds stored two bytes for what were
 * once two checkboxes; src/cfg.c folds either of them being set into this
 * one flag when it imports such an install.) */
int read_trainer_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.trainer;
}

/* "Start at" per install.  Host-only, like everything else in pfemu.cfg:
 * the guest never sees it, and a release whose INT 65h layout cannot be
 * derived just falls back to the menu at boot (src/fantasies.c). */
int read_table_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.start_table;
}

/* Last-used session file per install (record target or replay source).
 * Restored into the dialog so a replay file doesn't have to be re-picked
 * every time; overwritten on each successful Launch in record/replay mode.
 * Display/hint only - replay identity still comes from the file header. */
void read_session_path(const char *dir, char *dst, size_t n){
    PfCfg c;
    cfg_read(dir, &c);
    snprintf(dst, n, "%s", c.session);
}

void write_session_path(const char *dir, const char *p){
    PfCfg c;
    if(!dir || !dir[0] || !p || !p[0]) return;
    cfg_read(dir, &c);
    snprintf(c.session, sizeof(c.session), "%s", p);
    cfg_write(dir, &c);
}

/* Default record target: sessions/<install>_<date>.pfr (REPLAY.md section
 * 4), in the working directory, with the host's separator.  The install dir
 * is sanitised: it is only ever a plain directory name, but never trust a
 * filename you did not build.
 *
 * Rebuilt on every entry into record mode, so the timestamp in the name is
 * the recording's own.  A second-resolution stamp can still repeat if
 * record mode is re-entered within the same second, so an existing file
 * gets _2, _3, ... rather than being overwritten: a recording is a
 * playthrough that cannot be reproduced, and one lost to a name clash is
 * gone for good. */
#ifdef _WIN32
#define SESSIONS_DIR "sessions\\"
#else
#define SESSIONS_DIR "sessions/"
#endif

void launch_record_path(const char *dir, char *out, size_t n){
    SYSTEMTIME t;
    char safe[64], stem[480];
    size_t i;
    int k;
    if(!dir) dir = "";
    GetLocalTime(&t);
    for(i=0;i<sizeof(safe)-1 && dir[i];i++){
        char c = dir[i];
        safe[i] = (c=='\\'||c=='/'||c==':'||c==' ') ? '_' : c;
    }
    safe[i] = 0;
    if(!safe[0]) snprintf(safe, sizeof(safe), "GAME");
    snprintf(stem, sizeof(stem),
             SESSIONS_DIR "%s_%04d%02d%02d_%02d%02d%02d",
             safe, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    snprintf(out, n, "%s.pfr", stem);
    for(k=2; k<100; k++){
        FILE *f = fopen(out, "rb");
        if(!f) break;
        fclose(f);
        snprintf(out, n, "%s_%d.pfr", stem, k);
    }
}

/* ------------------------------------------------------ replay decision */
/* Identity compare for auto-restore (REPLAY.md 4.1): same release id AND
 * same code hash vector.  Summaries, directory names and timestamps are
 * never identity. */
int same_vector(const ReplayHeader *h, const RelResult *r){
    int k;
    if(!r->rel || _stricmp(h->release_id, r->rel->id)) return 0;
    if(h->ncode != r->ncode) return 0;
    for(k=0;k<h->ncode;k++){
        if(_stricmp(h->names[k], r->code_names[k])) return 0;
        if(h->have[k] != r->code_have[k]) return 0;
        if(h->have[k] && (h->size[k] != r->code_size[k] ||
                           memcmp(h->sha[k], r->code_sha[k], 32))) return 0;
    }
    return 1;
}

/* Is r a valid replay target for the loaded file?  Fills why (when
 * non-NULL) for the detection line. */
int replay_check(int hdr_ok, const ReplayHeader *h, const char *err,
                 const RelResult *r, char *why, size_t n){
    if(!hdr_ok){
        if(why) snprintf(why, n, "%s", err ? err : "");
        return 0;
    }
    if(!h->trainer_off){
        if(why) snprintf(why, n, "Replay was recorded with the trainer on - refused.");
        return 0;
    }
    if(!r){
        if(why) snprintf(why, n, "No game found for this replay.");
        return 0;
    }
    if(!release_runnable(r)){
        if(why) snprintf(why, n, "%s: %s", release_state_name(r->state), r->summary);
        return 0;
    }
    if(!same_vector(h, r)){
        if(r->rel && !_stricmp(h->release_id, r->rel->id)){
            if(why) snprintf(why, n, "Same release (%s) but a different copy - replay needs"
                             " the recorded programs.", r->rel->id);
        } else {
            if(why) snprintf(why, n, "Replay is '%s', selected install is '%s' - refused.",
                             h->release_id, r->rel ? r->rel->id : "?");
        }
        return 0;
    }
    if(_stricmp(h->program, r->boot)){
        if(why) snprintf(why, n, "File starts at '%s', not this install's boot program"
                         " (direct-table replay is v2).", h->program);
        return 0;
    }
    if(read_trainer_cfg(r->dir)){
        if(why) snprintf(why, n, "Trainer is enabled for '%s' - replay needs it off.", r->dir);
        return 0;
    }
    return 1;
}

/* -------------------------------------------------------- details report
 * Details used to be a MessageBox (see src/launch.c, the details window, for
 * why it is not any more).  One report that states each thing once: the code
 * vector in particular appears exactly one time, as a recorded-vs-installed
 * comparison, which is the only form of it that answers the question the
 * user opened the window to ask.  The text stays plain and copyable: for a
 * release the database does not know, it is the intake format of
 * docs/RELEASES.md, sizes and hashes the user can send on without having run
 * anything. */
#define DET_RULE "----------------------------------------------------------------"

void det_add(char *dst, size_t n, const char *fmt, ...){
    va_list ap;
    size_t used = strlen(dst);
    if(used + 1 >= n) return;
    va_start(ap, fmt);
    vsnprintf(dst + used, n - used, fmt, ap);
    va_end(ap);
}

static void det_sect(char *dst, size_t n, const char *title){
    det_add(dst, n, "%s%s\n%s\n", dst[0] ? "\n" : "", title, DET_RULE);
}

/* One fact per line, label column fixed so the values line up. */
void det_kv(char *dst, size_t n, const char *label, const char *fmt, ...){
    char val[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(val, sizeof(val), fmt, ap);
    va_end(ap);
    det_add(dst, n, "  %-13s %s\n", label, val);
}

/* Someone else's report (release.c's), indented into this one unchanged -
 * its own alignment is already monospace and it is what -releases prints. */
static void det_body(char *dst, size_t n, const char *text){
    const char *p = text;
    while(*p){
        const char *nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        det_add(dst, n, "  %.*s\n", len, p);
        if(!nl) break;
        p = nl + 1;
    }
}

void det_hex32(const uint8_t d[32], char out[65]){
    static const char *hx = "0123456789abcdef";
    int i;
    for(i=0;i<32;i++){ out[i*2] = hx[d[i]>>4]; out[i*2+1] = hx[d[i]&15]; }
    out[64] = 0;
}

/* The six intro options as words rather than the raw bytes the .pfr keeps:
 * the labels are the launcher's own, minus their trailing colon.  Three to
 * a line - all six on one runs past the width the code vector below already
 * decides, and this is the one value here long enough to do that. */
static void det_options(const uint8_t o[6], char *dst, size_t n, int from, int to){
    int i;
    dst[0] = 0;
    for(i=from;i<to;i++){
        char lab[32];
        int v = o[i];
        size_t l;
        snprintf(lab, sizeof(lab), "%s", launch_opts[i].label);
        l = strlen(lab);
        if(l && lab[l-1] == ':') lab[l-1] = 0;
        snprintf(dst + strlen(dst), n - strlen(dst), "%s%s %s",
                 dst[0] ? ", " : "", lab,
                 (v >= 0 && v < launch_opts[i].n) ? launch_opts[i].values[v] : "?");
    }
}

/* Recorded identity against what is on disk now.  This is the one place any
 * hash is printed in replay mode; the second line only appears for a program
 * that actually differs, in release.c's expected/actual idiom. */
static void det_vector(char *dst, size_t n, const ReplayHeader *h,
                       const RelResult *r){
    int k;
    char hx[65];
    for(k=0;k<h->ncode;k++){
        int same;
        /* The leftmost column is the program name, or DIFFERS on the second
         * line of a pair - so a mismatch is visible down the left edge and
         * every line still ends at the same column. */
        if(h->have[k]){
            det_hex32(h->sha[k], hx);
            det_add(dst, n, "  %-11s %-9s %8u  %s\n",
                    h->names[k], "recorded", (unsigned)h->size[k], hx);
        } else {
            det_add(dst, n, "  %-11s %-9s %8s\n",
                    h->names[k], "recorded", "MISSING");
        }
        if(!r || k >= r->ncode || _stricmp(h->names[k], r->code_names[k])){
            det_add(dst, n, "  %-11s %-9s no program of this name\n",
                    "DIFFERS", "installed");
            continue;
        }
        same = (h->have[k] == r->code_have[k]) &&
               (!h->have[k] || (h->size[k] == r->code_size[k] &&
                                !memcmp(h->sha[k], r->code_sha[k], 32)));
        if(same) continue;
        if(r->code_have[k]){
            det_hex32(r->code_sha[k], hx);
            det_add(dst, n, "  %-11s %-9s %8u  %s\n",
                    "DIFFERS", "installed", (unsigned)r->code_size[k], hx);
        } else {
            det_add(dst, n, "  %-11s %-9s %8s\n",
                    "DIFFERS", "installed", "MISSING");
        }
    }
}

/* The whole report for whatever the launcher is currently showing. */
void details_report(char *dst, size_t n, LaunchMode mode, const char *replay_path,
                    int hdr_ok, const ReplayHeader *hd, const char *err,
                    const RelResult *r){
    dst[0] = 0;
    if(mode == LAUNCH_REPLAY){
        char why[256];
        int ok = replay_check(hdr_ok, hd, err, r, why, sizeof(why));
        det_sect(dst, n, "Status");
        det_add(dst, n, "  %s\n", ok ? "Ready to replay." : why);
        if(hdr_ok){
            const ReplayHeader *h = hd;
            char buf[512];
            det_sect(dst, n, "Replay file");
            det_kv(dst, n, "File", "%s", replay_path);
            det_kv(dst, n, "Recorded from", "%s - %s", h->release_id, h->summary);
            det_kv(dst, n, "Starts at", "%s, then %s", h->program,
                   (h->start_table >= 1 && h->start_table <= 4)
                   ? table_labels[h->start_table] : "the table menu");
            if(h->nevents > 0 && h->have_end)
                det_kv(dst, n, "Session", "%d events, %.1f s, %llu cycles",
                       h->nevents, h->end_emu,
                       (unsigned long long)h->end_cycles);
            else if(h->nevents > 0)
                det_kv(dst, n, "Session", "%d events", h->nevents);
            if(h->sound)
                det_kv(dst, n, "Sound", "on, quality %s",
                       (h->quality >= 0 && h->quality < 5)
                       ? quality_labels[h->quality] : "?");
            else
                det_kv(dst, n, "Sound", "off");
            det_options(h->options, buf, sizeof(buf), 0, 3);
            det_kv(dst, n, "Options", "%s", buf);
            det_options(h->options, buf, sizeof(buf), 3, 6);
            det_kv(dst, n, "", "%s", buf);
            det_kv(dst, n, "Display", "%s", h->fullscreen ? "fullscreen" : "windowed");
            snprintf(buf, sizeof(buf), "%.0f ips, speed %g", h->ips, h->speed);
            if(h->nopatch) snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), ", nopatch");
            det_kv(dst, n, "Emulation", "%s", buf);
            /* Only worth a line when it is the reason the file is refused:
             * a playable replay always recorded with the trainer off. */
            if(!h->trainer_off)
                det_kv(dst, n, "Trainer", "on when recorded - unplayable");
            if(h->overlay[0]) det_kv(dst, n, "Overlay", "%s", h->overlay);
            det_kv(dst, n, "State", "%s", h->state[0]
                   ? "canonical (ranked)" : "the player's own PFEMU-STATE");
            if(h->have_wav && strcmp(h->wav_hash, "none"))
                det_kv(dst, n, "Capture", "%s, %lu samples",
                       h->wav_hash, h->wav_samples);
            if(h->dir_hint[0]) det_kv(dst, n, "Recorded in", "%s", h->dir_hint);
            det_sect(dst, n, "Programs - recorded vs. this install");
            det_vector(dst, n, h, r);
        }
    }
    det_sect(dst, n, "Installation");
    if(r) det_body(dst, n, r->detail);
    else  det_add(dst, n, "  No installation is selected.\n");
}

/* Edit controls want CRLF; every report above is built with bare \n. */
void det_crlf(const char *src, char *dst, size_t n){
    size_t o = 0;
    for(; *src && o + 3 < n; src++){
        if(*src == '\n') dst[o++] = '\r';
        dst[o++] = *src;
    }
    dst[o] = 0;
}

/* ------------------------------------------------------------ recordings */
/* The best claimed three-ball rankable score per table 1-4, -1 where there
 * is none.  0 when the recording has no .games file. */
int read_claims(const char *pfr, long long best[5]){
    char path[600], line[128];
    FILE *f;
    int i;
    for(i = 0; i < 5; i++) best[i] = -1;
    snprintf(path, sizeof(path), "%s.games", pfr);
    f = fopen(path, "r");
    if(!f) return 0;
    while(fgets(line, sizeof(line), f)){
        int t, balls, rankable;
        unsigned long long sc;
        if(line[0] == '#') continue;
        if(sscanf(line, "%d %d %d %llu", &t, &balls, &rankable, &sc) != 4) continue;
        if(t < 1 || t > 4 || !rankable || balls != RANKED_BALLS) continue;
        if((long long)sc > best[t]) best[t] = (long long)sc;
    }
    fclose(f);
    return 1;
}

/* Every game the recording claims, not just the best per table.  This is
 * pfemu's own count on this machine; the server's is the leaderboard's. */
void games_report(char *dst, size_t n, const char *pfr, int have_games){
    char path[600], line[128];
    FILE *f;
    int any = 0;
    snprintf(path, sizeof(path), "%s.games", pfr);
    f = fopen(path, "r");
    while(f && fgets(line, sizeof(line), f)){
        int t, balls, rankable;
        unsigned long long g;
        char sc[32];
        if(line[0] == '#' || sscanf(line, "%d %d %d %llu", &t, &balls, &rankable, &g) != 4)
            continue;
        fmt_score((long long)g, sc, sizeof(sc));
        det_kv(dst, n, any ? "" : "Counted here", "%-18s %d balls %14s%s",
               table_name(t), balls, sc,
               !rankable ? "  not a finished one-player game" :
               balls != RANKED_BALLS ? "  only 3-ball games rank" : "");
        any = 1;
    }
    if(f) fclose(f);
    /* A recording from before the .games file gets one from its first
     * complete replay in the launcher (run.c). */
    if(!any) det_kv(dst, n, "Counted here", "%s", have_games
                    ? "no finished game"
                    : "not yet - replay it once to the end to count its games");
}

/* ------------------------------------------------------------------ text */
void fmt_score(long long v, char *out, size_t n){
    char d[32];
    int len, i;
    size_t k = 0;
    snprintf(d, sizeof(d), "%lld", v);
    len = (int)strlen(d);
    for(i = 0; i < len && k + 2 < n; i++){
        if(i && d[0] != '-' && (len - i) % 3 == 0) out[k++] = ',';
        out[k++] = d[i];
    }
    out[k] = 0;
}

const char *base_name(const char *p){
    const char *b = p, *s;
    for(s = p; *s; s++) if(*s == '\\' || *s == '/') b = s + 1;
    return b;
}

/* "Speed Devils" out of "Table 2 - Speed Devils". */
const char *table_name(long long t){
    return t >= 1 && t <= 4 ? table_labels[t] + 10 : "?";
}

/* ------------------------------------------------------------ leaderboard
 * Moved here from src/launch.c with the Linux launcher, like everything
 * above: what an answer from pfemu-web means is the same on both hosts. */

/* The sentence to show for a failed request: the server's own message when
 * it sent one (API.md: "an English sentence to show the player as is"),
 * else what went wrong on the way. */
void online_message(const HttpResp *r, char *out, size_t n){
    if(r->status == 0){
        snprintf(out, n, "%s", r->err[0] ? r->err : "No answer from the server.");
        return;
    }
    if(json_str(r->body, r->body + r->len, "message", out, n) && out[0]) return;
    snprintf(out, n, "The server answered with status %d.", r->status);
}

/* One submission object, as a line for the status row.  A result names the
 * pfemu build that produced it: the server verifies a kept recording again
 * when its build changes, so which build said "mismatch" is the part that
 * tells an old answer from a current one. */
void sub_describe(const char *s, const char *e, char *out, size_t n, int *pending){
    long long id = 0, score = 0, qp = 0;
    char state[24] = "", reason[96] = "", why[128], sc[32], build[40] = "", where[48] = "";
    char games[160] = "";
    const char *rs, *re, *as, *ae, *o, *oe;
    int rankable = 0, have_result;
    json_num(s, e, "id", &id);
    json_str(s, e, "state", state, sizeof(state));
    have_result = json_obj(s, e, "result", &rs, &re);
    if(strcmp(state, "done") || !have_result){
        *pending = 1;
        /* Everybody's uploads wait in one line, first come first served
         * (pfemu-web API.md), and 1 is the one being verified or next. */
        if(json_num(s, e, "queue_position", &qp) && qp > 1)
            snprintf(where, sizeof(where), ", position %lld in the queue", qp);
        /* A result while not done is the previous round's: this one is a
         * re-verification (a new pfemu build on the server). */
        snprintf(out, n, "Submission #%lld: %s%s...", id,
                 have_result ? "being verified again with the server's new build" :
                 !strcmp(state, "queued") || qp > 1 ? "queued for verification"
                                                    : "being verified", where);
        return;
    }
    *pending = 0;
    json_bool(rs, re, "rankable", &rankable);
    json_str(rs, re, "reason", reason, sizeof(reason));
    json_str(rs, re, "build", build, sizeof(build));
    /* A session on several tables ranks on each of them. */
    if(rankable && json_arr(rs, re, "games", &as, &ae)){
        size_t k = 0;
        for(o = as; (o = json_next_obj(o, ae, &oe)) != NULL && k < sizeof(games); o = oe){
            long long t = 0, g = 0;
            if(!json_num(o, oe, "table", &t) || !json_num(o, oe, "score", &g)) continue;
            fmt_score(g, sc, sizeof(sc));
            k += (size_t)snprintf(games + k, sizeof(games) - k, "%s%s on %s",
                                  k ? ", " : "", sc, table_name(t));
        }
    }
    if(rankable && games[0]){
        snprintf(out, n, "Submission #%lld verified and ranked: %s (build %s)", id, games,
                 build[0] ? build : "?");
    } else if(rankable && json_num(rs, re, "score", &score)){
        fmt_score(score, sc, sizeof(sc));
        snprintf(out, n, "Submission #%lld verified and ranked: %s points (build %s)", id, sc,
                 build[0] ? build : "?");
    } else {
        snprintf(out, n, "Submission #%lld: %s (build %s)", id,
                 online_reason_text(reason, why, sizeof(why)), build[0] ? build : "?");
    }
}

/* Newest first (API.md), so the first object is the one to show and, while
 * it is still running, to follow. */
int sub_latest(const HttpResp *r, long long *id, char *out, size_t n, int *pending){
    const char *s = r->body, *e = r->body + r->len, *v = NULL, *o, *oe;
    for(o = s; o < e; o++) if(*o == '['){ v = o + 1; break; }
    if(!v || (o = json_next_obj(v, e, &oe)) == NULL) return 0;
    *id = 0;
    json_num(o, oe, "id", id);
    sub_describe(o, oe, out, n, pending);
    return 1;
}

/* ---- the submissions table
 * One row per upload, newest first, the way GET /api/v1/submissions sends
 * them. */
const char *const sl_titles[SL_COLS] = {
    "#", "Received (UTC)", "State", "Score", "Result", "Verified (UTC)", "Build"
};

/* "2026-09-23T10:00:00+00:00" -> "2026-09-23 10:00". */
static void sl_time(char *t){
    if(strlen(t) < 16) return;
    if(t[10] == 'T') t[10] = ' ';
    t[16] = 0;
}

/* One submission object as the table's cells.  Returns the row's colour. */
int sl_row(const char *p, const char *oe, char col[SL_COLS][SL_CELL]){
    long long id = 0, qp = 0, score = 0;
    char state[24] = "", reason[96] = "", why[128], sc[32];
    const char *rs, *re, *as, *ae, *o, *ge;
    int rankable = 0, have_result, done, i;
    for(i = 0; i < SL_COLS; i++) col[i][0] = 0;
    json_num(p, oe, "id", &id);
    snprintf(col[0], SL_CELL, "%lld", id);
    json_str(p, oe, "received_at", col[1], SL_CELL);
    sl_time(col[1]);
    json_str(p, oe, "state", state, sizeof(state));
    done = !strcmp(state, "done");
    have_result = json_obj(p, oe, "result", &rs, &re);
    if(done) snprintf(col[2], SL_CELL, "done");
    else {
        /* The same words as the status line (sub_describe). */
        json_num(p, oe, "queue_position", &qp);
        snprintf(col[2], SL_CELL, "%s", have_result ? "verifying again" :
                 !strcmp(state, "queued") || qp > 1 ? "queued" : "verifying");
        if(qp > 1)
            snprintf(col[2] + strlen(col[2]), SL_CELL - strlen(col[2]),
                     " (position %lld)", qp);
    }
    if(!have_result) return SL_PENDING;
    json_bool(rs, re, "rankable", &rankable);
    json_str(rs, re, "reason", reason, sizeof(reason));
    json_str(rs, re, "finished_at", col[5], SL_CELL);
    sl_time(col[5]);
    json_str(rs, re, "build", col[6], SL_CELL);
    /* Every table the session ranked on, else the one-line summary. */
    if(rankable && json_arr(rs, re, "games", &as, &ae)){
        size_t k = 0;
        for(o = as; (o = json_next_obj(o, ae, &ge)) != NULL && k < SL_CELL; o = ge){
            long long t = 0, g = 0;
            if(!json_num(o, ge, "table", &t) || !json_num(o, ge, "score", &g)) continue;
            fmt_score(g, sc, sizeof(sc));
            k += (size_t)snprintf(col[3] + k, SL_CELL - k, "%s%s %s",
                                  k ? ", " : "", sc, table_name(t));
        }
    }
    if(!col[3][0] && json_num(rs, re, "score", &score) && score > 0)
        fmt_score(score, col[3], SL_CELL);
    /* Not done but with a result: the previous round's answer, shown as
     * such while the server verifies it again. */
    snprintf(col[4], SL_CELL, "%s%s", done ? "" : "previous: ",
             online_reason_text(reason, why, sizeof(why)));
    return !done ? SL_PENDING : rankable ? SL_COUNTS : SL_PLAIN;
}

/* ---- submitting
 * Before an upload: would it change any board?  The session's .games file
 * (src/fantasies.c) says what the recording claims, /api/v1/me where the
 * player stands.  This only ever asks.  The claim is not evidence and the
 * server decides; without the file or an answer the upload simply goes.
 * A tie does not beat a best, because on a board the earlier of two equal
 * scores stays. */
int submit_check(const char *pfr, const HttpResp *me, char *box, size_t n){
    const char *s = me->body, *e = me->body + me->len, *as, *ae, *o, *oe;
    long long claim[5], mine[5];
    char a[32], b[32];
    size_t k = 0;
    int t, any = 0, beats = 0;
    box[0] = 0;
    /* No answer, or no claims to hold against it: upload, and let the
     * server say what it thinks. */
    if(me->status != 200 || !read_claims(pfr, claim)) return 1;
    for(t = 0; t < 5; t++) mine[t] = -1;
    if(json_arr(s, e, "standings", &as, &ae))
        for(o = as; (o = json_next_obj(o, ae, &oe)) != NULL; o = oe){
            long long tb = 0, sc = 0;
            if(json_num(o, oe, "table", &tb) && json_num(o, oe, "score", &sc)
               && tb >= 1 && tb <= 4)
                mine[tb] = sc;
        }
    for(t = 1; t <= 4; t++){
        if(claim[t] < 0) continue;
        any = 1;
        if(claim[t] > mine[t]) beats = 1;
    }
    if(beats) return 1;
    if(!any)
        k += (size_t)snprintf(box + k, n - k,
                              "pfemu counted no finished three-ball game in this"
                              " recording, so it will not reach a board.\n");
    else {
        k += (size_t)snprintf(box + k, n - k,
                              "This recording does not beat your best on any table"
                              " it was played on:\n\n");
        for(t = 1; t <= 4 && k < n; t++){
            if(claim[t] < 0) continue;
            fmt_score(claim[t], a, sizeof(a));
            fmt_score(mine[t], b, sizeof(b));
            k += (size_t)snprintf(box + k, n - k, "%s: %s (your best: %s)\n",
                                  table_name(t), a, b);
        }
    }
    if(k < n)
        snprintf(box + k, n - k,
                 "\nEvery upload is verified in turn, so one that changes nothing"
                 " only makes the queue longer for everybody.\n\nSubmit anyway?");
    return 0;
}

/* The bytes exactly as pfemu wrote them: the file is hashed over its raw
 * disk bytes, so "rb" and nothing in between. */
int read_recording(const char *path, char **data, size_t *n, const char **err){
    FILE *f = fopen(path, "rb");
    long len;
    *data = NULL;
    *n = 0;
    if(!f){ *err = "The recording cannot be read."; return 0; }
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(len <= 0 || len > 16L * 1024 * 1024){
        fclose(f);
        *err = "The recording is empty or far too large to upload.";
        return 0;
    }
    *data = (char*)malloc((size_t)len);
    *n = *data ? fread(*data, 1, (size_t)len, f) : 0;
    fclose(f);
    if(!*data || *n != (size_t)len){
        free(*data);
        *data = NULL;
        *n = 0;
        *err = "The recording cannot be read.";
        return 0;
    }
    return 1;
}

int login_body(int reg, const char *user, const char *pass, const char *email,
               char *body, size_t n){
    char eu[200], ep[700], ee[700];
    if(!user[0] || !pass[0]) return 0;
    json_esc(user, eu, sizeof(eu));
    json_esc(pass, ep, sizeof(ep));
    if(reg && email && email[0]){
        json_esc(email, ee, sizeof(ee));
        snprintf(body, n, "{\"username\":\"%s\",\"password\":\"%s\",\"email\":\"%s\"}",
                 eu, ep, ee);
    } else if(reg){
        snprintf(body, n, "{\"username\":\"%s\",\"password\":\"%s\",\"email\":null}", eu, ep);
    } else {
        snprintf(body, n, "{\"username\":\"%s\",\"password\":\"%s\"}", eu, ep);
    }
    secure_wipe(ep, sizeof(ep));
    return 1;
}
