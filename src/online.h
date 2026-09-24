/* online.h - the launchers' client for pfemu-web (src/online.c).
 *
 * Both launchers use it: WinHTTP and DPAPI on Windows, libcurl and the
 * desktop's keyring on Linux.  The contract is pfemu-web/docs/API.md;
 * nothing here is part of the emulator, and nothing the emulator does
 * depends on it.
 */
#ifndef PFEMU_ONLINE_H
#define PFEMU_ONLINE_H

#include <stddef.h>

#define ONLINE_DEFAULT_SERVER "https://pf.dark-secrets.eu"

/* pfemu-online.cfg, next to the program.  On Windows the token is stored
 * in it encrypted with DPAPI, so the file is useless on another machine or
 * to another Windows account; everything else in it is plain text.  On
 * Linux the username and token are in the desktop's keyring instead, and
 * the file holds only the server and Ranked. */
typedef struct {
    char server[256];
    char username[64];
    char token[160];     /* plaintext in memory only; "" when logged out */
    int  ranked;         /* Record mode's Ranked checkbox, default on */
} OnlineCfg;
void online_load(OnlineCfg *c);
/* 1 when the login is kept for the next start as well.  0 when it lasts
 * only as long as this launcher: no keyring, or the file cannot be written. */
int  online_save(const OnlineCfg *c);
/* Zero a password or token copy in a way the compiler may not drop. */
void secure_wipe(void *p, size_t n);

/* One HTTP exchange, synchronous: the launcher calls it from a worker
 * thread.  status is the HTTP status, or 0 when there was no response at
 * all, in which case err says why in a sentence for the player.  body is
 * malloc'd and NUL-terminated (never NULL after the call). */
typedef struct {
    int    status;
    char   err[200];
    char  *body;
    size_t len;
} HttpResp;
void online_http(const char *server, const char *method, const char *path,
                 const char *token, const char *ctype,
                 const void *data, size_t n, HttpResp *r);
void online_resp_free(HttpResp *r);

/* Just enough JSON for the answers in API.md.  Every lookup works on the
 * range [s, e) and finds a key at any depth, so the caller narrows the range
 * first with json_obj() where a key could be ambiguous. */
int  json_str(const char *s, const char *e, const char *key, char *out, size_t n);
int  json_num(const char *s, const char *e, const char *key, long long *out);
int  json_bool(const char *s, const char *e, const char *key, int *out);
int  json_obj(const char *s, const char *e, const char *key,
              const char **os, const char **oe);
/* The inside of the array under key, from just past its '[' to its end, for
 * json_next_obj() to walk. */
int  json_arr(const char *s, const char *e, const char *key,
              const char **as, const char **ae);
/* The next {...} in an array at or after p, or NULL at the end. */
const char *json_next_obj(const char *p, const char *e, const char **oe);
/* in as a JSON string body, without the quotes. */
void json_esc(const char *in, char *out, size_t n);

/* What to tell the player for a result's reason (the table in API.md). */
const char *online_reason_text(const char *reason);

#endif
