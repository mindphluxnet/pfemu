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

#endif
