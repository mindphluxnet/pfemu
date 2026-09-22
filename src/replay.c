/* Session record / replay (docs/REPLAY.md).
 *
 * v1 records a session's keypresses with instruction-exact emulated time and
 * replays them on that same clock.  The file (.pfr) is plain text so a
 * session stays inspectable with any editor:
 *
 *   PFEMU-REPLAY 1
 *   release: floppy              # identity key ...
 *   code: INTRO.PRG 123456 abc.. # ... plus this hash vector (release.c)
 *   summary: ...                 # display only, never matched on
 *   dir_hint: GAME               # hint only, never identity
 *   boot: / program: / layout: / ips: / speed: / nopatch: / nolzexe:
 *   sound: / quality: / options: / fullscreen:  # the recorded session setup
 *   trainer_off: 1               # the invariant, not a setting
 *   date: / time:                # the frozen guest epoch (dos.c)
 *   overlay: ...                 # FNV-1a over PFEMU-STATE/ (drift detection)
 *   events:
 *   <cycles> <emu_time> <scancode> <down>   # sorted, one per kbd_key() entry
 *   end_emu: / end_cycles:                  # footer: the stop condition
 *
 * Events carry BOTH the absolute instruction count and the emulated time.
 * Injection keys off cycles (integer-exact: no rounding anywhere between
 * record and replay), while the time stays for readability.  A 1 us
 * rounding error is ~6 instructions or ~7 PIT counts - harmless almost
 * everywhere, but a guest RNG sampled from a fast counter is exactly the
 * place "almost" stops holding, so cycles close the gap completely.
 * Pre-cycle files (three-field lines, no count) still replay on emu_time.
 *
 * Volume is deliberately absent: it is host sink gain only (src/sound.c) and
 * stays live in every mode.  The footer carries a running FNV-1a over the
 * -wav capture bytes ("none" when no capture ran), so replay-twice checks
 * are self-contained; -shotevery frames stay external.  v1 records from
 * the boot program; anything else in program: stays refused.
 */
#include "compat.h"
#include "pfemu.h"

extern double emu_time;
extern double emu_ips;
extern double emu_inv_ips;

/* The frozen guest epoch (Tuesday 1993-06-15 12:00:00).  Jan 1 1993 was a
 * Friday, so Jun 1 was a Tuesday and Jun 15 too; DOS weekday 2 = Tuesday.
 * dos.c answers INT 21h AH=2Ah/2Ch and file timestamps from these when the
 * time freeze is on (REPLAY.md section 2.3). */
#define FROZEN_YEAR 1993
#define FROZEN_MON  6
#define FROZEN_DAY  15
#define FROZEN_WDAY 2
#define FROZEN_HOUR 12
#define FROZEN_MIN  0
#define FROZEN_SEC  0

void replay_frozen_datetime(int *year, int *mon, int *day, int *wday,
                            int *hour, int *min, int *sec){
    if(year) *year = FROZEN_YEAR;
    if(mon)  *mon  = FROZEN_MON;
    if(day)  *day  = FROZEN_DAY;
    if(wday) *wday = FROZEN_WDAY;
    if(hour) *hour = FROZEN_HOUR;
    if(min)  *min  = FROZEN_MIN;
    if(sec)  *sec  = FROZEN_SEC;
}

/* DOS packed date/time for FindFirst/Next while frozen. */
void replay_frozen_dos_dt(uint16_t *dosdate, uint16_t *dostime){
    if(dosdate) *dosdate = (uint16_t)(((FROZEN_YEAR-1980)<<9) | (FROZEN_MON<<5) | FROZEN_DAY);
    if(dostime) *dostime = (uint16_t)(FROZEN_HOUR<<11);
}

static int mode_record = 0, mode_replay = 0;
int replay_is_recording(void){ return mode_record; }
int replay_is_replaying(void){ return mode_replay; }

/* ------------------------------------------------------------- utilities */
static uint64_t fnv1a(const void *p, size_t n, uint64_t h){
    const uint8_t *b = (const uint8_t*)p;
    size_t i;
    for(i=0;i<n;i++){ h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

static char *lstrip(char *s){
    while(*s==' ' || *s=='\t' || *s=='\r' || *s=='\n') s++;
    return s;
}
static void rstrip(char *s){
    size_t n = strlen(s);
    while(n && (s[n-1]=='\r' || s[n-1]=='\n' || s[n-1]==' ' || s[n-1]=='\t')) s[--n] = 0;
}

/* Create every directory component of a file path (skips a drive prefix). */
static void mkdir_parents(const char *path){
    char tmp[600];
    size_t i, n;
    snprintf(tmp, sizeof(tmp), "%s", path);
    n = strlen(tmp);
    for(i=0;i<n;i++){
        if(tmp[i]=='/' || tmp[i]=='\\'){
            char save = tmp[i];
            tmp[i] = 0;
            if(i > 0 && tmp[i-1] != ':') _mkdir(tmp);
            tmp[i] = save;
        }
    }
}

static void hex32(const uint8_t d[32], char out[65]){
    static const char *x = "0123456789abcdef";
    int i;
    for(i=0;i<32;i++){ out[i*2] = x[d[i]>>4]; out[i*2+1] = x[d[i]&15]; }
    out[64] = 0;
}
static int unhex32(const char *s, uint8_t out[32]){
    int i;
    for(i=0;i<32;i++){
        int hi, lo, c;
        c = s[i*2];
        if(c>='0'&&c<='9') hi = c-'0';
        else if(c>='a'&&c<='f') hi = c-'a'+10;
        else if(c>='A'&&c<='F') hi = c-'A'+10;
        else return -1;
        c = s[i*2+1];
        if(c>='0'&&c<='9') lo = c-'0';
        else if(c>='a'&&c<='f') lo = c-'a'+10;
        else if(c>='A'&&c<='F') lo = c-'A'+10;
        else return -1;
        out[i] = (uint8_t)((hi<<4)|lo);
    }
    return 0;
}

/* The effective 6-byte options blob, for the record header (main.c): the
 * install's settings file, or the game's own defaults for an install whose
 * options page was never opened (src/cfg.c). */
void replay_read_options(const char *dir, uint8_t out[6]){
    PfCfg c;
    cfg_read(dir, &c);
    memcpy(out, c.options, 6);
}

/* ------------------------------------------------------- overlay hashing */
void replay_overlay_hash(const char *dir, char out[32]){
    char pat[600];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char names[256][64];
    int n = 0, i, j;
    uint64_t hh = 1469598103934665603ULL;
    snprintf(pat, sizeof(pat), "%s/PFEMU-STATE/*", dir);
    h = FindFirstFileA(pat, &fd);
    if(h != INVALID_HANDLE_VALUE){
        do {
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            if(strlen(fd.cFileName) >= sizeof(names[0])) continue;
            if(n < 256) snprintf(names[n++], sizeof(names[0]), "%s", fd.cFileName);
        } while(FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* Sort so the hash does not depend on filesystem return order. */
    for(i=0;i<n;i++) for(j=i+1;j<n;j++) if(strcmp(names[j], names[i]) < 0){
        char t[64];
        memcpy(t, names[i], sizeof(t));
        memcpy(names[i], names[j], sizeof(t));
        memcpy(names[j], t, sizeof(t));
    }
    hh = fnv1a(&n, sizeof(n), hh);
    for(i=0;i<n;i++){
        char path[700];
        FILE *f;
        static uint8_t buf[32768];
        size_t got;
        uint32_t sz = 0;
        hh = fnv1a(names[i], strlen(names[i])+1, hh);
        snprintf(path, sizeof(path), "%s/PFEMU-STATE/%s", dir, names[i]);
        f = fopen(path, "rb");
        if(!f){ hh = fnv1a("missing", 7, hh); continue; }
        while((got = fread(buf, 1, sizeof(buf), f)) > 0){
            sz += (uint32_t)got;
            hh = fnv1a(buf, got, hh);
        }
        fclose(f);
        hh = fnv1a(&sz, sizeof(sz), hh);
    }
    snprintf(out, 32, "%016llx", (unsigned long long)hh);
}

/* --------------------------------------------- overlay isolation (replay) */
static char iso_tmp[600] = "";

void replay_isolate_overlay(const char *dir){
    char base[600], pat[700];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    /* A sibling of the real overlay would still write into the user's game
     * directory, so this goes to the system temp area instead.  The real
     * overlay is only ever read after this point; every DOS write lands in
     * the copy (src/dos.c writedir remap below). */
    if(!GetTempPathA((DWORD)sizeof(base), base)) snprintf(base, sizeof(base), ".\\");
    snprintf(iso_tmp, sizeof(iso_tmp), "%spfemu_replay_%lu_%lu",
             base, (unsigned long)GetCurrentProcessId(), (unsigned long)GetTickCount());
    CreateDirectoryA(iso_tmp, NULL);
    snprintf(pat, sizeof(pat), "%s/PFEMU-STATE/*", dir);
    h = FindFirstFileA(pat, &fd);
    if(h != INVALID_HANDLE_VALUE){
        do {
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            {
                char src[700], dst[700];
                FILE *a, *b;
                static uint8_t buf[32768];
                size_t got;
                snprintf(src, sizeof(src), "%s/PFEMU-STATE/%s", dir, fd.cFileName);
                snprintf(dst, sizeof(dst), "%s/%s", iso_tmp, fd.cFileName);
                a = fopen(src, "rb");
                if(!a) continue;
                b = fopen(dst, "wb");
                if(!b){ fclose(a); continue; }
                while((got = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, got, b);
                fclose(a); fclose(b);
            }
        } while(FindNextFileA(h, &fd));
        FindClose(h);
    }
    dos_remap_writedir(iso_tmp);
    fprintf(stderr, "[replay] overlay isolated: '%s' is read-only, writes go to '%s'\n",
            dir, iso_tmp);
}

/* Where the isolated copy lives, or NULL when there is none.  -keepoverlay
 * in main.c reports it instead of deleting it; the guest's own writes from
 * the session are in there. */
const char *replay_overlay_path(void){ return iso_tmp[0] ? iso_tmp : NULL; }

void replay_cleanup_overlay(void){
    if(!iso_tmp[0]) return;
    {
        char pat[700];
        WIN32_FIND_DATAA fd;
        HANDLE h;
        snprintf(pat, sizeof(pat), "%s/*", iso_tmp);
        h = FindFirstFileA(pat, &fd);
        if(h != INVALID_HANDLE_VALUE){
            do {
                if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                {
                    char p[700];
                    snprintf(p, sizeof(p), "%s/%s", iso_tmp, fd.cFileName);
                    DeleteFileA(p);
                }
            } while(FindNextFileA(h, &fd));
            FindClose(h);
        }
    }
    if(!RemoveDirectoryA(iso_tmp))
        fprintf(stderr, "[replay] warning: could not remove temp overlay '%s'\n",
                iso_tmp);
    else
        iso_tmp[0] = 0;
}

/* ---------------------------------------------------------------- record */
static FILE *rec_fp = NULL;
static char rec_path[512] = "";
static int rec_events = 0;

int replay_begin_record(const char *path, const RelResult *rel, const char *prog,
                        double ips, int nopatch, int nolzexe,
                        int sound, int quality, const uint8_t options[6],
                        int fullscreen, int start_table){
    char ov[32];
    int i;
    if(fantasies_trainer_enabled()){
        fprintf(stderr, "[record] refused: the trainer is enabled for '%s'"
                        " (PFEMU-STATE/pfemu.cfg). Recording needs it off.\n",
                rel ? rel->dir : "?");
        return -1;
    }
    if(!rel || !rel->rel){
        fprintf(stderr, "[record] refused: '%s' is not a recognised release;"
                        " a replay needs identity (release + hash vector).\n",
                rel ? rel->dir : "?");
        return -1;
    }
    mkdir_parents(path);
    rec_fp = fopen(path, "w");
    if(!rec_fp){ fprintf(stderr, "[record] cannot write '%s'\n", path); return -1; }
    replay_overlay_hash(rel->dir, ov);
    snprintf(rec_path, sizeof(rec_path), "%s", path);
    rec_events = 0;
    fprintf(rec_fp, "PFEMU-REPLAY 1\n");
    fprintf(rec_fp, "release: %s\n", rel->rel->id);
    fprintf(rec_fp, "summary: %s\n", rel->summary);
    fprintf(rec_fp, "boot: %s\n", rel->boot[0] ? rel->boot : prog);
    fprintf(rec_fp, "program: %s\n", prog);
    fprintf(rec_fp, "layout: %s\n", rel->rel->layout ? rel->rel->layout->id : "full");
    /* Direct-to-table: 0 is a normal boot through the intro.  This has to
     * travel, because the guest event stream depends on it - a session
     * recorded at a table would replay from the menu and desync on the
     * first key.  program: stays the boot program either way, so the
     * cross-release refusal is unaffected. */
    fprintf(rec_fp, "start_table: %d\n", start_table);
    for(i=0;i<rel->ncode;i++){
        if(rel->code_have[i]){
            char hx[65];
            hex32(rel->code_sha[i], hx);
            fprintf(rec_fp, "code: %s %u %s\n",
                    rel->code_names[i], (unsigned)rel->code_size[i], hx);
        } else {
            fprintf(rec_fp, "code: %s MISSING\n", rel->code_names[i]);
        }
    }
    fprintf(rec_fp, "ips: %f\n", ips);
    /* speed is forced to 1 for the whole recording (main.c); the value here
     * is what replay enforces, so a future speed is a file edit away. */
    fprintf(rec_fp, "speed: 1\n");
    fprintf(rec_fp, "nopatch: %d\n", nopatch ? 1 : 0);
    fprintf(rec_fp, "nolzexe: %d\n", nolzexe ? 1 : 0);
    fprintf(rec_fp, "sound: %d\n", sound ? 1 : 0);
    fprintf(rec_fp, "quality: %d\n", quality);
    fprintf(rec_fp, "options: %02X %02X %02X %02X %02X %02X\n",
            options[0], options[1], options[2], options[3], options[4], options[5]);
    fprintf(rec_fp, "fullscreen: %d\n", fullscreen ? 1 : 0);
    /* The invariant, not a setting: a file that claims the trainer was on is
     * refused on replay, and recording with it on is refused above. */
    fprintf(rec_fp, "trainer_off: 1\n");
    fprintf(rec_fp, "date: %04d-%02d-%02d\n", FROZEN_YEAR, FROZEN_MON, FROZEN_DAY);
    fprintf(rec_fp, "time: %02d:%02d:%02d\n", FROZEN_HOUR, FROZEN_MIN, FROZEN_SEC);
    fprintf(rec_fp, "overlay: %s\n", ov);
    fprintf(rec_fp, "dir_hint: %s\n", rel->dir);
    fprintf(rec_fp, "events:\n");
    fflush(rec_fp);
    mode_record = 1;
    fprintf(stderr, "[record] writing '%s' (%s, %s, ips=%.0f)\n",
            path, rel->rel->id, prog, ips);
    return 0;
}

/* dev.c calls this for every kbd_key() entry, and for every synthesized
 * focus-loss break.  Overlap-repair re-asserts are deliberately NOT logged:
 * they are deterministic derived state, reproduced on replay from the same
 * primary events. */
void replay_log_key(int scancode, int down){
    if(!mode_record || !rec_fp) return;
    fprintf(rec_fp, "%llu %.6f %04X %d\n",
            (unsigned long long)cpu.cycles, emu_now(),
            scancode & 0xFFFF, down ? 1 : 0);
    rec_events++;
    fflush(rec_fp);
}

/* Integrity line: FNV-1a over every disk byte before it.  Catches
 * truncation (no footer + no hash) and edits alike; a mismatch refuses the
 * file instead of replaying something nobody recorded.  Hashed on disk
 * bytes ("rb"), not stdio text, so CRLF translation can't skew it. */
static void append_file_hash(const char *path){
    FILE *f = fopen(path, "rb");
    uint64_t h = 1469598103934665603ULL;
    if(f){
        static uint8_t buf[32768];
        size_t got;
        while((got = fread(buf, 1, sizeof(buf), f)) > 0)
            h = fnv1a(buf, got, h);
        fclose(f);
        f = fopen(path, "ab");
        if(f){
            fprintf(f, "file_hash: %016llx\n", (unsigned long long)h);
            fclose(f);
        }
    }
}

/* The matching check on load: re-read the disk bytes, hash everything up to
 * the file_hash: line itself, compare with the hex it carries.  1 ok,
 * 0 mismatch, -1 no hash line (legacy files predate it - warned about,
 * still played). */
static int verify_file_hash(const char *path){
    FILE *f = fopen(path, "rb");
    char *data = NULL;
    char fh[32] = "", got[32];
    long len = 0, cut = -1, i;
    uint64_t h;
    int r;
    if(!f) return -1;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(len <= 0 || len > (1<<24)){ fclose(f); return -1; }
    data = (char*)malloc((size_t)len + 1);
    if(!data){ fclose(f); return -1; }
    if(fread(data, 1, (size_t)len, f) != (size_t)len){ free(data); fclose(f); return -1; }
    fclose(f);
    data[len] = 0;
    /* The line must start at a line boundary (a forged body line merely
     * containing the text must not count). */
    for(i=0;i<len;i++){
        if((i == 0 || data[i-1] == '\n') &&
           !strncmp(data+i, "file_hash:", 10)){
            char *e = data+i+10;
            while(*e==' '||*e=='\t') e++;
            snprintf(fh, sizeof(fh), "%16.16s", e);
            cut = i;
            break;
        }
    }
    if(cut < 0){ free(data); return -1; }
    h = 1469598103934665603ULL;
    h = fnv1a(data, (size_t)cut, h);
    snprintf(got, sizeof(got), "%016llx", (unsigned long long)h);
    r = !strncmp(fh, got, 16) ? 1 : 0;
    free(data);
    return r;
}

void replay_end_record(void){
    if(!mode_record) return;
    mode_record = 0;
    if(rec_fp){
        char wh[17];
        unsigned long ws = 0;
        wav_current_hash(wh, &ws);
        fprintf(rec_fp, "end_emu: %.6f\n", emu_now());
        fprintf(rec_fp, "end_cycles: %llu\n", (unsigned long long)cpu.cycles);
        fprintf(rec_fp, "wav_hash: %s\n", wh);
        fprintf(rec_fp, "wav_samples: %lu\n", ws);
        fclose(rec_fp);
        rec_fp = NULL;
        append_file_hash(rec_path);
    }
    fprintf(stderr, "[record] '%s': %d events, end %.3fs / %llu cycles\n",
            rec_path, rec_events, emu_now(), (unsigned long long)cpu.cycles);
}

/* ---------------------------------------------------------------- replay */
/* have_cyc = 0 for pre-cycle files (three-field lines): those replay on
 * emu_time exactly as before. */
typedef struct { uint64_t cyc; int have_cyc; double t; int sc; int down; } Event;
static ReplayHeader rh;
static int rh_valid = 0;
static Event *ev = NULL;
static int nev = 0, ev_cap = 0, ev_idx = 0;
static double end_emu = -1.0;
static unsigned long long end_cycles = 0;
static char end_wav_hash[17] = "";
static unsigned long end_wav_samples = 0;
static int have_end_wav = 0;

static void header_defaults(ReplayHeader *o){
    memset(o, 0, sizeof(*o));
    o->ips = 6000000.0;
    o->speed = 1.0;
    memcpy(o->options, cfg_option_defaults, 6);
}

/* The last parse/load failure, in full sentences for fail_msg(): launcher
 * flows have no console, so "cannot replay 'x'" alone would say nothing. */
static char parse_err[512] = "";
const char *replay_parse_error(void){ return parse_err[0] ? parse_err : NULL; }

/* Which release the loaded file was recorded from, so a run that was not
 * told which install to use can pick the one that matches instead of the
 * first one on disk.  NULL until a file has parsed.  Identity is still the
 * hash vector - this only decides which install gets offered to
 * replay_verify_install(), which then checks everything properly. */
const char *replay_wanted_release(void){ return rh_valid ? rh.release_id : NULL; }
#define PARSE_FAIL(...) do { snprintf(parse_err, sizeof(parse_err), __VA_ARGS__); \
    fprintf(stderr, "%s\n", parse_err); fclose(f); return -1; } while(0)

static int parse_file(const char *path, ReplayHeader *h, int load_events){
    char line[1024];
    FILE *f = fopen(path, "r");
    int in_events = 0, lineno = 0, have_end_emu = 0, have_end_cycles = 0;
    parse_err[0] = 0;
    if(!f){
        snprintf(parse_err, sizeof(parse_err),
                 "[replay] cannot open '%s'", path);
        fprintf(stderr, "%s\n", parse_err);
        return -1;
    }
    if(!fgets(line, sizeof(line), f)){
        snprintf(parse_err, sizeof(parse_err),
                 "[replay] '%s' is empty", path);
        fprintf(stderr, "%s\n", parse_err);
        fclose(f);
        return -1;
    }
    rstrip(line);
    if(strcmp(line, "PFEMU-REPLAY 1")){
        PARSE_FAIL("[replay] '%s': bad magic (not a pfemu replay)", path);
    }
    header_defaults(h);
    while(fgets(line, sizeof(line), f)){
        char *s;
        lineno++;
        rstrip(line);
        s = lstrip(line);
        if(!*s) continue;
        if(!strcmp(s, "events:")){ in_events = 1; continue; }
        if(!in_events){
            if(!strncmp(s, "release:", 8)) snprintf(h->release_id, sizeof(h->release_id), "%s", lstrip(s+8));
            else if(!strncmp(s, "summary:", 8)) snprintf(h->summary, sizeof(h->summary), "%s", lstrip(s+8));
            else if(!strncmp(s, "boot:", 5)) snprintf(h->boot, sizeof(h->boot), "%s", lstrip(s+5));
            else if(!strncmp(s, "program:", 8)) snprintf(h->program, sizeof(h->program), "%s", lstrip(s+8));
            else if(!strncmp(s, "start_table:", 12)) h->start_table = atoi(lstrip(s+12));
            else if(!strncmp(s, "layout:", 7)) snprintf(h->layout, sizeof(h->layout), "%s", lstrip(s+7));
            else if(!strncmp(s, "code:", 5)){
                char nm[16], rest[128];
                if(h->ncode < 5 && sscanf(lstrip(s+5), "%15s %127[^\n]", nm, rest) == 2){
                    int k = h->ncode;
                    snprintf(h->names[k], sizeof(h->names[k]), "%s", nm);
                    if(!strcmp(rest, "MISSING")){
                        h->have[k] = 0;
                    } else {
                        unsigned sz;
                        char hx[65];
                        if(sscanf(rest, "%u %64s", &sz, hx) == 2 && unhex32(hx, h->sha[k]) == 0){
                            h->have[k] = 1;
                            h->size[k] = sz;
                        } else {
                            PARSE_FAIL("[replay] '%s' line %d: bad code vector", path, lineno);
                        }
                    }
                    h->ncode++;
                }
            }
            else if(!strncmp(s, "ips:", 4)) h->ips = atof(lstrip(s+4));
            else if(!strncmp(s, "speed:", 6)) h->speed = atof(lstrip(s+6));
            else if(!strncmp(s, "nopatch:", 8)) h->nopatch = atoi(lstrip(s+8)) != 0;
            else if(!strncmp(s, "nolzexe:", 8)) h->nolzexe = atoi(lstrip(s+8)) != 0;
            else if(!strncmp(s, "sound:", 6)) h->sound = atoi(lstrip(s+6)) != 0;
            else if(!strncmp(s, "quality:", 8)) h->quality = atoi(lstrip(s+8));
            else if(!strncmp(s, "options:", 8)){
                unsigned b[6];
                if(sscanf(lstrip(s+8), "%x %x %x %x %x %x",
                          &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6){
                    int k;
                    for(k=0;k<6;k++) h->options[k] = (uint8_t)b[k];
                }
            }
            else if(!strncmp(s, "fullscreen:", 11)) h->fullscreen = atoi(lstrip(s+11)) != 0;
            else if(!strncmp(s, "trainer_off:", 12)) h->trainer_off = atoi(lstrip(s+12)) != 0;
            else if(!strncmp(s, "overlay:", 8)) snprintf(h->overlay, sizeof(h->overlay), "%s", lstrip(s+8));
            else if(!strncmp(s, "dir_hint:", 9)) snprintf(h->dir_hint, sizeof(h->dir_hint), "%s", lstrip(s+9));
            /* date:/time: are informational (the epoch is fixed); nevents is
             * advisory.  Unknown lines are ignored for forward compatibility. */
        } else {
            if(!strncmp(s, "file_hash:", 10)){
                continue;   /* checked after the loop, over disk bytes */
            }
            if(!strncmp(s, "end_emu:", 8)){
                h->end_emu = atof(lstrip(s+8)); h->have_end = 1;
                if(load_events){ end_emu = h->end_emu; have_end_emu = 1; }
                continue;
            }
            else if(!strncmp(s, "end_cycles:", 11)){
                h->end_cycles = strtoull(lstrip(s+11), NULL, 10); h->have_end = 1;
                if(load_events){ end_cycles = h->end_cycles; have_end_cycles = 1; }
                continue;
            }
            else if(!strncmp(s, "wav_hash:", 9)){
                snprintf(h->wav_hash, sizeof(h->wav_hash), "%s", lstrip(s+9));
                h->have_wav = 1;
                if(load_events){
                    snprintf(end_wav_hash, sizeof(end_wav_hash), "%s", lstrip(s+9));
                    have_end_wav = 1;
                }
                continue;
            }
            else if(!strncmp(s, "wav_samples:", 12)){
                h->wav_samples = strtoul(lstrip(s+12), NULL, 10);
                if(load_events) end_wav_samples = h->wav_samples;
                continue;
            }
            if(!load_events){ h->nevents++; continue; }   /* display count only */
            {
                /* Cycle-exact ("<cycles> <time> <sc> <down>") or legacy
                 * ("<time> <sc> <down>"): the first token decides, since a
                 * count never contains a '.'.  %n rejects trailing garbage
                 * (it only runs when reached, so nn stays -1 on clean
                 * lines and the extra %s bumps the count on dirty ones). */
                char tok1[32];
                if(sscanf(s, "%31s", tok1) == 1 && !strchr(tok1, '.')){
                    unsigned long long cyc = 0;
                    double t = 0.0;
                    unsigned sc = 0, dn = 0;
                    char extra[16];
                    int nn = -1;
                    if(sscanf(s, "%llu %lf %x %u %15s%n", &cyc, &t, &sc, &dn,
                              extra, &nn) == 4 && nn < 0){
                        if(nev >= ev_cap){
                            int ncap = ev_cap ? ev_cap*2 : 1024;
                            Event *n = (Event*)realloc(ev, (size_t)ncap * sizeof(Event));
                            if(!n){ fclose(f); return -1; }
                            ev = n; ev_cap = ncap;
                        }
                        ev[nev].cyc = (uint64_t)cyc;
                        ev[nev].have_cyc = 1;
                        ev[nev].t = t;
                        ev[nev].sc = (int)(sc & 0xFFFF);
                        ev[nev].down = dn ? 1 : 0;
                        nev++;
                    } else if(s[0]){
                        PARSE_FAIL("[replay] '%s' line %d: bad event, stopping parse", path, lineno);
                    }
                } else {
                    double t = 0.0;
                    unsigned sc = 0, dn = 0;
                    char extra[16];
                    int nn = -1;
                    if(sscanf(s, "%lf %x %u %15s%n", &t, &sc, &dn,
                              extra, &nn) == 3 && nn < 0){
                        if(nev >= ev_cap){
                            int ncap = ev_cap ? ev_cap*2 : 1024;
                            Event *n = (Event*)realloc(ev, (size_t)ncap * sizeof(Event));
                            if(!n){ fclose(f); return -1; }
                            ev = n; ev_cap = ncap;
                        }
                        ev[nev].cyc = 0;
                        ev[nev].have_cyc = 0;
                        ev[nev].t = t;
                        ev[nev].sc = (int)(sc & 0xFFFF);
                        ev[nev].down = dn ? 1 : 0;
                        nev++;
                    } else if(s[0]){
                        PARSE_FAIL("[replay] '%s' line %d: bad event, stopping parse", path, lineno);
                    }
                    }
            }
        }
    }
    fclose(f);
    if(!h->release_id[0]){
        snprintf(parse_err, sizeof(parse_err),
                 "[replay] '%s': no release in header", path);
        fprintf(stderr, "%s\n", parse_err);
        return -1;
    }
    if(load_events){
        /* A complete record always ends in a footer.  Without one the file
         * was truncated (crash, copy, edit) - and without an end the
         * replay would run forever, so refuse it loudly instead. */
        if(!have_end_emu || !have_end_cycles){
            snprintf(parse_err, sizeof(parse_err),
                     "[replay] '%s': truncated file (no footer)", path);
            fprintf(stderr, "%s\n", parse_err);
            return -1;
        }
        switch(verify_file_hash(path)){
        case 0:
            snprintf(parse_err, sizeof(parse_err),
                     "[replay] '%s': integrity check failed (file changed)", path);
            fprintf(stderr, "%s\n", parse_err);
            return -1;
        case -1:
            /* Legacy files predate the hash line: warned about once here,
             * still played.  Anything WITH the line but wrong fails above. */
            fprintf(stderr, "[replay] '%s': no integrity line (legacy file)\n", path);
            break;
        default:
            break;
        }
    }
    h->nevents = load_events ? nev : h->nevents;
    return 0;
}

int replay_read_header(const char *path, ReplayHeader *out){
    return parse_file(path, out, 0);
}

int replay_begin_replay(const char *path){
    nev = 0; ev_idx = 0; end_emu = -1.0; end_cycles = 0;
    end_wav_hash[0] = 0; end_wav_samples = 0; have_end_wav = 0;
    if(parse_file(path, &rh, 1) != 0) return -1;
    rh_valid = 1;
    mode_replay = 1;
    fprintf(stderr, "[replay] '%s': %s, %d events, end %.3fs\n",
            path, rh.release_id, nev, end_emu);
    return 0;
}

/* Disarm whatever a refused attempt had already armed.  The launcher comes
 * back after a refusal (a replay whose install does not match, a record
 * target that will not open, a boot program that will not load), and the
 * next attempt has to start from a clean sheet: a file parsed but never
 * started would otherwise leave mode_replay set and inject the old
 * session's events into the following Play run.  The record file is
 * removed rather than left behind - fopen("w") already emptied whatever
 * was there, and a header with no events is not a replay, only a stub that
 * would sit in the picker's file list pretending to be one.  No footer and
 * no report: there was no session to close or count. */
void replay_abort(void){
    if(rec_fp){
        fclose(rec_fp);
        rec_fp = NULL;
        if(rec_path[0]) remove(rec_path);
    }
    rec_path[0] = 0;
    rec_events = 0;
    mode_record = 0;
    mode_replay = 0;
    rh_valid = 0;
    nev = 0; ev_idx = 0;
    end_emu = -1.0; end_cycles = 0;
    end_wav_hash[0] = 0; end_wav_samples = 0; have_end_wav = 0;
    /* The isolated overlay copy belongs to the attempt that made it. */
    replay_cleanup_overlay();
}

/* The install must BE the recording: same release id and same hash vector
 * (REPLAY.md section 4.1).  The directory name, boot filename spelling and
 * timestamps are explicitly not identity - but the release id and every
 * program hash are. */
int replay_verify_install(const RelResult *rel, const char *prog,
                           char *why, size_t nwhy){
    char cur[32];
    int k;
    (void)prog;
#define REFUSE(...) do { if(why) snprintf(why, nwhy, __VA_ARGS__); return -1; } while(0)
    if(!rh_valid) REFUSE("[replay] no replay file loaded.");
    if(!rel || !release_runnable(rel)){
        REFUSE("[replay] refused: '%s' is not a runnable install"
               " (%s: %s)",
               rel ? rel->dir : "?", rel ? release_state_name(rel->state) : "?",
               rel ? rel->summary : "?");
    }
    if(_stricmp(rh.release_id, rel->rel->id)){
        REFUSE("[replay] refused: file is '%s', install is '%s'."
               " Cross-release replay is refused by design.",
               rh.release_id, rel->rel->id);
    }
    if(rh.ncode != rel->ncode){
        REFUSE("[replay] refused: code vector length differs"
               " (file %d, install %d).", rh.ncode, rel->ncode);
    }
    for(k=0;k<rh.ncode;k++){
        if(_stricmp(rh.names[k], rel->code_names[k]) ||
           rh.have[k] != rel->code_have[k] ||
           (rh.have[k] && (rh.size[k] != rel->code_size[k] ||
                            memcmp(rh.sha[k], rel->code_sha[k], 32)))){
            REFUSE("[replay] refused: program '%s' differs from the recording"
                   " (file %s).", rh.names[k], rh.summary);
        }
    }
    /* v1 records from the boot program; a file naming a table program is a
     * direct-into-table replay, deferred to v2 (REPLAY.md section 3.4). */
    if(_stricmp(rh.program, rel->boot)){
        REFUSE("[replay] refused: file starts at '%s' but this install"
               " boots '%s'. Direct-into-table replay is deferred to v2.",
               rh.program, rel->boot);
    }
    /* Sound on/off decides whether the .SDR driver loads at all, so it is
     * as load-bearing as the quality notch.  The install must match the
     * file; replay writes nothing, so the message says which side to flip
     * (the launcher's Sound checkbox, in Play mode). */
    if(rh.sound != read_sound_is_sb(rel->dir)){
        REFUSE("[replay] refused: file was recorded with sound %s, but '%s'"
               " has it %s. Match the Sound checkbox to the recording first.",
               rh.sound ? "on" : "off", rel->dir,
               rh.sound ? "off" : "on");
    }
    if(!rh.trainer_off){
        REFUSE("[replay] refused: file was recorded with the trainer on.");
    }
    if(fantasies_trainer_enabled()){
        REFUSE("[replay] refused: the trainer is enabled for '%s'."
               " Replay needs it (and the file) off.", rel->dir);
    }
    replay_overlay_hash(rel->dir, cur);
    if(strcmp(cur, rh.overlay)){
        fprintf(stderr, "[replay] warning: PFEMU-STATE/ differs from the recording"
                        " (now %s, file %s). Same inputs + different overlay can diverge.\n",
                cur, rh.overlay);
    }
    return 0;
#undef REFUSE
}

void replay_apply_recorded_env(void){
    if(!rh_valid) return;
    if(rh.ips > 0.0){
        emu_ips = rh.ips;
        emu_inv_ips = 1.0 / emu_ips;
    }
    dos_no_patch = rh.nopatch ? 1 : 0;
    dos_no_lzexe = rh.nolzexe ? 1 : 0;
    dos_set_time_frozen(1);
    fprintf(stderr, "[replay] environment: ips=%.0f speed=%g nopatch=%d nolzexe=%d"
                    " quality=%d options=%02X%02X%02X%02X%02X%02X time frozen\n",
            emu_ips, rh.speed, dos_no_patch, dos_no_lzexe, rh.quality,
            rh.options[0], rh.options[1], rh.options[2],
            rh.options[3], rh.options[4], rh.options[5]);
}

/* The recorded session settings.  Quality and the options blob change what
 * the guest executes (the notch picks the driver's mixing rate and hence
 * its 386 budget; the blob is the game's own setup), so replay runs these
 * even when the install's current configs say something else - the
 * install's files are inputs, but the file is the session. */
int replay_recorded_fullscreen(void){
    return (rh_valid && rh.fullscreen) ? 1 : 0;
}
int replay_recorded_quality(int *have){
    if(have) *have = rh_valid;
    return rh.quality;
}
int replay_recorded_options(uint8_t out[6]){
    if(!rh_valid) return 0;
    memcpy(out, rh.options, 6);
    return 1;
}

/* Quality lives in SOUND.CFG byte 0x14, which the guest reads through DOS -
 * and on replay DOS reads the isolated temp copy.  Patch the recorded notch
 * in there (after replay_isolate_overlay), so the driver mixes exactly as
 * recorded.  A short config (NOSOUND.SDR) carries no notch and is left
 * alone: with no SB driver there is nothing to mix. */
void replay_apply_config_to_overlay(void){
    char path[700];
    FILE *f;
    uint8_t q;
    long len;
    int have;
    if(!mode_replay || !rh_valid || !iso_tmp[0]) return;
    q = (uint8_t)replay_recorded_quality(&have);
    if(!have || q > 4) return;
    snprintf(path, sizeof(path), "%s/SOUND.CFG", iso_tmp);
    f = fopen(path, "r+b");
    if(!f) return;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    if(len > 0x14){
        fseek(f, 0x14, SEEK_SET);
        fwrite(&q, 1, 1, f);
        fprintf(stderr, "[replay] sound quality %d restored into isolated overlay\n", q);
    }
    fclose(f);
}

int replay_forced_start_table(int *have){
    if(have) *have = rh_valid;
    return rh.start_table;
}

double replay_forced_ips(int *have){
    if(have) *have = rh_valid;
    return rh.ips;
}
double replay_forced_speed(int *have){
    if(have) *have = rh_valid;
    return rh.speed;
}

/* Cycle-exact injection (REPLAY.md section 2.1): events fire on the exact
 * recorded instruction, never on wall time, so host stutter cannot shift
 * what the guest sees - and a guest RNG sampled from a fast counter reads
 * the same value it did on record.  Legacy events without a count fall
 * back to emu_time. */
static int event_due(const Event *e){
    if(e->have_cyc) return e->cyc <= cpu.cycles;
    return e->t <= emu_now();
}

void replay_inject_due(void){
    if(!mode_replay || !rh_valid) return;
    while(ev_idx < nev && event_due(&ev[ev_idx])){
        kbd_key(ev[ev_idx].sc, ev[ev_idx].down);
        ev_idx++;
    }
}

/* cpu.cycles at which the next event is due (for batch clamping, the same
 * way IRQ0's deadline keeps timer precision).  Cycle-stamped events name
 * theirs directly; legacy ones convert from emu_time; once the list is out
 * the footer paces instead, so the stop lands within a batch rather than
 * a frame; ~0 when there is nothing left to pace. */
uint64_t replay_next_deadline(void){
    double now, dt;
    if(!mode_replay || !rh_valid) return ~(uint64_t)0;
    if(ev_idx < nev){
        if(ev[ev_idx].have_cyc)
            return ev[ev_idx].cyc <= cpu.cycles ? cpu.cycles : ev[ev_idx].cyc;
        now = emu_now();
        if(ev[ev_idx].t <= now) return cpu.cycles;
        dt = ev[ev_idx].t - now;
        return cpu.cycles + (uint64_t)(dt * emu_ips);
    }
    if(end_cycles > cpu.cycles) return end_cycles;
    if(end_emu >= 0.0){
        now = emu_now();
        if(end_emu > now) return cpu.cycles + (uint64_t)((end_emu - now) * emu_ips);
    }
    return ~(uint64_t)0;
}

int replay_events_pending(void){
    return mode_replay && rh_valid && ev_idx < nev;
}

/* End condition (REPLAY.md section 3.3): the footer's cycle count, events
 * out - or ScrollLock / window close, which the main loop handles as today.
 * The emu_time footer stays as the stop for legacy (count-less) files. */
int replay_should_stop(void){
    int out;
    if(!mode_replay || !rh_valid || ev_idx < nev) return 0;
    out = (end_cycles > 0 && cpu.cycles >= end_cycles) ||
          (end_emu >= 0.0 && emu_now() >= end_emu);
    return out;
}

void replay_report(void){
    if(mode_record || rec_fp){
        /* Ended without going through replay_end_record (should not happen;
         * keep the file valid). */
        replay_end_record();
        return;
    }
    if(!mode_replay || !rh_valid) return;
    fprintf(stderr, "[replay] done: %d/%d events injected\n", ev_idx, nev);
    fprintf(stderr, "[replay] recorded end %.6fs / %llu cycles, actual %.6fs / %llu cycles\n",
            end_emu, end_cycles, emu_now(), (unsigned long long)cpu.cycles);
    /* Self-contained audio check: the footer's running hash over the -wav
     * capture bytes (upstream of the host gain, so volume never moves it).
     * A mismatch warns only - audio-sink differences must never invalidate
     * the input replay itself. */
    {
        char wh[17];
        unsigned long ws = 0;
        wav_current_hash(wh, &ws);
        if(have_end_wav && !strcmp(end_wav_hash, "none") && !strcmp(wh, "none")){
            fprintf(stderr, "[replay] wav: no capture on either run\n");
        } else if(have_end_wav && !strcmp(end_wav_hash, "none")){
            fprintf(stderr, "[replay] wav: recorded none, actual %s (%lu samples)"
                            " (capture ran only now)\n", wh, ws);
        } else if(!strcmp(wh, "none")){
            if(have_end_wav)
                fprintf(stderr, "[replay] wav: recorded %s (%lu samples), no capture"
                                " this run (replay without -wav)\n",
                        end_wav_hash, end_wav_samples);
            else
                fprintf(stderr, "[replay] wav: no capture this run"
                                " (legacy file, no recorded hash)\n");
        } else if(have_end_wav && !strcmp(end_wav_hash, wh)){
            fprintf(stderr, "[replay] wav hash MATCH: %s (%lu samples)\n", wh, ws);
        } else if(have_end_wav){
            fprintf(stderr, "[replay] wav hash MISMATCH: recorded %s (%lu samples),"
                            " actual %s (%lu samples) - nondeterminism bug\n",
                    end_wav_hash, end_wav_samples, wh, ws);
        } else {
            fprintf(stderr, "[replay] wav: actual %s (%lu samples)"
                            " (legacy file, no recorded hash)\n", wh, ws);
        }
    }
    /* "Accurate" is defined by artifacts (REPLAY.md section 5): matching
     * footer wav hashes + -shotevery frames + the two numbers above.
     * Any mismatch is a nondeterminism bug. */
}
