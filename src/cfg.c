/* One settings file per install: PFEMU-STATE/pfemu.cfg.
 *
 * Everything the launcher remembers - volume, sound quality notch, the six
 * PINBALL.CFG option bytes, the trainer switch, fullscreen, "Start at" and
 * the last session file - used to live in six little binary files
 * (pfemu_audio.cfg, pfemu_options.cfg, pfemu_cheats.cfg, pfemu_display.cfg,
 * pfemu_table.cfg, pfemu_session.cfg).  They are all host-only - the guest
 * never opens any of them (src/fantasies.c pokes the option bytes into
 * INTRO.PRG's own buffer instead; see the big comment in src/launch.c) - so
 * nothing about their layout was ever forced by the game.  One key=value
 * text file is easier to read, easier to hand-edit when something needs
 * debugging, and does not grow a new file per setting.
 *
 * Old installs are picked up automatically: when pfemu.cfg is missing,
 * cfg_read() imports whatever the six legacy files hold, and the next
 * cfg_write() lays down pfemu.cfg and deletes them.  A read on its own never
 * writes, which is what keeps replay's promise never to touch the user's
 * real PFEMU-STATE/ (docs/REPLAY.md section 3.3) intact.
 *
 * Unknown keys in the file are dropped on the next write, and an unreadable
 * or partial file just reads as defaults - these are preferences, and losing
 * one is a smaller problem than refusing to start over it.
 */
#include "compat.h"
#include "pfemu.h"

#define CFG_NAME "pfemu.cfg"

/* Also the game's own hardcoded default: a fresh install with no PINBALL.CFG
 * boots with the intro's option buffer already at 00 00 01 00 00 00. */
const uint8_t cfg_option_defaults[6] = {0,0,1,0,0,0};

/* The six option bytes, in PINBALL.CFG's own order (src/launch.c documents
 * what each value means).  One key each rather than one opaque blob: the
 * file is text now, so it may as well say what it is setting. */
static const char *option_keys[6] = {
    "balls", "angle", "scrolling", "music", "resolution", "color"
};

static const char *legacy_names[6] = {
    "pfemu_audio.cfg", "pfemu_options.cfg", "pfemu_cheats.cfg",
    "pfemu_display.cfg", "pfemu_table.cfg", "pfemu_session.cfg"
};

static void cfg_defaults(PfCfg *c){
    memset(c, 0, sizeof(*c));
    c->volume = AUDIO_VOLUME_DEFAULT;
    c->bass = AUDIO_BASS_DEFAULT;
    c->treble = AUDIO_TREBLE_DEFAULT;
    c->oomph = AUDIO_OOMPH_DEFAULT;
    c->headphone = 0;
    c->quality = 0;
    memcpy(c->options, cfg_option_defaults, 6);
}

static void state_path(const char *dir, const char *name, char *out, size_t n){
    snprintf(out, n, "%s/PFEMU-STATE/%s", dir ? dir : "", name);
}

/* ------------------------------------------------------- legacy import --
 *
 * Exactly the reads the old per-file helpers did, including their tolerance
 * for a short file.  Only reached once per install, and only until the
 * first write replaces them. */
static void import_legacy(const char *dir, PfCfg *c){
    char path[600];
    FILE *f;
    uint8_t b[6];
    size_t n;
    int v;

    state_path(dir, legacy_names[0], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 2, f);
        fclose(f);
        if(n >= 1 && b[0] <= 100) c->volume = b[0];
        if(n >= 2 && b[1] <= 4) c->quality = b[1];
    }
    state_path(dir, legacy_names[1], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 6, f);
        fclose(f);
        if(n == 6) memcpy(c->options, b, 6);
    }
    /* Two bytes, once two separate checkboxes: either one on means the
     * trainer is on (src/fantasies.c load_cheat_cfg said the same). */
    state_path(dir, legacy_names[2], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 2, f);
        fclose(f);
        if(n == 2) c->trainer = (b[0] != 0) || (b[1] != 0);
    }
    state_path(dir, legacy_names[3], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        n = fread(b, 1, 1, f);
        fclose(f);
        if(n == 1) c->fullscreen = b[0] != 0;
    }
    state_path(dir, legacy_names[4], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        if(fscanf(f, "%d", &v) == 1 && v >= 1 && v <= 4) c->start_table = v;
        fclose(f);
    }
    state_path(dir, legacy_names[5], path, sizeof(path));
    if((f = fopen(path, "rb")) != NULL){
        if(fgets(c->session, (int)sizeof(c->session), f))
            c->session[strcspn(c->session, "\r\n")] = 0;
        else
            c->session[0] = 0;
        fclose(f);
    }
}

static void drop_legacy(const char *dir){
    char path[600];
    int i;
    for(i=0;i<6;i++){
        state_path(dir, legacy_names[i], path, sizeof(path));
        DeleteFileA(path);
    }
}

/* ------------------------------------------------------------- parsing -- */
static char *trim(char *s){
    char *e;
    while(*s==' '||*s=='\t') s++;
    e = s + strlen(s);
    while(e > s && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    return s;
}

static void apply_pair(PfCfg *c, const char *key, const char *val){
    int i, v = atoi(val);
    if(!strcmp(key, "volume")){ if(v >= 0 && v <= 100) c->volume = v; return; }
    if(!strcmp(key, "bass")){ if(v >= -12 && v <= 12) c->bass = v; return; }
    if(!strcmp(key, "treble")){ if(v >= -12 && v <= 12) c->treble = v; return; }
    if(!strcmp(key, "oomph")){ if(v >= 0 && v <= 12) c->oomph = v; return; }
    if(!strcmp(key, "headphone")){ c->headphone = v != 0; return; }
    if(!strcmp(key, "quality")){ if(v >= 0 && v <= 4) c->quality = v; return; }
    if(!strcmp(key, "trainer")){ c->trainer = v != 0; return; }
    if(!strcmp(key, "fullscreen")){ c->fullscreen = v != 0; return; }
    if(!strcmp(key, "start_table")){ if(v >= 0 && v <= 4) c->start_table = v; return; }
    if(!strcmp(key, "session")){
        snprintf(c->session, sizeof(c->session), "%s", val);
        return;
    }
    for(i=0;i<6;i++)
        if(!strcmp(key, option_keys[i])){
            /* Range is per option (2 or 3 choices), but the intro's own
             * buffer is a byte and out-of-range values are the launcher's
             * problem to clamp, not this parser's. */
            if(v >= 0 && v <= 255) c->options[i] = (uint8_t)v;
            return;
        }
}

void cfg_read(const char *dir, PfCfg *c){
    char path[600], line[700];
    FILE *f;
    cfg_defaults(c);
    if(!dir || !dir[0]) return;
    state_path(dir, CFG_NAME, path, sizeof(path));
    f = fopen(path, "rb");
    if(!f){ import_legacy(dir, c); return; }
    while(fgets(line, sizeof(line), f)){
        char *k, *eq;
        k = trim(line);
        if(!*k || *k=='#' || *k==';') continue;
        eq = strchr(k, '=');
        if(!eq) continue;
        *eq = 0;
        apply_pair(c, trim(k), trim(eq+1));
    }
    fclose(f);
}

void cfg_write(const char *dir, const PfCfg *c){
    char path[600], sub[600];
    FILE *f;
    int i, vol, qual, tbl;
    if(!dir || !dir[0]) return;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    state_path(dir, CFG_NAME, path, sizeof(path));
    f = fopen(path, "wb");
    if(!f) return;
    vol = c->volume < 0 ? 0 : (c->volume > 100 ? 100 : c->volume);
    qual = c->quality < 0 ? 0 : (c->quality > 4 ? 4 : c->quality);
    tbl = (c->start_table >= 1 && c->start_table <= 4) ? c->start_table : 0;
    fprintf(f, "# pfemu settings for this install - written by the launcher.\n"
               "# The game never reads this file; delete it to start over.\n");
    fprintf(f, "volume=%d\n", vol);
    { int b = c->bass < -12 ? -12 : (c->bass > 12 ? 12 : c->bass);
      int t = c->treble < -12 ? -12 : (c->treble > 12 ? 12 : c->treble);
      int o = c->oomph < 0 ? 0 : (c->oomph > 12 ? 12 : c->oomph);
      fprintf(f, "bass=%d\n", b);
      fprintf(f, "treble=%d\n", t);
      fprintf(f, "oomph=%d\n", o);
      fprintf(f, "headphone=%d\n", c->headphone ? 1 : 0); }
    fprintf(f, "quality=%d\n", qual);
    for(i=0;i<6;i++) fprintf(f, "%s=%d\n", option_keys[i], c->options[i]);
    fprintf(f, "trainer=%d\n", c->trainer ? 1 : 0);
    fprintf(f, "fullscreen=%d\n", c->fullscreen ? 1 : 0);
    fprintf(f, "start_table=%d\n", tbl);
    fprintf(f, "session=%s\n", c->session);
    fclose(f);
    drop_legacy(dir);
}

/* ----------------------------------------------- effective-setting readers
 *
 * These four used to live in src/launch.c, purely because the launcher was
 * their first caller.  Nothing in them is a dialog: they are the same
 * settings this file already owns, read the way the rest of the program
 * needs them - the effective value, which for sound means the overlay's
 * SOUND.CFG before the installed one before the mirror kept here.
 *
 * src/run.c calls all four on every run, headless included, so leaving them
 * in the Win32-only launcher meant a build without a GUI could not read its
 * own configuration.  They are plain stdio and belong here.
 */
/* 1 when the effective config (overlay first, then installed file) selects
 * a SoundBlaster-family driver; used for the checkbox initial state. */
int read_sound_is_sb(const char *dir){
    FILE *f;
    char name[13];
    int i;
    const char *cands[2];
    static char ov[600], orig[600];
    snprintf(ov, sizeof(ov), "%s/PFEMU-STATE/SOUND.CFG", dir);
    snprintf(orig, sizeof(orig), "%s/SOUND.CFG", dir);
    cands[0]=ov; cands[1]=orig;
    for(i=0;i<2;i++){
        f = fopen(cands[i], "rb");
        if(!f) continue;
        memset(name, 0, sizeof(name));
        fread(name, 1, 12, f);
        fclose(f);
        return strncmp(name, "SBLASTER", 8)==0 || strncmp(name, "SBPRO", 5)==0 ||
               strncmp(name, "SB16", 4)==0 || strncmp(name, "SB20", 4)==0;
    }
    return 0;
}

/* ------------------------------------------------------------- audio I/O
 *
 * Two of the settings in the install's PFEMU-STATE/pfemu.cfg (src/cfg.c):
 * volume 0-100 and the quality notch 0-4.
 *
 * Volume is host-only by nature.  The guest has nothing to configure: the
 * driver mixes at full scale, and the card's mixer registers are an SB Pro
 * feature this DSP-1.05 card does not have - so the setting is a property of
 * the host sink alone (see the comment over audio_volume in src/sound.c).
 *
 * Quality *is* a guest setting - it goes in SOUND.CFG byte 0x14, where the
 * driver reads it - but it is mirrored here so that turning sound off and on
 * again doesn't silently reset it: a NOSOUND.SDR config is 16 bytes long and
 * has no byte 0x14 to remember it in.  SOUND.CFG still wins when it has one,
 * so running the real SETSOUND (-setup) is still picked up here. */
int read_volume_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.volume;
}

/* Volume alone, for the in-window -/+ keys on their way out (src/main.c).
 * Reads the whole file first so every other setting survives: the game
 * window can change none of them, and clobbering one would silently undo
 * the launcher. */
void write_volume_cfg(const char *dir, int vol){
    PfCfg c;
    cfg_read(dir, &c);
    c.volume = vol;
    cfg_write(dir, &c);
}

/* Quality notch actually in force: the effective SOUND.CFG when it is an SB
 * config long enough to carry one (overlay first, then installed, same order
 * as read_sound_is_sb), otherwise the mirror above. */
int read_sound_quality(const char *dir){
    FILE *f;
    uint8_t buf[25];
    size_t n;
    int i;
    char path[600];
    PfCfg c;
    for(i=0;i<2;i++){
        snprintf(path, sizeof(path), i==0 ? "%s/PFEMU-STATE/SOUND.CFG" : "%s/SOUND.CFG", dir);
        f = fopen(path, "rb");
        if(!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if(n > 0x14 && buf[0x14] <= 4) return buf[0x14];
        break;
    }
    cfg_read(dir, &c);
    return c.quality;
}

/* 1 when the game has a SOUND.CFG to read at all, overlay or installed.
 * Without one the guest prints a DOS error and stops about 1.3 emulated
 * seconds in (docs/HANDOFF.md, Traps).  The Windows launcher writes one on
 * every Launch; a start with no launcher in front of it has to check
 * (src/host_sdl.c). */
int sound_cfg_exists(const char *dir){
    char path[600];
    FILE *f;
    int i;
    for(i=0;i<2;i++){
        snprintf(path, sizeof(path), i==0 ? "%s/PFEMU-STATE/SOUND.CFG" : "%s/SOUND.CFG", dir);
        f = fopen(path, "rb");
        if(f){ fclose(f); return 1; }
    }
    return 0;
}

/* ------------------------------------------------------- SOUND.CFG I/O
 *
 * Moved here from src/launch.c for the same reason as the readers above:
 * the Linux build has no launcher and still has to write it. */
/* Sound off: byte-identical to what SETSOUND writes for NOSOUND.SDR. */
static const uint8_t cfg_nosound[16] = {
    'N','O','S','O','U','N','D','.','S','D','R',0, 0,0,0x64,0
};

/* Sound on: SBLASTER.SDR with defaults.  Layout from static analysis of the
 * driver's config parse (open, lseek to 0x0E/0x11/0x14, one answer byte
 * each, masked with 7 through lookup tables for base port, IRQ, quality):
 * byte 0x0E = base-port index (1 -> 220h), 0x11 = IRQ index (3 -> IRQ 7),
 * 0x14 = quality index.  The gap bytes are never read by the driver.
 *
 * The quality byte is SETSOUND's five-notch Low..High setting, and the
 * driver's own table says exactly what it buys (disassembled at image
 * 0x1969, table at DS:6BA2 = image 0x25D3, five 4-byte entries):
 *
 *   notch  mixing rate   second word
 *     0      12000 Hz       0
 *     1      16000 Hz       0
 *     2      20000 Hz       0
 *     3      21000 Hz       0
 *     4      21000 Hz     0x00FF
 *
 * The first word is the mixing rate.  Notch 0's 12000 is the rate pfemu
 * already sees at today's default, so the word does reach the DSP time
 * constant; the other four are read out of the table, not measured, and
 * -snddbg prints what each one actually programs.
 *
 * The second word picks which of two templates the driver's code generator
 * at 0x1752 stamps out 64 times, patching each copy with its own index 0-63
 * (a per-volume-level mixing routine, on the usual MOD-player pattern):
 * 11 bytes per copy for notches 0-3, 31 bytes for notch 4.  What the longer
 * one computes has not been decoded, so notch 4 is offered but not described
 * beyond "21 kHz plus something extra" - it is the only way it differs from
 * notch 3.
 *
 * Cost is real and lands on the emulated 386, not the host: the mixer is
 * guest code, so a higher rate spends more of the fixed ~6 MIPS budget
 * (src/dev.c emu_ips) per second of audio, exactly as it would have on
 * period hardware.  -ips raises the modelled CPU if a high notch starves
 * the game loop. */
static void cfg_sblaster(uint8_t out[25], int quality){
    int i;
    for(i=0;i<25;i++) out[i]=0;
    memcpy(out, "SBLASTER.SDR", 12);
    out[0x0E]=1; out[0x11]=3;
    out[0x14]=(uint8_t)(quality < 0 ? 0 : (quality > 4 ? 4 : quality));
}

void write_sound_cfg(const char *dir, int on, int quality){
    char path[600], sub[600];
    FILE *f;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/SOUND.CFG", dir);
    f = fopen(path, "wb");
    if(!f) return;
    if(on){ uint8_t cfg[25]; cfg_sblaster(cfg, quality); fwrite(cfg, 1, 25, f); }
    else fwrite(cfg_nosound, 1, sizeof(cfg_nosound), f);
    fclose(f);
}

/* ------------------------------------------------ files beside the program
 *
 * Global state, not per install: which installation was launched last
 * (pfemu-last.cfg) and where the windows were (pfemu-winpos.cfg).  Moved
 * here from src/launch.c so the Linux build, which has no launcher, keeps
 * the same memory in the same files. */
/* Deliberately not called pfemu.cfg: that name is already the per-install
 * settings file (PFEMU-STATE/pfemu.cfg), and two different files sharing it
 * would only confuse. */
#define LAST_FILE "pfemu-last.cfg"
/* Window positions live apart from install memory on purpose
 * (pfemu-winpos.cfg): a read that finds nothing must only mean "center the
 * window", never wipe the remembered installation - and vice versa.  Two
 * tiny files that cannot clobber each other beat one clever one.
 *
 * The Linux build keeps its own.  Under WSL both programs sit in one folder,
 * and their coordinates do not mean the same place: Windows puts 0,0 at the
 * primary monitor's corner, WSLg at the leftmost monitor's.  A Windows
 * position restored under WSLg opened the window on the wrong monitor. */
#ifdef _WIN32
#define LAST_WINPOS "pfemu-winpos.cfg"
#else
#define LAST_WINPOS "pfemu-winpos-sdl.cfg"
#endif

/* Absolute path next to the exe.  A relative path would follow the process
 * CWD, which is not stable: a shortcut's "Start in" directory, a CLI run
 * from another folder, or any future file dialog can point launch-time and
 * quit-time at two different files, so a position saved on quit is never
 * found again on launch (or vice versa).  The exe's own directory never
 * moves under a running process.  The separator is the one the path already
 * uses, so this is right on both hosts (src/posix.c answers
 * GetModuleFileNameA from /proc/self/exe). */
void beside_exe(char *out, size_t n, const char *name){
    char exe[1024];
    DWORD len = GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe));
    if(len > 0 && len < sizeof(exe)){
        char *sep = strrchr(exe, '\\');
        char *sep2 = strrchr(exe, '/');
        if(sep2 && (!sep || sep2 > sep)) sep = sep2;
        if(sep){
            size_t dlen = (size_t)(sep - exe);
            if(dlen + 1 + strlen(name) < n){
                memcpy(out, exe, dlen);
                out[dlen] = 0;
                snprintf(out + dlen, n - dlen, "%c%s", *sep, name);
                return;
            }
        }
    }
    snprintf(out, n, "%s", name);   /* degraded, but never a failure */
}

/* One key=value line, trimmed; 0 for blanks and comments. */
static int kv_line(char *line, char **k_out, char **v_out){
    char *k = line, *eq, *e, *v;
    while(*k==' '||*k=='\t') k++;
    e = k + strlen(k);
    while(e > k && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
    if(!*k || *k=='#' || *k==';') return 0;
    eq = strchr(k, '=');
    if(!eq) return 0;
    *eq = 0;
    v = eq + 1;
    while(*v==' '||*v=='\t') v++;
    *k_out = k; *v_out = v;
    return 1;
}

/* Install memory only: dir + release.  Written outright on Launch, so there
 * is no read whose failure could ever blank it.  The directory as picked
 * plus the release id it detected as, so a renamed folder can still land on
 * the same version.  Missing or unreadable just reads as "no memory",
 * exactly like a first run - these are preferences, never a reason to
 * refuse. */
void last_save(const RelResult *r){
    char path[1080];
    FILE *f;
    if(!r || !r->dir[0]) return;
    beside_exe(path, sizeof(path), LAST_FILE);
    f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "# Last installation launched from the pfemu launcher.\n");
    fprintf(f, "dir=%s\n", r->dir);
    fprintf(f, "release=%s\n", r->rel ? r->rel->id : "");
    fclose(f);
}

/* The remembered installation among inst[0..n), or -1: the same directory
 * wins outright (even if what is in it changed - the launcher's detection
 * line then says so), otherwise the first runnable install of the
 * remembered release. */
int last_pick(const RelResult *inst, int n){
    char line[600], path[1080], dir[512] = "", rel[64] = "";
    FILE *f;
    int i;
    beside_exe(path, sizeof(path), LAST_FILE);
    f = fopen(path, "r");
    if(!f) return -1;
    while(fgets(line, sizeof(line), f)){
        char *k, *v;
        if(!kv_line(line, &k, &v)) continue;
        if(!strcmp(k, "dir")) snprintf(dir, sizeof(dir), "%s", v);
        else if(!strcmp(k, "release")) snprintf(rel, sizeof(rel), "%s", v);
    }
    fclose(f);
    if(dir[0]){
        for(i=0;i<n;i++)
            if(!_stricmp(inst[i].dir, dir)) return i;
    }
    if(rel[0]){
        for(i=0;i<n;i++)
            if(release_runnable(&inst[i]) && inst[i].rel &&
               !_stricmp(inst[i].rel->id, rel)) return i;
    }
    return -1;
}

/* Window positions only. Stored as full pairs or nothing: a half-written file
 * (crash between lines) centers instead of restoring garbage. */
typedef struct {
    int win_x, win_y;
    int have_win;
    int launch_x, launch_y;
    int have_launch;
} WinPos;

static void winpos_read(WinPos *o){
    char line[600], path[1080];
    FILE *f;
    memset(o, 0, sizeof(*o));
    beside_exe(path, sizeof(path), LAST_WINPOS);
    f = fopen(path, "r");
    if(!f) return;
    while(fgets(line, sizeof(line), f)){
        char *k, *v;
        if(!kv_line(line, &k, &v)) continue;
        if(!strcmp(k, "win_x")){ o->win_x = atoi(v); o->have_win |= 1; }
        else if(!strcmp(k, "win_y")){ o->win_y = atoi(v); o->have_win |= 2; }
        else if(!strcmp(k, "launch_x")){ o->launch_x = atoi(v); o->have_launch |= 1; }
        else if(!strcmp(k, "launch_y")){ o->launch_y = atoi(v); o->have_launch |= 2; }
    }
    fclose(f);
    if(o->have_win != 3) o->have_win = 0;   /* need the full pair to restore */
    if(o->have_launch != 3) o->have_launch = 0;
}

static void winpos_write(const WinPos *o){
    char path[1080];
    FILE *f;
    beside_exe(path, sizeof(path), LAST_WINPOS);
    f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "# pfemu window positions - delete to re-center.\n");
    if(o->have_win)
        fprintf(f, "win_x=%d\nwin_y=%d\n", o->win_x, o->win_y);
    if(o->have_launch)
        fprintf(f, "launch_x=%d\nlaunch_y=%d\n", o->launch_x, o->launch_y);
    fclose(f);
}

/* Global game-window position, shared by all installs (see pfemu.h). */
int last_read_winpos(int *x, int *y){
    WinPos o;
    winpos_read(&o);
    if(!o.have_win) return 0;
    if(x) *x = o.win_x;
    if(y) *y = o.win_y;
    return 1;
}

void last_save_winpos(int x, int y){
    WinPos o;
    winpos_read(&o);   /* keep the launcher dialog's spot */
    o.win_x = x; o.win_y = y; o.have_win = 3;
    winpos_write(&o);
}

/* Launcher dialog position: same global file, its own keys (see pfemu.h).
 * Saved on every teardown path, restored in show_launcher, centered when
 * there is nothing saved yet. */
int last_read_launchpos(int *x, int *y){
    WinPos o;
    winpos_read(&o);
    if(!o.have_launch) return 0;
    if(x) *x = o.launch_x;
    if(y) *y = o.launch_y;
    return 1;
}

void last_save_launchpos(int x, int y){
    WinPos o;
    winpos_read(&o);   /* keep the game window's spot */
    o.launch_x = x; o.launch_y = y; o.have_launch = 3;
    winpos_write(&o);
}
