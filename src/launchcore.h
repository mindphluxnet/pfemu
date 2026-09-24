/* launchcore.h - what the launcher knows, without its windows.
 *
 * src/launch.c (Win32) and src/launch_gtk.c (GTK 3, Linux) are two faces on
 * the same rules: which replay may run against which installation, what the
 * Details report says, where a recording goes, what the option combo boxes
 * offer.  Those live in src/launchcore.c, once, so the two launchers cannot
 * drift apart on anything that decides whether a session is allowed.
 */
#ifndef PFEMU_LAUNCHCORE_H
#define PFEMU_LAUNCHCORE_H

#include "pfemu.h"

/* Combo box contents.  Index is the stored value throughout: the quality
 * notch, the start table (0 = the menu), each option byte. */
typedef struct { const char *label; const char *values[3]; int n; } OptDef;
extern const char *const table_labels[5];
extern const OptDef launch_opts[6];
extern const char *const quality_labels[5];
extern const char *const eq_labels[9];
extern const char *const oomph_labels[5];
int  eq_idx_to_db(int idx);
int  eq_db_to_idx(int db);
int  oomph_idx_to_db(int idx);
int  oomph_db_to_idx(int db);

/* Per-install settings the launcher reads one at a time (PFEMU-STATE/pfemu.cfg). */
int  read_trainer_cfg(const char *dir);
int  read_table_cfg(const char *dir);
void read_session_path(const char *dir, char *dst, size_t n);
void write_session_path(const char *dir, const char *p);

/* A fresh record target, sessions/<install>_<date>.pfr, never an existing
 * file. */
void launch_record_path(const char *dir, char *out, size_t n);

/* Replay against an installation (REPLAY.md 4.1).  same_vector() is the
 * identity compare; replay_check() is the whole decision, with the reason
 * in why when it refuses.  hdr_ok 0 means the file did not parse, and err
 * says why. */
int  same_vector(const ReplayHeader *h, const RelResult *r);
int  replay_check(int hdr_ok, const ReplayHeader *h, const char *err,
                  const RelResult *r, char *why, size_t n);

/* The Details report: plain text, \n line ends, monospace columns. */
#define DET_MAX 20480
void det_add(char *dst, size_t n, const char *fmt, ...);
void det_kv(char *dst, size_t n, const char *label, const char *fmt, ...);
void det_hex32(const uint8_t d[32], char out[65]);
void det_crlf(const char *src, char *dst, size_t n);
void details_report(char *dst, size_t n, LaunchMode mode, const char *replay_path,
                    int hdr_ok, const ReplayHeader *h, const char *err,
                    const RelResult *r);

/* Recordings and what they claim (<file>.pfr.games, written by src/run.c). */
#define RANKED_BALLS 3   /* pfemu-web's policy: a 5-ball game is another game */
int  read_claims(const char *pfr, long long best[5]);
void games_report(char *dst, size_t n, const char *pfr, int have_games);

/* Small text helpers. */
void fmt_score(long long v, char *out, size_t n);        /* 1,234,567 */
const char *base_name(const char *p);
const char *table_name(long long t);                     /* "Speed Devils" */

/* ------------------------------------------------------------ leaderboard
 * What the launchers make of pfemu-web's answers (pfemu-web/docs/API.md),
 * over src/online.c.  The requests themselves and every window are the
 * launchers' own. */
#include "online.h"

/* The sentence to show for a failed request. */
void online_message(const HttpResp *r, char *out, size_t n);
/* One submission object as the status line; *pending when it is not done. */
void sub_describe(const char *s, const char *e, char *out, size_t n, int *pending);
/* The newest submission in a GET /api/v1/submissions answer, described as
 * above.  0 when there is none. */
int  sub_latest(const HttpResp *r, long long *id, char *out, size_t n, int *pending);

/* The submissions table: one row per submission object.  sl_row() fills
 * the cells and returns how the row is coloured. */
enum { SL_PLAIN, SL_COUNTS, SL_PENDING };
#define SL_COLS 7
#define SL_CELL 200
extern const char *const sl_titles[SL_COLS];
int  sl_row(const char *p, const char *oe, char col[SL_COLS][SL_CELL]);

/* Before an upload: 1 to upload straight away, 0 to ask first, with the
 * question in box.  me is the answer to GET /api/v1/me. */
int  submit_check(const char *pfr, const HttpResp *me, char *box, size_t n);
/* A recording's bytes, exactly as on disk, for the upload.  0 and a
 * sentence in err when it cannot be sent. */
int  read_recording(const char *path, char **data, size_t *n, const char **err);
/* The JSON body of a login (reg 0) or register (reg 1) request.  0 when
 * user or password is empty. */
int  login_body(int reg, const char *user, const char *pass, const char *email,
                char *body, size_t n);

#endif
