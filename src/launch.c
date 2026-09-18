/* Win32 launcher: startup dialog for Pinball Fantasies.
 *
 * Lets the user turn SoundBlaster sound on/off (writes SOUND.CFG, read
 * directly by the game) and set every option from the game's own F5
 * in-game menu, without ever having to open that menu.  Unlike SOUND.CFG,
 * the options are NOT written to PINBALL.CFG for the game to read - see the
 * big comment above read_pinball_cfg() below for why, and src/fantasies.c
 * for the other half (the boot-time interception that actually applies
 * them).  Short version: an existing PINBALL.CFG at boot can wedge the
 * sound driver's PLL calibration into a busy-wait that never terminates,
 * a pfemu timing-emulation issue rather than a bug in the values, so they
 * go into a host-only file instead and get poked into memory directly.
 * Ingame Music additionally never worked from the in-game menu at all (the
 * setting was ignored), so the launcher is the only place it works.
 *
 * Ingame Music, once it reaches the table (see fantasies.c), only ever mutes
 * music *during an actual ball in play* - confirmed against the reconstructed
 * source (historicalsource/pinballfantasies on GitHub: FANTASIE.ASM,
 * SDEV/STONES/PLAND/SHOW.ASM). Setting it Off makes the table's boot-time
 * init call MUSIC_TOGGLE, which overwrites the shared S_SPRING/S_MAIN jingle
 * slots with the "empty jingle" ID; every later PLAYJINGLE S_SPRING/S_MAIN
 * (F1 start-of-game, NEW_BALL, ...) then plays silence. But the attract-mode
 * background jingle - played the instant a table boots and again after every
 * game-over or quit - is DEMO_MUSIC, and in all four tables that hardcodes
 * PLAYJINGLE S_NOHIGH, a different, never-muted slot; muzik_off is never
 * checked anywhere near it. So a table sitting idle always has music
 * regardless of this setting - that is the original 1992 game, not a pfemu
 * bug, and not something this option was ever wired to affect. Muting it too
 * would mean patching S_NOHIGH's slot the same way, which needs a per-table
 * signature scan (like fantasies_patch_sdr's) to locate - not done yet.
 *
 * This build supports Pinball Fantasies only.
 *
 * main() skips the dialog for explicit/automated runs (see -nolauncher,
 * -p, -setup, -secs handling there).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <commdlg.h>
#include <stdio.h>
#include <stdarg.h>
#include "pfemu.h"
#include "../res/resource.h"

/* Which release is in front of us is not something this dialog decides any
 * more, and not something it reads off a directory name.  src/release.c
 * hashes each candidate directory's five program files and identifies the
 * build from that; the dialog just shows the answer and, when more than one
 * installation is sitting side by side, lets the user pick which directory
 * to boot.  See docs/RELEASES.md for why the old two-hardcoded-directories,
 * one-hardcoded-PINBALL.EXE arrangement could not survive a third release:
 * Power Pack renamed the launcher to PF.EXE and shares its bytes with the
 * floppy one, so neither name identifies anything.
 *
 * The per-release differences that matter (intro options layout, the CD
 * marker check, which config fallback to defuse) are carried in the release
 * descriptor and consumed in src/fantasies.c.  The flipper/pause/spring
 * fixes still locate themselves by signature scan, so they need none of it. */

/* How many installation directories the picker will list.  Anyone with more
 * than this many collected side by side can pass -d. */
#define MAX_INSTALLS 8

#define ID_SOUND        104
#define ID_NOTE         105
#define ID_LAUNCH       106
#define ID_QUIT         107
#define ID_CHEAT_ENABLE 108
#define ID_FULLSCREEN   109
#define ID_INSTALL      110
#define ID_DETAILS      111
#define ID_QUALITY      112
#define ID_VOLUME       113
#define ID_VOLLABEL     114
#define ID_MODE_PLAY    115
#define ID_MODE_RECORD  116
#define ID_MODE_REPLAY  117
#define ID_REPLAY_PATH  118
#define ID_BROWSE       119
#define ID_TABLE        130   /* "Start at" combo; above ID_OPT_FIRST+5 */
#define ID_OPT_FIRST 120   /* ID_OPT_FIRST + option index = combo control id */

/* ------------------------------------------------------- SOUND.CFG I/O */
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
 * Two bytes in a host-only file: {volume 0-100, quality notch 0-4}.
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
static void read_audio_cfg(const char *dir, int *vol, int *qual){
    char path[600];
    FILE *f;
    uint8_t b[2];
    size_t n;
    *vol = AUDIO_VOLUME_DEFAULT;
    *qual = 0;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_audio.cfg", dir);
    f = fopen(path, "rb");
    if(!f) return;
    n = fread(b, 1, 2, f);
    fclose(f);
    if(n >= 1 && b[0] <= 100) *vol = b[0];
    if(n >= 2 && b[1] <= 4) *qual = b[1];
}

static void write_audio_cfg(const char *dir, int vol, int qual){
    char path[600], sub[600];
    FILE *f;
    uint8_t b[2];
    if(vol < 0) vol = 0;
    if(vol > 100) vol = 100;
    if(qual < 0) qual = 0;
    if(qual > 4) qual = 4;
    b[0] = (uint8_t)vol;
    b[1] = (uint8_t)qual;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_audio.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    fwrite(b, 1, 2, f);
    fclose(f);
}

int read_volume_cfg(const char *dir){
    int vol, qual;
    read_audio_cfg(dir, &vol, &qual);
    return vol;
}

/* Volume alone, for the in-window -/+ keys on their way out (src/main.c).
 * Reads first so the quality notch in byte 1 survives: the game window has no
 * way to change that, and clobbering it would silently undo the launcher. */
void write_volume_cfg(const char *dir, int vol){
    int cur, qual;
    read_audio_cfg(dir, &cur, &qual);
    write_audio_cfg(dir, vol, qual);
}

/* Quality notch actually in force: the effective SOUND.CFG when it is an SB
 * config long enough to carry one (overlay first, then installed, same order
 * as read_sound_is_sb), otherwise the mirror above. */
int read_sound_quality(const char *dir){
    FILE *f;
    uint8_t buf[25];
    size_t n;
    int i, vol, qual;
    char path[600];
    for(i=0;i<2;i++){
        snprintf(path, sizeof(path), i==0 ? "%s/PFEMU-STATE/SOUND.CFG" : "%s/SOUND.CFG", dir);
        f = fopen(path, "rb");
        if(!f) continue;
        n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if(n > 0x14 && buf[0x14] <= 4) return buf[0x14];
        break;
    }
    read_audio_cfg(dir, &vol, &qual);
    return qual;
}

/* --------------------------------------------------- launcher options I/O
 *
 * These six option bytes mirror the layout of the 6-byte blob INTRO.PRG's
 * own F5 menu edits in place and writes to PINBALL.CFG at the intro-to-
 * table handoff (docs/EMULATOR.md: launch chain, copy protection) - but this launcher
 * does NOT write PINBALL.CFG itself.  Whenever INTRO.PRG's boot-time read of
 * an *existing* PINBALL.CFG succeeds, the extra DOS calls that costs versus
 * a fresh install (where the open just fails) are frequently enough to keep
 * a table's sound-driver PLL calibration from ever converging - a pfemu
 * timing-emulation issue, not a bug in these values.  Confirmed by direct
 * A/B testing: the unmodified emulator reliably loads tables when
 * PINBALL.CFG has never existed, and reliably wedges once one exists and
 * gets read at boot, regardless of what's in it.  (A watchdog that detected
 * the wedge and kicked the emulated clock forward was tried and measured
 * worse than doing nothing - it was tripping on legitimate PIT activity
 * unrelated to the calibration and destabilising boots that would have
 * converged fine on their own, so it was removed rather than tuned
 * further.)  So the launcher writes these bytes to a host-only file DOS
 * never opens; src/fantasies.c makes every boot-time PINBALL.CFG open fail
 * like a fresh install always has, and pokes these bytes into INTRO.PRG's
 * own buffer at that exact moment instead - zero extra guest instructions,
 * so boot timing stays identical to the one case already proven reliable.
 *
 * Byte order and values confirmed empirically, one option at a time, by
 * dumping the live in-memory buffer (DS:49A3 in INTRO.PRG, so linear 062E3)
 * against what the F5 menu displayed for each:
 *
 *   0  Balls          0 = 3          1 = 5
 *   1  Angle          0 = High       1 = Low
 *   2  Scrolling      0 = Hard       1 = Medium   2 = Soft
 *   3  Ingame Music   0 = On         1 = Off
 *   4  Resolution     0 = Normal     1 = High
 *   5  Color Mode     0 = Color      1 = Mono
 *
 * This is also the game's own hardcoded default (a fresh install with no
 * PINBALL.CFG on disk boots with the buffer already at 00 00 01 00 00 00). */
static const uint8_t cfg_pinball_defaults[6] = {0,0,1,0,0,0};

typedef struct { const char *label; const char *values[3]; int n; } OptDef;
/* "Start at" choices.  Index is the program slot the boot loop EXECs,
 * so 0 really is "the menu" and 1-4 line up with Table1-4.Prg. */
static const char *table_labels[5] = {
    "Menu (normal start)",
    "Table 1 - Party Land",
    "Table 2 - Speed Devils",
    "Table 3 - Billion Dollar Gameshow",
    "Table 4 - Stones 'N Bones",
};

static const OptDef opts[6] = {
    { "Balls:",        {"3","5",NULL},                 2 },
    { "Angle:",        {"High","Low",NULL},             2 },
    { "Scrolling:",    {"Hard","Medium","Soft"},         3 },
    { "Ingame Music:", {"On","Off",NULL},                2 },
    { "Resolution:",   {"Normal","High",NULL},           2 },
    { "Color Mode:",   {"Color","Mono",NULL},            2 },
};

/* Host-only staging file: read by src/fantasies.c's boot-time interception,
 * never opened by the guest. */
static void read_pinball_cfg(const char *dir, uint8_t out[6]){
    char path[600];
    FILE *f;
    size_t n = 0;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_options.cfg", dir);
    f = fopen(path, "rb");
    if(f){ n = fread(out, 1, 6, f); fclose(f); }
    if(n < 6) memcpy(out, cfg_pinball_defaults, 6);
}

static void write_pinball_cfg(const char *dir, const uint8_t in[6]){
    char path[600], sub[600];
    FILE *f;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_options.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    fwrite(in, 1, 6, f);
    fclose(f);
}

/* Trainer cheats, ported from trainer/PINTRN.COM and trainer/TRAINER.EXE
 * (see src/fantasies.c for the reverse-engineering writeup and the actual
 * patch logic).  Separate 2-byte file from pfemu_options.cfg above since
 * these aren't part of PINBALL.CFG's own layout - just a starting state
 * that src/fantasies.c applies the moment a table loads.  One checkbox in
 * the UI ("Enable trainer") drives both bytes together - infinite balls,
 * ball control and infinite tilts are still three independent patches
 * underneath (and the '1'-'3' hotkeys still toggle them independently
 * in-game), but there's no real reason to make the user tick two boxes
 * to turn "the trainer" on, so both get written identically here.  Reading them back separately (rather than
 * assuming they match) means a config saved by an older build of this
 * launcher, with only one of the two set, still shows the checkbox checked
 * instead of silently discarding half of it. */
static void read_cheats_cfg(const char *dir, int *balls, int *spring){
    char path[600];
    FILE *f;
    uint8_t b[2] = {0,0};
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_cheats.cfg", dir);
    f = fopen(path, "rb");
    if(f){ if(fread(b,1,2,f) != 2){ b[0]=0; b[1]=0; } fclose(f); }
    *balls = b[0] != 0;
    *spring = b[1] != 0;
}

static void write_cheats_cfg(const char *dir, int balls, int spring){
    char path[600], sub[600];
    FILE *f;
    uint8_t b[2];
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_cheats.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    b[0] = (uint8_t)(balls ? 1 : 0);
    b[1] = (uint8_t)(spring ? 1 : 0);
    fwrite(b, 1, 2, f);
    fclose(f);
}

/* Host-only, one byte: whether to start the game window fullscreen. Alt+Enter
 * still toggles it live once running (src/main.c); this just picks the
 * starting state so it doesn't have to be flipped by hand every launch. */
static int read_fullscreen_cfg(const char *dir){
    char path[600];
    FILE *f;
    uint8_t b = 0;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_display.cfg", dir);
    f = fopen(path, "rb");
    if(f){ if(fread(&b,1,1,f) != 1) b = 0; fclose(f); }
    return b != 0;
}

static void write_fullscreen_cfg(const char *dir, int on){
    char path[600], sub[600];
    FILE *f;
    uint8_t b = (uint8_t)(on ? 1 : 0);
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_display.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    fwrite(&b, 1, 1, f);
    fclose(f);
}

/* ------------------------------------------------------------------ UI */
/* Quality notch labels.  SETSOUND offered these as five unlabelled steps
 * between "Low" and "High"; the rate is what the driver's table actually
 * selects for each (see cfg_sblaster above), which is more use than the
 * original wording. */
static const char *quality_labels[5] = {
    "1 - 12000 Hz",
    "2 - 16000 Hz",
    "3 - 20000 Hz",
    "4 - 21000 Hz",
    "5 - 21000 Hz (extended mix)"
};

typedef struct {
    int sound;              /* checkbox state */
    int quality;            /* combo state: SOUND.CFG quality notch, 0-4 */
    int volume;             /* slider state: host output gain, 0-100 */
    uint8_t cfg[6];         /* PINBALL.CFG option bytes */
    int cheat_enable;        /* checkbox state: trainer (infinite balls + ball control) */
    int fullscreen;          /* checkbox state: start the window fullscreen */
    int done;               /* dialog finished */
    int ok;                 /* 1 = launch, 0 = quit */
    RelResult inst[MAX_INSTALLS]; /* every installation directory found */
    int ninst;
    int sel;                 /* which one is selected */
    HWND hSound, hOpt[6], hCheatEnable, hFullscreen;
    HWND hInstall, hDetected, hDetails;
    HWND hQuality, hVolume, hVolLabel;
    HWND hTable;            /* "Start at": menu, or straight to a table */
    int  start_table;       /* 0 = menu, 1-4 */
    HWND hModePlay, hModeRecord, hModeReplay, hPathLabel, hPath, hBrowse;
    HFONT hFont;
    /* Session record / replay (docs/REPLAY.md section 4).  mode is the
     * Play/Record/Replay radio group below fullscreen; replay_path is the
     * .pfr target (record) or source (replay).  rhdr is the parsed replay
     * header used by auto-restore; replay_err explains why a file was
     * rejected, shown in the detection line. */
    LaunchMode mode;
    char replay_path[512];
    int path_custom;        /* the user typed/picked a path; keep it */
    int updating_path;      /* SetWindowText in progress; ignore EN_CHANGE */
    ReplayHeader rhdr;
    int rhdr_ok;
    char replay_err[256];
} LaunchState;

static void set_vol_label(LaunchState *st){
    char t[16];
    snprintf(t, sizeof(t), "%d%%", st->volume);
    if(st->hVolLabel) SetWindowTextA(st->hVolLabel, t);
}

static const RelResult *cur_inst(const LaunchState *st){
    return (st->ninst && st->sel >= 0 && st->sel < st->ninst)
           ? &st->inst[st->sel] : NULL;
}

static const char *cur_game_dir(const LaunchState *st){
    const RelResult *r = cur_inst(st);
    return r ? r->dir : "";
}

/* "Start at" per install.  Host-only, like the volume and session files:
 * the guest never sees it, and a release whose INT 65h layout cannot be
 * derived just falls back to the menu at boot (src/fantasies.c). */
static int read_table_cfg(const char *dir){
    char path[600];
    FILE *f;
    int v = 0;
    if(!dir || !dir[0]) return 0;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_table.cfg", dir);
    f = fopen(path, "rb");
    if(!f) return 0;
    if(fscanf(f, "%d", &v) != 1) v = 0;
    fclose(f);
    return (v >= 1 && v <= 4) ? v : 0;
}
static void write_table_cfg(const char *dir, int v){
    char path[600], sub[600];
    FILE *f;
    if(!dir || !dir[0]) return;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_table.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "%d\n", v);
    fclose(f);
}

/* Last-used session file per install (record target or replay source).
 * Restored into the dialog so a replay file doesn't have to be re-picked
 * every time; overwritten on each successful Launch in record/replay mode.
 * Display/hint only - replay identity still comes from the file header. */
static void read_session_path(const char *dir, char *dst, size_t n){
    char path[600];
    FILE *f;
    dst[0] = 0;
    if(!dir || !dir[0]) return;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_session.cfg", dir);
    f = fopen(path, "rb");
    if(!f) return;
    if(!fgets(dst, (int)n, f)) dst[0] = 0;
    fclose(f);
    dst[strcspn(dst, "\r\n")] = 0;
}
static void write_session_path(const char *dir, const char *p){
    char path[600], sub[600];
    FILE *f;
    if(!dir || !dir[0] || !p || !p[0]) return;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_session.cfg", dir);
    f = fopen(path, "wb");
    if(!f) return;
    fprintf(f, "%s\n", p);
    fclose(f);
}

/* Forward: replay_autorestore()/apply_mode_ui() below call these, which are
 * defined further down next to the controls they update. */
static void show_detection(HWND h, LaunchState *st);
static void reload_for_dir(HWND h, LaunchState *st);

/* Default record target: sessions/<install>_<date>.pfr (REPLAY.md section
 * 4), next to pfemu.exe.  The install dir is sanitised: it is only ever a
 * plain directory name, but never trust a filename you did not build. */
static void default_record_path(LaunchState *st){
    SYSTEMTIME t;
    char safe[64];
    size_t i;
    const char *dir = cur_game_dir(st);
    GetLocalTime(&t);
    for(i=0;i<sizeof(safe)-1 && dir[i];i++){
        char c = dir[i];
        safe[i] = (c=='\\'||c=='/'||c==':'||c==' ') ? '_' : c;
    }
    safe[i] = 0;
    if(!safe[0]) snprintf(safe, sizeof(safe), "GAME");
    snprintf(st->replay_path, sizeof(st->replay_path),
             "sessions\\%s_%04d%02d%02d_%02d%02d%02d.pfr",
             safe, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
}

/* Identity compare for auto-restore (REPLAY.md 4.1): same release id AND
 * same code hash vector.  Summaries, directory names and timestamps are
 * never identity. */
static int same_vector(const ReplayHeader *h, const RelResult *r){
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

/* Is the currently selected install a valid replay target for the loaded
 * file?  Fills why (when non-NULL) for the detection line. */
static int replay_selection_ok(LaunchState *st, char *why, size_t n){
    const RelResult *r = cur_inst(st);
    if(!st->rhdr_ok){
        if(why) snprintf(why, n, "%s", st->replay_err);
        return 0;
    }
    if(!st->rhdr.trainer_off){
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
    if(!same_vector(&st->rhdr, r)){
        if(r->rel && !_stricmp(st->rhdr.release_id, r->rel->id)){
            if(why) snprintf(why, n, "Same release (%s) but a different copy - replay needs"
                             " the recorded programs.", r->rel->id);
        } else {
            if(why) snprintf(why, n, "Replay is '%s', selected install is '%s' - refused.",
                             st->rhdr.release_id, r->rel ? r->rel->id : "?");
        }
        return 0;
    }
    if(_stricmp(st->rhdr.program, r->boot)){
        if(why) snprintf(why, n, "File starts at '%s', not this install's boot program"
                         " (direct-table replay is v2).", st->rhdr.program);
        return 0;
    }
    {
        int balls, spring;
        read_cheats_cfg(r->dir, &balls, &spring);
        if(balls || spring){
            if(why) snprintf(why, n, "Trainer is enabled for '%s' - replay needs it off.", r->dir);
            return 0;
        }
    }
    return 1;
}

/* Auto-restore on replay-file load (REPLAY.md 4.1): scan the installs and
 * pick one that IS the recording - exact vector match first (preferring the
 * recorded dir if it still matches), else the first runnable install of the
 * same release, else no auto-launch.  Never applies one release's layout to
 * another: that falls out of same_vector() refusing. */
static void replay_autorestore(HWND h, LaunchState *st){
    int k, same = -1;
    if(!st->rhdr_ok || !st->rhdr.trainer_off) return;
    /* Exact pass over the dialog's install list. */
    for(k=0;k<st->ninst;k++){
        const RelResult *r = &st->inst[k];
        if(!release_runnable(r) || !same_vector(&st->rhdr, r)) continue;
        if(same < 0) same = k;
        /* Prefer the recorded dir when it still matches. */
        if(!_stricmp(r->dir, st->rhdr.dir_hint)){ same = k; break; }
    }
    if(same >= 0){
        st->sel = same;
        if(st->hInstall) SendMessageA(st->hInstall, CB_SETCURSEL, st->sel, 0);
        reload_for_dir(h, st);
        return;
    }
    /* Same release, different copy: first runnable install with the id. */
    for(k=0;k<st->ninst;k++){
        const RelResult *r = &st->inst[k];
        if(release_runnable(r) && r->rel && !_stricmp(st->rhdr.release_id, r->rel->id)){
            st->sel = k;
            if(st->hInstall) SendMessageA(st->hInstall, CB_SETCURSEL, st->sel, 0);
            reload_for_dir(h, st);
            return;
        }
    }
    /* No match: no auto-launch.  show_detection() (via the caller) leaves
     * Launch disabled and Details carries the recorded-vs-found report. */
    (void)h;
}

/* Push one replay file through parse + restore + detection-line update.
 * Silent (no popups): typing a half-finished path must not nag. */
static void replay_load_file(HWND h, LaunchState *st){
    if(!st->replay_path[0]){
        st->rhdr_ok = 0;
        snprintf(st->replay_err, sizeof(st->replay_err), "Pick a .pfr replay file.");
    } else if(replay_read_header(st->replay_path, &st->rhdr) != 0){
        st->rhdr_ok = 0;
        snprintf(st->replay_err, sizeof(st->replay_err),
                 "Not a readable replay file: %s", st->replay_path);
    } else {
        st->rhdr_ok = 1;
        st->replay_err[0] = 0;
        replay_autorestore(h, st);
    }
    show_detection(h, st);
}

/* Fill the path field on mode entry: the install's last-used session file
 * when there is one, else the default record target (record) or empty
 * (replay).  Never clobbers a user-typed/picked path.  Install switches
 * keep the current path, except record mode which re-targets to the new
 * install (saved file or fresh default) - a record target naming another
 * install would only confuse. */
static void restore_session_path(HWND h, LaunchState *st, int install_switch){
    char saved[512];
    if(st->path_custom) return;
    if(install_switch && st->mode == LAUNCH_REPLAY) return;
    read_session_path(cur_game_dir(st), saved, sizeof(saved));
    if(saved[0]) snprintf(st->replay_path, sizeof(st->replay_path), "%s", saved);
    else if(st->mode == LAUNCH_RECORD) default_record_path(st);
    else st->replay_path[0] = 0;
    if(st->hPath){
        st->updating_path = 1;
        SetWindowTextA(st->hPath, st->replay_path);
        st->updating_path = 0;
    }
    if(st->mode == LAUNCH_REPLAY) replay_load_file(h, st);
}

/* Mode switching: the trainer checkbox is greyed out in record and replay
 * modes (REPLAY.md 4), and entering those modes unchecks it - the trainer
 * is incompatible with both, and main() refuses to start either mode with
 * it on as a backstop (covers the CLI and hand-edited configs).  The volume
 * slider stays enabled in all modes: volume is host gain, never recorded. */
static void apply_mode_ui(HWND h, LaunchState *st){
    int rec = (st->mode != LAUNCH_PLAY);
    if(st->hModePlay) CheckDlgButton(h, ID_MODE_PLAY, st->mode==LAUNCH_PLAY?BST_CHECKED:BST_UNCHECKED);
    if(st->hModeRecord) CheckDlgButton(h, ID_MODE_RECORD, st->mode==LAUNCH_RECORD?BST_CHECKED:BST_UNCHECKED);
    if(st->hModeReplay) CheckDlgButton(h, ID_MODE_REPLAY, st->mode==LAUNCH_REPLAY?BST_CHECKED:BST_UNCHECKED);
    /* The .pfr records how its session began - menu or a table - and
     * that has to win, so replay owns this control. */
    if(st->hTable) EnableWindow(st->hTable, st->mode != LAUNCH_REPLAY);
    if(st->hPath) EnableWindow(st->hPath, rec);
    if(st->hBrowse) EnableWindow(st->hBrowse, rec);
    if(st->hPathLabel) EnableWindow(st->hPathLabel, rec);
    if(rec){
        int balls, spring;
        st->cheat_enable = 0;
        CheckDlgButton(h, ID_CHEAT_ENABLE, BST_UNCHECKED);
        if(st->hCheatEnable) EnableWindow(st->hCheatEnable, FALSE);
        /* A saved trainer-on must not survive into the session either: the
         * mode owns it, so it goes off on commit (see ID_LAUNCH). */
        read_cheats_cfg(cur_game_dir(st), &balls, &spring);
        (void)balls; (void)spring;
        restore_session_path(h, st, 0);
    } else {
        int balls, spring;
        if(st->hCheatEnable) EnableWindow(st->hCheatEnable, TRUE);
        /* Leaving record/replay restores the saved trainer state into the
         * checkbox; play mode neither forces nor forbids it. */
        read_cheats_cfg(cur_game_dir(st), &balls, &spring);
        st->cheat_enable = balls || spring;
        CheckDlgButton(h, ID_CHEAT_ENABLE, st->cheat_enable?BST_CHECKED:BST_UNCHECKED);
    }
    show_detection(h, st);
}

/* The one read-only line that replaced the version radio buttons: whatever
 * the detector concluded about the selected directory.  Launch is enabled
 * only for a release that was actually recognised - an unknown or mixed
 * installation must never get a known release's memory layout poked into it,
 * and the Details button is there to say exactly what was wrong. */
static void show_detection(HWND h, LaunchState *st){
    const RelResult *r = cur_inst(st);
    char line[300];
    HWND btn = GetDlgItem(h, ID_LAUNCH);
    if(st->mode == LAUNCH_REPLAY){
        /* Replay: Launch needs a valid file AND a matching install
         * (REPLAY.md 4.1).  Details stays enabled so a refusal explains
         * itself through the recorded-vs-found report. */
        char why[256];
        if(replay_selection_ok(st, why, sizeof(why))){
            snprintf(line, sizeof(line), "Replay ready: %s", st->rhdr.summary);
            if(st->hDetected) SetWindowTextA(st->hDetected, line);
            if(btn) EnableWindow(btn, TRUE);
        } else {
            snprintf(line, sizeof(line), "Replay: %s", why);
            if(st->hDetected) SetWindowTextA(st->hDetected, line);
            if(btn) EnableWindow(btn, FALSE);
        }
        if(st->hDetails) EnableWindow(st->hDetails, TRUE);
    } else if(st->mode == LAUNCH_RECORD){
        if(!r) snprintf(line, sizeof(line), "No game found. Put one release's files in GAME\\.");
        else if(release_runnable(r) && st->replay_path[0])
            snprintf(line, sizeof(line), "Record %.120s -> %.120s", r->summary, st->replay_path);
        else if(release_runnable(r))
            snprintf(line, sizeof(line), "Record %s (pick a target file)", r->summary);
        else snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        if(st->hDetected) SetWindowTextA(st->hDetected, line);
        if(btn) EnableWindow(btn, r && release_runnable(r) && st->replay_path[0]);
        if(st->hDetails) EnableWindow(st->hDetails, r != NULL);
    } else {
        if(!r) snprintf(line, sizeof(line), "No game found. Put one release's files in GAME\\.");
        else if(release_runnable(r)) snprintf(line, sizeof(line), "Detected: %s", r->summary);
        else snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
        if(st->hDetected) SetWindowTextA(st->hDetected, line);
        if(btn) EnableWindow(btn, r && release_runnable(r));
        if(st->hDetails) EnableWindow(st->hDetails, r != NULL);
    }
    /* The six game options are the intro's own PINBALL.CFG structure, and the
     * 1993 demo's intro simply has none - no F5 menu, no config file, nothing
     * for fantasies.c to poke (see its cfg_buf).  Offering combo boxes that
     * could not reach the game would be worse than showing them greyed. */
    {
        int has_opts = r && release_runnable(r) && r->rel && r->rel->cfg_buf != 0;
        int i;
        for(i=0;i<6;i++)
            if(st->hOpt[i]) EnableWindow(st->hOpt[i], has_opts);
    }
}

/* Re-reads every per-install setting for whichever directory is now selected
 * and pushes it into the already-created controls - used both at dialog
 * startup and whenever the installation picker changes, so switching never
 * leaves one install's checkbox state applied to another's. */
static void reload_for_dir(HWND h, LaunchState *st){
    const char *dir = cur_game_dir(st);
    int i, balls, spring;
    st->sound = read_sound_is_sb(dir);
    st->quality = read_sound_quality(dir);
    st->volume = read_volume_cfg(dir);
    read_pinball_cfg(dir, st->cfg);
    read_cheats_cfg(dir, &balls, &spring);
    st->cheat_enable = balls || spring;
    st->fullscreen = read_fullscreen_cfg(dir);
    if(st->hSound){
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        SendMessageA(st->hQuality,CB_SETCURSEL,st->quality,0);
        SendMessageA(st->hVolume,TBM_SETPOS,TRUE,st->volume);
        set_vol_label(st);
        for(i=0;i<6;i++) SendMessageA(st->hOpt[i],CB_SETCURSEL,st->cfg[i],0);
        CheckDlgButton(h,ID_CHEAT_ENABLE,st->cheat_enable?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,ID_FULLSCREEN,st->fullscreen?BST_CHECKED:BST_UNCHECKED);
    }
    if(st->mode != LAUNCH_PLAY){
        /* The install switch must not resurrect the trainer behind the
         * mode's back: it stays unchecked and greyed until Play returns. */
        st->cheat_enable = 0;
        CheckDlgButton(h,ID_CHEAT_ENABLE,BST_UNCHECKED);
        if(st->hCheatEnable) EnableWindow(st->hCheatEnable, FALSE);
        if(st->mode == LAUNCH_RECORD && !st->path_custom)
            restore_session_path(h, st, 1);
        /* Replay shows the FILE's session, not the install's current
         * settings: sound, quality, the six options and fullscreen are
         * what the run will use (quality/options/sound are verified and
         * enforced at runtime; fullscreen is host-only).  Display only -
         * replay mode writes no configs on Launch. */
        if(st->mode == LAUNCH_REPLAY && st->rhdr_ok && st->hSound){
            int i;
            st->sound = st->rhdr.sound ? 1 : 0;
            CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
            st->quality = st->rhdr.quality;
            if(st->quality < 0) st->quality = 0;
            if(st->quality > 4) st->quality = 4;
            SendMessageA(st->hQuality,CB_SETCURSEL,st->quality,0);
            st->start_table = read_table_cfg(cur_game_dir(st));
            if(st->hTable) SendMessageA(st->hTable,CB_SETCURSEL,st->start_table,0);
            for(i=0;i<6;i++){
                int v = st->rhdr.options[i];
                if(v < 0) v = 0;
                if(v >= opts[i].n) v = opts[i].n - 1;
                st->cfg[i] = (uint8_t)v;
                SendMessageA(st->hOpt[i],CB_SETCURSEL,v,0);
            }
            st->fullscreen = st->rhdr.fullscreen ? 1 : 0;
            CheckDlgButton(h,ID_FULLSCREEN,st->fullscreen?BST_CHECKED:BST_UNCHECKED);
        }
    }
    show_detection(h, st);
}

/* ------------------------------------------------------- details window */
/* Details used to be a MessageBox.  That renders the report in the shell's
 * proportional UI font, so every column the report aligns with %-11s and a
 * 64-hex digest collapsed into an unreadable ribbon, and a long report was
 * cut off with no way to scroll.  In replay mode it was worse still: the
 * recorded header and the install report were simply concatenated, so the
 * release id, the layout, the boot program and the entire code vector each
 * appeared twice, once in each half's own wording.
 *
 * So: a real modal window, fixed-pitch and scrollable and resizable, fed a
 * single report that states each thing once.  The code vector in particular
 * now appears exactly one time - as a recorded-vs-installed comparison,
 * which is the only form of it that answers the question the user opened
 * this window to ask.  The text stays plain and copyable (Copy selects the
 * lot and copies it): for a release the database does not know, it is the
 * intake format of docs/RELEASES.md, sizes and hashes the user can send on
 * without having run anything.
 */
#define ID_DET_TEXT  200
#define ID_DET_COPY  201
/* Close carries IDCANCEL so Esc, the title bar's X and the button are one
 * path (IsDialogMessage turns Esc into WM_COMMAND/IDCANCEL). */

#define DET_MAX 20480
#define DET_RULE "----------------------------------------------------------------"

static void det_add(char *dst, size_t n, const char *fmt, ...){
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
static void det_kv(char *dst, size_t n, const char *label, const char *fmt, ...){
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

static void det_hex32(const uint8_t d[32], char out[65]){
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
        snprintf(lab, sizeof(lab), "%s", opts[i].label);
        l = strlen(lab);
        if(l && lab[l-1] == ':') lab[l-1] = 0;
        snprintf(dst + strlen(dst), n - strlen(dst), "%s%s %s",
                 dst[0] ? ", " : "", lab,
                 (v >= 0 && v < opts[i].n) ? opts[i].values[v] : "?");
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

/* The whole report for whatever the dialog is currently showing. */
static void details_build(LaunchState *st, char *dst, size_t n){
    const RelResult *r = cur_inst(st);
    dst[0] = 0;
    if(st->mode == LAUNCH_REPLAY){
        char why[256];
        int ok = replay_selection_ok(st, why, sizeof(why));
        det_sect(dst, n, "Status");
        det_add(dst, n, "  %s\n", ok ? "Ready to replay." : why);
        if(st->rhdr_ok){
            const ReplayHeader *h = &st->rhdr;
            char buf[512];
            det_sect(dst, n, "Replay file");
            det_kv(dst, n, "File", "%s", st->replay_path);
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
            if(h->nolzexe) snprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), ", nolzexe");
            det_kv(dst, n, "Emulation", "%s", buf);
            /* Only worth a line when it is the reason the file is refused:
             * a playable replay always recorded with the trainer off. */
            if(!h->trainer_off)
                det_kv(dst, n, "Trainer", "on when recorded - unplayable");
            if(h->overlay[0]) det_kv(dst, n, "Overlay", "%s", h->overlay);
            if(h->have_wav && strcmp(h->wav_hash, "none"))
                det_kv(dst, n, "Capture", "%s, %lu samples",
                       h->wav_hash, h->wav_samples);
            if(h->dir_hint[0]) det_kv(dst, n, "Recorded in", "%s", h->dir_hint);
            snprintf(buf, sizeof(buf), "Programs - recorded (%s layout)"
                     " vs. this install", h->layout);
            det_sect(dst, n, buf);
            det_vector(dst, n, h, r);
        }
    }
    det_sect(dst, n, "Installation");
    if(r) det_body(dst, n, r->detail);
    else  det_add(dst, n, "  No installation is selected.\n");
}

/* Edit controls want CRLF; every report above is built with bare \n. */
static void det_crlf(const char *src, char *dst, size_t n){
    size_t o = 0;
    for(; *src && o + 3 < n; src++){
        if(*src == '\n') dst[o++] = '\r';
        dst[o++] = *src;
    }
    dst[o] = 0;
}

typedef struct {
    const char *text;
    HWND owner, hEdit, hCopy, hClose;
    HFONT hMono, hUi;
    int done;
} DetState;

static int det_registered = 0;

static LRESULT CALLBACK details_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    DetState *d = (DetState*)(INT_PTR)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch(m){
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)l;
        d = (DetState*)cs->lpCreateParams;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)d);
        /* ES_AUTOHSCROLL with a horizontal scrollbar rather than wrapping:
         * a wrapped hash line is exactly the mess this window exists to
         * fix, and every line is written to fit the default width anyway. */
        d->hEdit = CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","",
                        WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|WS_HSCROLL|
                        ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,
                        0,0,10,10,h,(HMENU)ID_DET_TEXT,cs->hInstance,0);
        SendMessageA(d->hEdit,WM_SETFONT,(WPARAM)d->hMono,0);
        SetWindowTextA(d->hEdit, d->text);
        d->hCopy = CreateWindowExA(0,"BUTTON","Copy",
                        WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                        0,0,10,10,h,(HMENU)ID_DET_COPY,cs->hInstance,0);
        SendMessageA(d->hCopy,WM_SETFONT,(WPARAM)d->hUi,0);
        d->hClose = CreateWindowExA(0,"BUTTON","Close",
                        WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                        0,0,10,10,h,(HMENU)IDCANCEL,cs->hInstance,0);
        SendMessageA(d->hClose,WM_SETFONT,(WPARAM)d->hUi,0);
        return 0; }
    case WM_SIZE: {
        int cw = LOWORD(l), ch = HIWORD(l);
        int pad = 10, bw = 88, bh = 26;
        if(!d) return 0;
        MoveWindow(d->hEdit, pad, pad, cw-2*pad, ch-3*pad-bh, TRUE);
        MoveWindow(d->hCopy, cw-pad-2*bw-8, ch-pad-bh, bw, bh, TRUE);
        MoveWindow(d->hClose, cw-pad-bw, ch-pad-bh, bw, bh, TRUE);
        return 0; }
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO*)l;
        mm->ptMinTrackSize.x = 420;
        mm->ptMinTrackSize.y = 220;
        return 0; }
    /* A read-only EDIT paints itself in the button face colour, which is
     * half of why the old report looked washed out.  Give it the same
     * white a normal text field has. */
    case WM_CTLCOLORSTATIC:
        if(d && (HWND)l == d->hEdit){
            SetBkColor((HDC)w, GetSysColor(COLOR_WINDOW));
            SetTextColor((HDC)w, GetSysColor(COLOR_WINDOWTEXT));
            return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
        }
        break;
    case WM_SETFOCUS:
        if(d && d->hEdit) SetFocus(d->hEdit);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if(id == ID_DET_COPY && d){
            SendMessageA(d->hEdit, EM_SETSEL, 0, (LPARAM)-1);
            SendMessageA(d->hEdit, WM_COPY, 0, 0);
            SendMessageA(d->hEdit, EM_SETSEL, (WPARAM)-1, 0);
            SetWindowTextA(d->hCopy, "Copied");
            SetFocus(d->hEdit);
            return 0;
        }
        if(id == IDCANCEL || id == IDOK){
            SendMessageA(h, WM_CLOSE, 0, 0);
            return 0;
        }
        return 0; }
    case WM_CLOSE:
        /* Hand the launcher back its input before this window goes away,
         * or the focus lands in whatever is behind pfemu. */
        if(d){
            d->done = 1;
            if(d->owner) EnableWindow(d->owner, TRUE);
        }
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcA(h,m,w,l);
}

/* Modal report window, owned by and centred on the launcher. */
static void show_details(HWND owner, const char *text){
    DetState d;
    MSG msg;
    HINSTANCE hinst = GetModuleHandleA(NULL);
    RECT ro;
    int winw = 780, winh = 540, x, y;
    HDC dc;

    memset(&d, 0, sizeof(d));
    d.text = text;
    d.owner = owner;
    d.hUi = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    dc = GetDC(NULL);
    d.hMono = CreateFontA(-MulDiv(9, GetDeviceCaps(dc, LOGPIXELSY), 72), 0, 0, 0,
                          FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                          CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
                          FIXED_PITCH|FF_MODERN, "Consolas");
    ReleaseDC(NULL, dc);

    if(!det_registered){
        WNDCLASSEXA wc;
        memset(&wc, 0, sizeof(wc));
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = details_proc;
        wc.hInstance = hinst;
        wc.lpszClassName = "pfemu-details";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
        wc.hIcon = LoadIcon(hinst, MAKEINTRESOURCE(IDI_PFEMU));
        wc.hIconSm = (HICON)LoadImage(hinst, MAKEINTRESOURCE(IDI_PFEMU),
                                      IMAGE_ICON, 16, 16, 0);
        if(!RegisterClassExA(&wc)){ DeleteObject(d.hMono); return; }
        det_registered = 1;
    }
    if(owner && GetWindowRect(owner, &ro)){
        x = (int)ro.left + ((int)(ro.right-ro.left) - winw)/2;
        y = (int)ro.top + ((int)(ro.bottom-ro.top) - winh)/2;
    } else {
        x = (GetSystemMetrics(SM_CXSCREEN)-winw)/2;
        y = (GetSystemMetrics(SM_CYSCREEN)-winh)/2;
    }
    if(x < 0) x = 0;
    if(y < 0) y = 0;
    {   HWND hwnd = CreateWindowExA(0,"pfemu-details","pfemu - details",
                        WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU|WS_THICKFRAME,
                        x, y, winw, winh, owner, NULL, hinst, &d);
        if(!hwnd){ DeleteObject(d.hMono); return; }
        /* Modal by hand: the launcher has no dialog manager to do it. */
        EnableWindow(owner, FALSE);
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        SetFocus(d.hEdit);
        while(!d.done && GetMessageA(&msg, NULL, 0, 0) > 0){
            if(!IsDialogMessageA(hwnd, &msg)){
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
        }
    }
    EnableWindow(owner, TRUE);
    SetActiveWindow(owner);
    DeleteObject(d.hMono);
}

static LRESULT CALLBACK launch_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    LaunchState *st = (LaunchState*)(INT_PTR)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch(m){
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)l;
        HWND c;
        int i, y, gh, gy;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        st = (LaunchState*)cs->lpCreateParams;
        st->hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        y = 10;
        c = CreateWindowExA(0,"STATIC","Pinball Fantasies",WS_CHILD|WS_VISIBLE,
                            14,y,400,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y += 22;
        /* Game group: install picker (when needed) + read-only detection
         * line with Details for the full report. */
        gy = y;
        gh = (st->ninst > 1) ? 70 : 46;
        c = CreateWindowExA(0,"BUTTON","Game",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        if(st->ninst > 1){
            /* Picks a *directory*, not a version - what is in each one is
             * whatever its hashes said it is. */
            int k;
            c = CreateWindowExA(0,"STATIC","Installation:",WS_CHILD|WS_VISIBLE,
                                24,gy+20,80,16,h,0,cs->hInstance,0);
            SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
            st->hInstall = CreateWindowExA(0,"COMBOBOX","",
                                WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                                106,gy+18,292,240,h,(HMENU)ID_INSTALL,cs->hInstance,0);
            SendMessageA(st->hInstall,WM_SETFONT,(WPARAM)st->hFont,0);
            for(k=0;k<st->ninst;k++){
                char item[200];
                snprintf(item,sizeof(item),"%s  -  %s",
                         st->inst[k].dir, st->inst[k].summary);
                SendMessageA(st->hInstall,CB_ADDSTRING,0,(LPARAM)item);
            }
            SendMessageA(st->hInstall,CB_SETCURSEL,st->sel,0);
        } else {
            st->hInstall = NULL;
        }
        /* The detection result, read-only: the files decide, Details explains. */
        st->hDetected = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_ENDELLIPSIS,
                            24,gy+gh-22,296,16,h,0,cs->hInstance,0);
        SendMessageA(st->hDetected,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hDetails = CreateWindowExA(0,"BUTTON","Details",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            328,gy+gh-26,70,22,h,(HMENU)ID_DETAILS,cs->hInstance,0);
        SendMessageA(st->hDetails,WM_SETFONT,(WPARAM)st->hFont,0);
        y += gh + 8;
        /* Sound group: checkbox carries the hardware detail, the combo
         * carries the rates - no extra notes needed. */
        gy = y; gh = 104;
        c = CreateWindowExA(0,"BUTTON","Sound",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hSound = CreateWindowExA(0,"BUTTON","Sound on (SoundBlaster 220h / IRQ 7)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,gy+18,374,20,h,(HMENU)ID_SOUND,cs->hInstance,0);
        SendMessageA(st->hSound,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        /* Quality: SETSOUND's five-notch setting, written straight to
         * SOUND.CFG byte 0x14 for the driver to read. */
        c = CreateWindowExA(0,"STATIC","Quality:",WS_CHILD|WS_VISIBLE,
                            24,gy+44,70,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hQuality = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            100,gy+42,230,200,h,(HMENU)ID_QUALITY,cs->hInstance,0);
        SendMessageA(st->hQuality,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<5;i++)
            SendMessageA(st->hQuality,CB_ADDSTRING,0,(LPARAM)quality_labels[i]);
        SendMessageA(st->hQuality,CB_SETCURSEL,st->quality,0);
        /* Volume: host-side only, applied where waveOut is fed. */
        c = CreateWindowExA(0,"STATIC","Volume:",WS_CHILD|WS_VISIBLE,
                            24,gy+70,70,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hVolume = CreateWindowExA(0,TRACKBAR_CLASSA,"",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_HORZ|TBS_AUTOTICKS,
                            96,gy+66,190,28,h,(HMENU)ID_VOLUME,cs->hInstance,0);
        SendMessageA(st->hVolume,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
        SendMessageA(st->hVolume,TBM_SETTICFREQ,25,0);
        SendMessageA(st->hVolume,TBM_SETPAGESIZE,0,10);
        SendMessageA(st->hVolume,TBM_SETPOS,TRUE,st->volume);
        st->hVolLabel = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_RIGHT,
                            290,gy+70,44,16,h,(HMENU)ID_VOLLABEL,cs->hInstance,0);
        SendMessageA(st->hVolLabel,WM_SETFONT,(WPARAM)st->hFont,0);
        set_vol_label(st);
        y += gh + 8;
        /* Game options in two columns instead of six stacked rows. Row-major
         * order preserves the PINBALL.CFG layout: Balls|Angle, Scrolling|
         * Music, Resolution|Color. */
        gy = y; gh = 94;
        c = CreateWindowExA(0,"BUTTON","Game options",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<6;i++){
            int k, row = i / 2, col = i % 2;
            int lx = (col == 0) ? 24 : 218;
            int cx = (col == 0) ? 110 : 304;
            int ry = gy + 20 + row * 24;
            c = CreateWindowExA(0,"STATIC",opts[i].label,WS_CHILD|WS_VISIBLE,
                                lx,ry+3,82,16,h,0,cs->hInstance,0);
            SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
            st->hOpt[i] = CreateWindowExA(0,"COMBOBOX","",
                                WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
                                CBS_DROPDOWNLIST,
                                cx,ry,94,200,h,(HMENU)(INT_PTR)(ID_OPT_FIRST+i),cs->hInstance,0);
            SendMessageA(st->hOpt[i],WM_SETFONT,(WPARAM)st->hFont,0);
            for(k=0;k<opts[i].n;k++)
                SendMessageA(st->hOpt[i],CB_ADDSTRING,0,(LPARAM)opts[i].values[k]);
            SendMessageA(st->hOpt[i],CB_SETCURSEL,st->cfg[i],0);
        }
        y += gh + 8;
        /* Extras: trainer and fullscreen side by side, one shared hint line. */
        gy = y; gh = 86;
        c = CreateWindowExA(0,"BUTTON","Extras",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hCheatEnable = CreateWindowExA(0,"BUTTON","Enable trainer",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,gy+18,180,20,h,(HMENU)ID_CHEAT_ENABLE,cs->hInstance,0);
        SendMessageA(st->hCheatEnable,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_CHEAT_ENABLE,st->cheat_enable?BST_CHECKED:BST_UNCHECKED);
        st->hFullscreen = CreateWindowExA(0,"BUTTON","Start in fullscreen",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            218,gy+18,180,20,h,(HMENU)ID_FULLSCREEN,cs->hInstance,0);
        SendMessageA(st->hFullscreen,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_FULLSCREEN,st->fullscreen?BST_CHECKED:BST_UNCHECKED);
        /* Start at: skip the intro and the menu and boot straight into a
         * table.  The boot program still runs and stays resident, so
         * quitting the table lands on the menu as usual - see
         * docs/EMULATOR.md.  Greyed out in replay mode: the .pfr records how
         * its session began and that has to win. */
        c = CreateWindowExA(0,"STATIC","Start at:",WS_CHILD|WS_VISIBLE,
                            24,gy+45,70,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hTable = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            100,gy+42,230,200,h,(HMENU)ID_TABLE,cs->hInstance,0);
        SendMessageA(st->hTable,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<5;i++)
            SendMessageA(st->hTable,CB_ADDSTRING,0,(LPARAM)table_labels[i]);
        SendMessageA(st->hTable,CB_SETCURSEL,st->start_table,0);
        c = CreateWindowExA(0,"STATIC",
                            "Trainer: '1'-'3' toggle, arrows / 'Z' move the ball. Alt+Enter toggles fullscreen.",
                            WS_CHILD|WS_VISIBLE|SS_ENDELLIPSIS,24,gy+66,374,14,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y += gh + 8;
        /* Session record / replay (docs/REPLAY.md section 4): the group title
         * replaces the old "Session:" label; CLI -record/-replay are thin
         * frontends to the same emu-time injector. */
        gy = y; gh = 76;
        c = CreateWindowExA(0,"BUTTON","Session",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hModePlay = CreateWindowExA(0,"BUTTON","Play",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_GROUP|BS_AUTORADIOBUTTON,
                            24,gy+18,60,20,h,(HMENU)ID_MODE_PLAY,cs->hInstance,0);
        SendMessageA(st->hModePlay,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hModeRecord = CreateWindowExA(0,"BUTTON","Record",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,
                            94,gy+18,80,20,h,(HMENU)ID_MODE_RECORD,cs->hInstance,0);
        SendMessageA(st->hModeRecord,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hModeReplay = CreateWindowExA(0,"BUTTON","Replay",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,
                            184,gy+18,80,20,h,(HMENU)ID_MODE_REPLAY,cs->hInstance,0);
        SendMessageA(st->hModeReplay,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_MODE_PLAY,st->mode==LAUNCH_PLAY?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,ID_MODE_RECORD,st->mode==LAUNCH_RECORD?BST_CHECKED:BST_UNCHECKED);
        CheckDlgButton(h,ID_MODE_REPLAY,st->mode==LAUNCH_REPLAY?BST_CHECKED:BST_UNCHECKED);
        st->hPathLabel = CreateWindowExA(0,"STATIC","File:",WS_CHILD|WS_VISIBLE,
                            24,gy+46,32,16,h,0,cs->hInstance,0);
        SendMessageA(st->hPathLabel,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hPath = CreateWindowExA(0,"EDIT","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_BORDER|ES_AUTOHSCROLL,
                            60,gy+44,268,22,h,(HMENU)ID_REPLAY_PATH,cs->hInstance,0);
        SendMessageA(st->hPath,WM_SETFONT,(WPARAM)st->hFont,0);
        if(st->replay_path[0]) SetWindowTextA(st->hPath, st->replay_path);
        st->hBrowse = CreateWindowExA(0,"BUTTON","Browse...",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            334,gy+44,64,22,h,(HMENU)ID_BROWSE,cs->hInstance,0);
        SendMessageA(st->hBrowse,WM_SETFONT,(WPARAM)st->hFont,0);
        y += gh + 12;
        c = CreateWindowExA(0,"BUTTON","Launch",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                            260,y,76,24,h,(HMENU)ID_LAUNCH,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Quit",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            344,y,76,24,h,(HMENU)ID_QUIT,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        apply_mode_ui(h, st);   /* needs the Launch button to exist */
        return 0; }
    /* The trackbar reports through WM_HSCROLL, not WM_COMMAND. */
    case WM_HSCROLL:
        if(st && st->hVolume && (HWND)l == st->hVolume){
            st->volume = (int)SendMessageA(st->hVolume,TBM_GETPOS,0,0);
            set_vol_label(st);
        }
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(w);
        if(id==ID_SOUND){
            st->sound = IsDlgButtonChecked(h,ID_SOUND)==BST_CHECKED;
        } else if(id==ID_TABLE && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hTable,CB_GETCURSEL,0,0);
            if(sel != CB_ERR) st->start_table = (int)sel;
            return 0;
        } else if(id==ID_QUALITY && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hQuality,CB_GETCURSEL,0,0);
            if(sel != CB_ERR) st->quality = (int)sel;
        } else if(id==ID_CHEAT_ENABLE){
            st->cheat_enable = IsDlgButtonChecked(h,ID_CHEAT_ENABLE)==BST_CHECKED;
        } else if(id==ID_FULLSCREEN){
            st->fullscreen = IsDlgButtonChecked(h,ID_FULLSCREEN)==BST_CHECKED;
        } else if(id==ID_INSTALL && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hInstall,CB_GETCURSEL,0,0);
            if(sel != CB_ERR){ st->sel = (int)sel; reload_for_dir(h, st); }
        } else if(id==ID_DETAILS){
            /* One report, built for whatever mode the dialog is in, shown
             * in the modal above.  static: the composed text plus its CRLF
             * copy is more than a window procedure's stack wants to carry. */
            static char raw[DET_MAX];
            static char text[DET_MAX*2];
            details_build(st, raw, sizeof(raw));
            det_crlf(raw, text, sizeof(text));
            show_details(h, text);
        } else if(id==ID_MODE_PLAY || id==ID_MODE_RECORD || id==ID_MODE_REPLAY){
            st->mode = (id==ID_MODE_RECORD) ? LAUNCH_RECORD :
                       (id==ID_MODE_REPLAY) ? LAUNCH_REPLAY : LAUNCH_PLAY;
            apply_mode_ui(h, st);
        } else if(id==ID_BROWSE){
            OPENFILENAMEA ofn;
            char file[512];
            memset(&ofn, 0, sizeof(ofn));
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = h;
            ofn.lpstrFilter = "Pinball replays (*.pfr)\0*.pfr\0All files (*.*)\0*.*\0";
            ofn.nFilterIndex = 1;
            snprintf(file, sizeof(file), "%s", st->replay_path);
            ofn.lpstrFile = file;
            ofn.nMaxFile = (DWORD)sizeof(file);
            ofn.lpstrDefExt = "pfr";
            /* OFN_NOCHANGEDIR is load-bearing: without it the dialog leaves
             * the process CWD in the picked file's directory, and every
             * install dir (stored relative: "FANTASY", ...) stops resolving
             * the moment main() runs detection - a replay picked from
             * anywhere but here then "quits on Launch" with only a stderr
             * message nobody can see. */
            if(st->mode == LAUNCH_REPLAY){
                ofn.Flags = OFN_FILEMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
                if(!GetOpenFileNameA(&ofn)) return 0;
            } else {
                ofn.Flags = OFN_OVERWRITEPROMPT | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
                if(!GetSaveFileNameA(&ofn)) return 0;
            }
            snprintf(st->replay_path, sizeof(st->replay_path), "%s", file);
            st->path_custom = 1;
            if(st->hPath){
                st->updating_path = 1;
                SetWindowTextA(st->hPath, st->replay_path);
                st->updating_path = 0;
            }
            if(st->mode == LAUNCH_REPLAY) replay_load_file(h, st);
            else show_detection(h, st);
        } else if(id==ID_REPLAY_PATH && HIWORD(w)==EN_CHANGE){
            if(st->updating_path) return 0;
            if(st->hPath){
                GetWindowTextA(st->hPath, st->replay_path, (int)sizeof(st->replay_path));
                st->path_custom = 1;
            }
            if(st->mode == LAUNCH_REPLAY) replay_load_file(h, st);
            else show_detection(h, st);
        } else if(id==ID_LAUNCH){
            int i;
            const RelResult *r = cur_inst(st);
            const char *dir;
            if(st->mode == LAUNCH_REPLAY){
                /* No config writes in replay mode: the install's files are
                 * the session's inputs, and replay promises never to write
                 * the real overlay (REPLAY.md 3.3 - DOS writes instead go
                 * to the temp copy).  The volume slider is exempt: host
                 * gain only, never recorded - but even it is not persisted
                 * here; main() skips the exit save on replay too. */
                char why[256];
                if(!replay_selection_ok(st, why, sizeof(why))){
                    MessageBoxA(h, why, "pfemu - cannot replay",
                                MB_OK|MB_ICONEXCLAMATION);
                    return 0;
                }
                write_session_path(cur_game_dir(st), st->replay_path);
                st->ok = 1; st->done = 1;
                DestroyWindow(h);
                return 0;
            }
            if(!release_runnable(r)) return 0;
            if(st->mode == LAUNCH_RECORD && !st->replay_path[0]){
                MessageBoxA(h, "Pick a target .pfr file first.",
                            "pfemu - cannot record", MB_OK|MB_ICONEXCLAMATION);
                return 0;
            }
            if(st->mode == LAUNCH_RECORD){
                /* The save dialog already appends .pfr, but a typed path may
                 * lack it - normalise so records stay findable/filterable. */
                const char *base = strrchr(st->replay_path, '\\');
                const char *base2 = strrchr(st->replay_path, '/');
                if(base2 && (!base || base2 > base)) base = base2;
                if(!base) base = st->replay_path;
                if(!strchr(base, '.') &&
                   strlen(st->replay_path) + 4 < sizeof(st->replay_path))
                    strcat(st->replay_path, ".pfr");
            }
            dir = r->dir;
            { LRESULT sel = SendMessageA(st->hQuality,CB_GETCURSEL,0,0);
              if(sel != CB_ERR) st->quality = (int)sel; }
            write_sound_cfg(dir, st->sound, st->quality);
            write_audio_cfg(dir, st->volume, st->quality);
            for(i=0;i<6;i++){
                LRESULT sel = SendMessageA(st->hOpt[i],CB_GETCURSEL,0,0);
                st->cfg[i] = (uint8_t)(sel==CB_ERR ? cfg_pinball_defaults[i] : sel);
            }
            write_pinball_cfg(dir, st->cfg);
            if(st->mode == LAUNCH_RECORD)
                /* The trainer is incompatible with recording: the checkbox
                 * was unchecked and greyed on mode entry, and main()
                 * refuses as a backstop.  The session runs with it off. */
                write_cheats_cfg(dir, 0, 0);
            else
                write_cheats_cfg(dir, st->cheat_enable, st->cheat_enable);
            write_fullscreen_cfg(dir, st->fullscreen);
            write_table_cfg(dir, st->start_table);
            if(st->mode == LAUNCH_RECORD)
                write_session_path(dir, st->replay_path);
            st->ok = 1; st->done = 1;
            DestroyWindow(h);
        } else if(id==ID_QUIT){
            st->ok = 0; st->done = 1;
            DestroyWindow(h);
        }
        return 0; }
    case WM_CLOSE:
        if(st){ st->ok = 0; st->done = 1; }
        DestroyWindow(h);
        return 0;
    }
    return DefWindowProcA(h,m,w,l);
}

/* The installation the last showing launched.  main() comes back here when
 * it has to refuse a launch, and the picker should reopen on what was
 * chosen rather than on the first runnable install. */
static char last_dir[512] = "";

/* Modal launcher.  Returns 1 with *out filled when the user picks Launch,
 * 0 when they quit (caller should exit without booting).  It can be shown
 * more than once per run: a refused launch returns to it. */
int show_launcher(LaunchChoice *out){
    WNDCLASSEXA wc;
    HWND hwnd;
    MSG msg;
    /* static: it now carries a RelResult per installation, which is a lot of
     * report text to put on the stack for a dialog that runs once. */
    static LaunchState st;
    int sw, sh;
    int winw = 458, winh = 562;   /* Extras grew a row for "Start at" */
    INITCOMMONCONTROLSEX icc;
    memset(&wc,0,sizeof(wc));
    wc.cbSize = sizeof(wc);
    memset(&st,0,sizeof(st));
    /* The volume slider is a common control; without this its window class
     * is not registered and CreateWindowEx for it just returns NULL. */
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES;
    InitCommonControlsEx(&icc);
    wc.lpfnWndProc = launch_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "pfemu-launcher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    /* Same app icon as the game window (res/pfemu.ico, IDI_PFEMU). */
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_PFEMU));
    wc.hIconSm = (HICON)LoadImage(wc.hInstance, MAKEINTRESOURCE(IDI_PFEMU),
                                  IMAGE_ICON, 16, 16, 0);
    RegisterClassExA(&wc);
    /* Every directory that holds an INTRO.PRG, identified by content.  GAME
     * comes first when it exists, then the rest alphabetically, so the
     * default selection is stable rather than filesystem-order. */
    st.ninst = release_scan(st.inst, MAX_INSTALLS);
    st.sel = 0;
    /* Prefer landing on one that can actually be launched. */
    { int i;
      for(i=0;i<st.ninst;i++)
          if(release_runnable(&st.inst[i])){ st.sel = i; break; } }
    /* ...but a second showing (main() comes back here when a launch was
     * refused) lands on whatever was picked last, or the user would have to
     * find their installation again every time something is refused. */
    if(last_dir[0]){ int i;
      for(i=0;i<st.ninst;i++)
          if(!_stricmp(st.inst[i].dir, last_dir)){ st.sel = i; break; } }
    if(st.ninst > 1) winh += 24;
    { const char *dir = cur_game_dir(&st);
      int balls, spring;
      st.sound = read_sound_is_sb(dir);
      st.quality = read_sound_quality(dir);
      st.volume = read_volume_cfg(dir);
      read_pinball_cfg(dir, st.cfg);
      read_cheats_cfg(dir, &balls, &spring);
      st.cheat_enable = balls || spring;
      st.fullscreen = read_fullscreen_cfg(dir);
      st.start_table = read_table_cfg(dir); }
    hwnd = CreateWindowExA(0,"pfemu-launcher","pfemu launcher",
                           WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
                           CW_USEDEFAULT,CW_USEDEFAULT,winw,winh,
                           NULL,NULL,wc.hInstance,&st);
    if(!hwnd) return 0;
    sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd,NULL,(sw-winw)/2,(sh-winh)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
    ShowWindow(hwnd,SW_SHOW);
    UpdateWindow(hwnd);
    while(!st.done && GetMessageA(&msg,NULL,0,0)>0){
        if(!IsDialogMessageA(hwnd,&msg)){
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
        }
    }
    UnregisterClassA("pfemu-launcher",wc.hInstance);
    if(det_registered){ UnregisterClassA("pfemu-details",wc.hInstance); det_registered = 0; }
    if(!st.ok) return 0;
    {   /* The boot program comes from the detected release, not from a
         * constant: Power Pack's is PF.EXE, the other two ship PINBALL.EXE.
         * ID_LAUNCH already refused anything not recognised. */
        const RelResult *r = cur_inst(&st);
        snprintf(out->dir, sizeof(out->dir), "%s", r->dir);
        snprintf(out->prog, sizeof(out->prog), "%s",
                 r->boot[0] ? r->boot : r->rel->boot);
        snprintf(last_dir, sizeof(last_dir), "%s", r->dir);
    }
    out->fullscreen = st.fullscreen;
    out->start_table = st.start_table;
    out->mode = st.mode;
    snprintf(out->replay_path, sizeof(out->replay_path), "%s", st.replay_path);
    return 1;
}
