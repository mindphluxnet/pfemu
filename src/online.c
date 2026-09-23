/* online.c - the launcher's client for pfemu-web.
 *
 * The contract is pfemu-web/docs/API.md: register or log in, keep the
 * token, upload a .pfr, poll the submission, show the result.  This file is
 * the plumbing for that - configuration, the token store, one HTTP call and
 * just enough JSON to read the answers.  The launcher (src/launch.c) owns
 * every decision and every window; nothing in the emulator links this.
 *
 * WinHTTP rather than a library: it ships with Windows, does TLS with the
 * system's certificate store and proxy settings, and the whole client is
 * a handful of calls.  The launcher runs it on a worker thread, so a slow
 * or absent server never freezes the dialog.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pfemu.h"
#include "build.h"
#include "online.h"

/* Next to pfemu.exe, like pfemu-last.cfg and pfemu-winpos.cfg: global to
 * the machine's copy of pfemu, not to one installation - an account is not
 * a property of a game directory. */
#define ONLINE_FILE "pfemu-online.cfg"

/* Upper bound on a response body.  The largest answer in API.md is the
 * submission list; a megabyte is thousands of them. */
#define RESP_MAX (1u << 20)

static void cfg_path(char *out, size_t n){
    char exe[1024];
    DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    char *sep, *sep2;
    if(len == 0 || len >= sizeof(exe)){ snprintf(out, n, "%s", ONLINE_FILE); return; }
    sep = strrchr(exe, '\\');
    sep2 = strrchr(exe, '/');
    if(sep2 && (!sep || sep2 > sep)) sep = sep2;
    if(!sep){ snprintf(out, n, "%s", ONLINE_FILE); return; }
    sep[1] = 0;
    snprintf(out, n, "%s%s", exe, ONLINE_FILE);
}

/* ------------------------------------------------------------ token store
 *
 * The token is a year-long login, so it is not written in the clear.
 * DPAPI ties the ciphertext to this Windows account on this machine: a
 * copied config file carries no usable login.  It does not protect against
 * code already running as the same user, and nothing a launcher could do
 * would. */
static int token_protect(const char *plain, char *hex, size_t n){
    DATA_BLOB in, out;
    DWORD i;
    in.pbData = (BYTE*)plain;
    in.cbData = (DWORD)strlen(plain);
    if(!CryptProtectData(&in, L"pfemu", NULL, NULL, NULL,
                         CRYPTPROTECT_UI_FORBIDDEN, &out))
        return 0;
    if((size_t)out.cbData * 2 + 1 > n){ LocalFree(out.pbData); return 0; }
    for(i = 0; i < out.cbData; i++) snprintf(hex + 2*i, 3, "%02x", out.pbData[i]);
    hex[2*out.cbData] = 0;
    LocalFree(out.pbData);
    return 1;
}

static int hexval(int c){
    if(c >= '0' && c <= '9') return c - '0';
    if(c >= 'a' && c <= 'f') return c - 'a' + 10;
    if(c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int token_unprotect(const char *hex, char *plain, size_t n){
    static BYTE buf[2048];
    DATA_BLOB in, out;
    size_t len = strlen(hex), i;
    if(len % 2 || len / 2 > sizeof(buf)) return 0;
    for(i = 0; i < len / 2; i++){
        int hi = hexval(hex[2*i]), lo = hexval(hex[2*i+1]);
        if(hi < 0 || lo < 0) return 0;
        buf[i] = (BYTE)((hi << 4) | lo);
    }
    in.pbData = buf;
    in.cbData = (DWORD)(len / 2);
    if(!CryptUnprotectData(&in, NULL, NULL, NULL, NULL,
                           CRYPTPROTECT_UI_FORBIDDEN, &out))
        return 0;
    if(out.cbData + 1 > n){ LocalFree(out.pbData); return 0; }
    memcpy(plain, out.pbData, out.cbData);
    plain[out.cbData] = 0;
    LocalFree(out.pbData);
    return 1;
}

/* ---------------------------------------------------------------- config */
void online_load(OnlineCfg *c){
    char path[1100], line[2400];
    FILE *f;
    memset(c, 0, sizeof(*c));
    snprintf(c->server, sizeof(c->server), "%s", ONLINE_DEFAULT_SERVER);
    c->ranked = 1;
    cfg_path(path, sizeof(path));
    f = fopen(path, "r");
    if(!f) return;
    while(fgets(line, sizeof(line), f)){
        char *k = line, *v, *e;
        while(*k == ' ' || *k == '\t') k++;
        e = k + strlen(k);
        while(e > k && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
        if(!*k || *k == '#' || *k == ';') continue;
        v = strchr(k, '=');
        if(!v) continue;
        *v++ = 0;
        while(*v == ' ' || *v == '\t') v++;
        if(!strcmp(k, "server") && *v){
            size_t l;
            snprintf(c->server, sizeof(c->server), "%s", v);
            l = strlen(c->server);
            while(l && c->server[l-1] == '/') c->server[--l] = 0;
        }
        else if(!strcmp(k, "username")) snprintf(c->username, sizeof(c->username), "%s", v);
        else if(!strcmp(k, "ranked")) c->ranked = atoi(v) != 0;
        /* A token that does not decrypt (another machine, another Windows
         * account) reads as logged out: the player logs in again. */
        else if(!strcmp(k, "token")){
            if(!token_unprotect(v, c->token, sizeof(c->token))) c->token[0] = 0;
        }
    }
    fclose(f);
    if(!c->token[0]) c->username[0] = 0;
}

void online_save(const OnlineCfg *c){
    char path[1100];
    static char hex[4200];
    FILE *f;
    cfg_path(path, sizeof(path));
    f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "# pfemu launcher: leaderboard account and settings.\n");
    fprintf(f, "# The token is encrypted for this Windows account (DPAPI).\n");
    fprintf(f, "server=%s\n", c->server);
    fprintf(f, "ranked=%d\n", c->ranked ? 1 : 0);
    if(c->token[0] && token_protect(c->token, hex, sizeof(hex))){
        fprintf(f, "username=%s\n", c->username);
        fprintf(f, "token=%s\n", hex);
    }
    fclose(f);
}

/* ------------------------------------------------------------------ HTTP */
static void set_err(HttpResp *r, const char *what){
    DWORD e = GetLastError();
    const char *s = NULL;
    switch(e){
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
        s = "The server's name could not be resolved. Is the internet connection up?"; break;
    case ERROR_WINHTTP_CANNOT_CONNECT:
        s = "Could not connect to the server."; break;
    case ERROR_WINHTTP_TIMEOUT:
        s = "The server did not answer in time."; break;
    case ERROR_WINHTTP_SECURE_FAILURE:
        s = "The secure connection to the server failed."; break;
    case ERROR_WINHTTP_CONNECTION_ERROR:
        s = "The connection to the server was lost."; break;
    }
    if(s) snprintf(r->err, sizeof(r->err), "%s", s);
    else  snprintf(r->err, sizeof(r->err), "Network error %lu (%s).",
                   (unsigned long)e, what);
}

static int widen(const char *in, wchar_t *out, int n){
    return MultiByteToWideChar(CP_UTF8, 0, in, -1, out, n) > 0;
}

void online_http(const char *server, const char *method, const char *path,
                 const char *token, const char *ctype,
                 const void *data, size_t n, HttpResp *r){
    static const char ua[] = "pfemu-launcher/" PFEMU_BUILD;
    char full[800], hdr[600];
    wchar_t wurl[800], whost[256], wpath[512], wmethod[16], wua[128], whdr[600];
    URL_COMPONENTS uc;
    HINTERNET hs = NULL, hc = NULL, hr = NULL;
    DWORD code = 0, sz = sizeof(code);

    memset(r, 0, sizeof(*r));
    snprintf(full, sizeof(full), "%s%s", server, path);
    memset(&uc, 0, sizeof(uc));
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = whost;
    uc.dwHostNameLength = (DWORD)(sizeof(whost) / sizeof(whost[0]));
    uc.lpszUrlPath = wpath;
    uc.dwUrlPathLength = (DWORD)(sizeof(wpath) / sizeof(wpath[0]));
    if(!widen(full, wurl, 800) || !widen(method, wmethod, 16) || !widen(ua, wua, 128) ||
       !WinHttpCrackUrl(wurl, 0, 0, &uc) ||
       (uc.nScheme != INTERNET_SCHEME_HTTPS && uc.nScheme != INTERNET_SCHEME_HTTP)){
        snprintf(r->err, sizeof(r->err), "The server address '%s' is not a web address.", server);
        goto done;
    }
    hs = WinHttpOpen(wua, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                     WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if(!hs){ set_err(r, "open"); goto done; }
    /* Resolve, connect, send, receive.  The send allows for an upload over
     * a slow line; the others are generous for a server this size. */
    WinHttpSetTimeouts(hs, 15000, 15000, 60000, 30000);
    hc = WinHttpConnect(hs, whost, uc.nPort, 0);
    if(!hc){ set_err(r, "connect"); goto done; }
    hr = WinHttpOpenRequest(hc, wmethod, wpath, NULL, WINHTTP_NO_REFERER,
                            WINHTTP_DEFAULT_ACCEPT_TYPES,
                            uc.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0);
    if(!hr){ set_err(r, "request"); goto done; }
    {   int k = snprintf(hdr, sizeof(hdr), "Accept: application/json\r\n");
        if(token && token[0])
            k += snprintf(hdr + k, sizeof(hdr) - (size_t)k,
                          "Authorization: Bearer %s\r\n", token);
        if(ctype && ctype[0])
            snprintf(hdr + k, sizeof(hdr) - (size_t)k, "Content-Type: %s\r\n", ctype);
        if(!widen(hdr, whdr, 600) ||
           !WinHttpAddRequestHeaders(hr, whdr, (DWORD)-1L, WINHTTP_ADDREQ_FLAG_ADD)){
            set_err(r, "headers"); goto done;
        }
    }
    if(!WinHttpSendRequest(hr, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                           (LPVOID)data, (DWORD)n, (DWORD)n, 0)){
        set_err(r, "send"); goto done;
    }
    if(!WinHttpReceiveResponse(hr, NULL)){ set_err(r, "receive"); goto done; }
    if(!WinHttpQueryHeaders(hr, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz,
                            WINHTTP_NO_HEADER_INDEX)){
        set_err(r, "status"); goto done;
    }
    for(;;){
        DWORD avail = 0, got = 0;
        char *nb;
        if(!WinHttpQueryDataAvailable(hr, &avail)){ set_err(r, "read"); code = 0; break; }
        if(!avail) break;
        if(r->len + avail > RESP_MAX){
            snprintf(r->err, sizeof(r->err), "The server's answer was too large.");
            code = 0;
            break;
        }
        nb = (char*)realloc(r->body, r->len + avail + 1);
        if(!nb){ snprintf(r->err, sizeof(r->err), "Out of memory."); code = 0; break; }
        r->body = nb;
        if(!WinHttpReadData(hr, r->body + r->len, avail, &got)){
            set_err(r, "read"); code = 0; break;
        }
        r->len += got;
        r->body[r->len] = 0;
    }
    r->status = (int)code;
done:
    if(hr) WinHttpCloseHandle(hr);
    if(hc) WinHttpCloseHandle(hc);
    if(hs) WinHttpCloseHandle(hs);
    if(!r->body){
        r->body = (char*)calloc(1, 1);
        r->len = 0;
    }
}

void online_resp_free(HttpResp *r){
    free(r->body);
    r->body = NULL;
    r->len = 0;
}

/* ------------------------------------------------------------------ JSON
 *
 * Not a parser: a scanner that knows where strings begin and end, so a key
 * is only ever matched as a key (a quoted string followed by ':') and never
 * inside a value.  The answers are small, flat and come from our own
 * server; anything it cannot read reads as "not there". */
static int is_ws(int c){ return c==' ' || c=='\t' || c=='\r' || c=='\n'; }

static const char *skip_str(const char *p, const char *e){
    p++;
    while(p < e && *p != '"'){
        if(*p == '\\' && p + 1 < e) p++;
        p++;
    }
    return p < e ? p + 1 : e;
}

/* p at '{' or '['; returns one past its partner, or NULL. */
static const char *match_close(const char *p, const char *e){
    int d = 0;
    while(p < e){
        if(*p == '"'){ p = skip_str(p, e); continue; }
        if(*p == '{' || *p == '[') d++;
        else if(*p == '}' || *p == ']'){ if(--d == 0) return p + 1; }
        p++;
    }
    return NULL;
}

/* Just past `"key" :` and any space, or NULL. */
static const char *find_key(const char *s, const char *e, const char *key){
    size_t kl = strlen(key);
    const char *p = s;
    if(!s || !e) return NULL;
    while(p < e){
        if(*p == '"'){
            const char *q = skip_str(p, e), *v = q;
            while(v < e && is_ws(*v)) v++;
            if(v < e && *v == ':' && (size_t)(q - p) == kl + 2 && !memcmp(p + 1, key, kl)){
                v++;
                while(v < e && is_ws(*v)) v++;
                return v;
            }
            p = q;
        } else p++;
    }
    return NULL;
}

int json_str(const char *s, const char *e, const char *key, char *out, size_t n){
    const char *v = find_key(s, e, key);
    size_t k = 0;
    if(n) out[0] = 0;
    if(!v || v >= e || *v != '"' || !n) return 0;
    v++;
    while(v < e && *v != '"'){
        char c = *v++;
        if(c == '\\' && v < e){
            char x = *v++;
            switch(x){
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'r': c = '\r'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'u': {
                /* Plain ASCII passes; anything wider is shown as '?', which
                 * only a message string could ever contain. */
                unsigned u = 0;
                int i;
                for(i = 0; i < 4 && v < e; i++, v++){
                    int h = hexval(*v);
                    if(h < 0) break;
                    u = (u << 4) | (unsigned)h;
                }
                c = (u >= 0x20 && u < 0x7F) ? (char)u : '?';
                break; }
            default: c = x; break;
            }
        }
        if(k + 1 < n) out[k++] = c;
    }
    out[k] = 0;
    return 1;
}

int json_num(const char *s, const char *e, const char *key, long long *out){
    const char *v = find_key(s, e, key);
    char buf[32];
    size_t k = 0;
    if(!v) return 0;
    while(v < e && k + 1 < sizeof(buf) && (*v == '-' || (*v >= '0' && *v <= '9')))
        buf[k++] = *v++;
    buf[k] = 0;
    if(!k || !strcmp(buf, "-")) return 0;
    *out = _strtoi64(buf, NULL, 10);
    return 1;
}

int json_bool(const char *s, const char *e, const char *key, int *out){
    const char *v = find_key(s, e, key);
    if(!v) return 0;
    if(e - v >= 4 && !memcmp(v, "true", 4)){ *out = 1; return 1; }
    if(e - v >= 5 && !memcmp(v, "false", 5)){ *out = 0; return 1; }
    return 0;
}

int json_obj(const char *s, const char *e, const char *key,
             const char **os, const char **oe){
    const char *v = find_key(s, e, key), *c;
    if(!v || v >= e || *v != '{') return 0;
    c = match_close(v, e);
    if(!c) return 0;
    *os = v;
    *oe = c;
    return 1;
}

const char *json_next_obj(const char *p, const char *e, const char **oe){
    while(p && p < e){
        if(*p == ']') return NULL;
        if(*p == '{'){
            const char *c = match_close(p, e);
            if(!c) return NULL;
            *oe = c;
            return p;
        }
        if(*p == '"'){ p = skip_str(p, e); continue; }
        p++;
    }
    return NULL;
}

void json_esc(const char *in, char *out, size_t n){
    size_t k = 0;
    if(!n) return;
    for(; *in; in++){
        unsigned char c = (unsigned char)*in;
        char tmp[8];
        const char *add = tmp;
        if(c == '"') add = "\\\"";
        else if(c == '\\') add = "\\\\";
        else if(c < 0x20) snprintf(tmp, sizeof(tmp), "\\u%04x", c);
        else { tmp[0] = (char)c; tmp[1] = 0; }
        if(k + strlen(add) + 1 > n) break;
        memcpy(out + k, add, strlen(add));
        k += strlen(add);
    }
    out[k] = 0;
}

/* ------------------------------------------------------------ reason text
 * The table in pfemu-web/docs/API.md, in the words it asks the launcher to
 * use. */
const char *online_reason_text(const char *reason){
    static const struct { const char *code, *text; } t[] = {
        { "rankable", "It counts." },
        { "no_rankable_attempt", "No finished one-player game. The recording has to run"
                                 " until the game is back in attract mode." },
        { "balls_not_3", "Only 3-ball games rank." },
        { "release_not_ranked", "This release has no leaderboard." },
        { "mismatch", "The replay did not reproduce the recording." },
        { "refused", "The file was not accepted for replay." },
        { "timeout", "Our fault. It will be looked at; nothing to do." },
        { "crashed", "Our fault. It will be looked at; nothing to do." },
        { "protocol_error", "Our fault. It will be looked at; nothing to do." },
        { "internal_error", "Our fault. It will be looked at; nothing to do." },
        { "untrusted_build", "Our fault: server configuration." },
        { "unexpected_build", "Our fault: server configuration." },
        { "not_strict", "Our fault: server configuration." },
    };
    size_t i;
    if(!reason || !reason[0]) return "";
    for(i = 0; i < sizeof(t) / sizeof(t[0]); i++)
        if(!strcmp(reason, t[i].code)) return t[i].text;
    if(!strncmp(reason, "triage_refused", 14)) return "The file was not accepted for replay.";
    if(!strncmp(reason, "warning:", 8)) return "Held for a person to look at.";
    return "It does not count.";
}
