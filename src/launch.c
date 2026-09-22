/* Win32 launcher: startup dialog for Pinball Fantasies.
 *
 * Lets the user turn SoundBlaster sound on/off (writes SOUND.CFG, read
 * directly by the game) and set every option from the game's own F5
 * in-game menu, without ever having to open that menu.  Unlike SOUND.CFG,
 * the options are NOT written to PINBALL.CFG for the game to read - see the
 * big "launcher options I/O" comment below for why, and src/fantasies.c
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
#define ID_BASS         131
#define ID_TREBLE       132
#define ID_OOMPH        133
#define ID_HEADPHONE    134

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
 * further.)  So the launcher writes these bytes to PFEMU-STATE/pfemu.cfg, a
 * host-only settings file DOS never opens; src/fantasies.c makes every
 * boot-time PINBALL.CFG open fail like a fresh install always has, and
 * pokes these bytes into INTRO.PRG's own buffer at that moment instead -
 * zero extra guest instructions, so boot timing stays identical to the one
 * case already proven reliable.
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
 * PINBALL.CFG on disk boots with the buffer already at 00 00 01 00 00 00) -
 * cfg_option_defaults in src/cfg.c, which every reader of the file shares. */

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
static int read_trainer_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.trainer;
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

/* Enhancement combo maps: bass/treble in 3 dB steps, oomph off then up. */
static const char *eq_labels[9] = {
    "-12 dB", "-9 dB", "-6 dB", "-3 dB", "Flat",
    "+3 dB", "+6 dB", "+9 dB", "+12 dB"
};
static const char *oomph_labels[5] = {
    "Off", "+3 dB", "+6 dB", "+9 dB", "+12 dB"
};
static int eq_idx_to_db(int idx){
    if(idx < 0) idx = 4;
    if(idx > 8) idx = 8;
    return (idx - 4) * 3;
}
static int eq_db_to_idx(int db){
    int i = (db + 12 + 1) / 3;
    if(i < 0) i = 0;
    if(i > 8) i = 8;
    return i;
}
static int oomph_idx_to_db(int idx){
    if(idx < 0) idx = 0;
    if(idx > 4) idx = 4;
    return idx * 3;
}
static int oomph_db_to_idx(int db){
    int i = (db + 1) / 3;
    if(i < 0) i = 0;
    if(i > 4) i = 4;
    return i;
}

typedef struct {
    int sound;              /* checkbox state */
    int quality;            /* combo state: SOUND.CFG quality notch, 0-4 */
    int volume;             /* slider state: host output gain, 0-100 */
    int bass;               /* combo state: EQ bass shelf dB, -12..+12 */
    int treble;             /* combo state: EQ treble shelf dB, -12..+12 */
    int oomph;              /* combo state: extra low-bass dB, 0..+12 */
    int headphone;          /* checkbox state: headphone pseudo-stereo */
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
    HWND hBass, hTreble, hOomph, hHeadphone;
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

/* "Start at" per install.  Host-only, like everything else in pfemu.cfg:
 * the guest never sees it, and a release whose INT 65h layout cannot be
 * derived just falls back to the menu at boot (src/fantasies.c). */
static int read_table_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    return c.start_table;
}

/* Last-used session file per install (record target or replay source).
 * Restored into the dialog so a replay file doesn't have to be re-picked
 * every time; overwritten on each successful Launch in record/replay mode.
 * Display/hint only - replay identity still comes from the file header. */
static void read_session_path(const char *dir, char *dst, size_t n){
    PfCfg c;
    cfg_read(dir, &c);
    snprintf(dst, n, "%s", c.session);
}

static void write_session_path(const char *dir, const char *p){
    PfCfg c;
    if(!dir || !dir[0] || !p || !p[0]) return;
    cfg_read(dir, &c);
    snprintf(c.session, sizeof(c.session), "%s", p);
    cfg_write(dir, &c);
}

/* Forward: replay_autorestore()/apply_mode_ui() below call these, which are
 * defined further down next to the controls they update. */
static void show_detection(HWND h, LaunchState *st);
static void reload_for_dir(HWND h, LaunchState *st);
static void launch_save_pos(HWND h);

/* Default record target: sessions/<install>_<date>.pfr (REPLAY.md section
 * 4), next to pfemu.exe.  The install dir is sanitised: it is only ever a
 * plain directory name, but never trust a filename you did not build.
 *
 * Rebuilt on every entry into record mode, so the timestamp in the name is
 * the recording's own.  A second-resolution stamp can still repeat if
 * record mode is re-entered within the same second, so an existing file
 * gets _2, _3, ... rather than being overwritten: a recording is a
 * playthrough that cannot be reproduced, and one lost to a name clash is
 * gone for good. */
static void default_record_path(LaunchState *st){
    SYSTEMTIME t;
    char safe[64], stem[480];
    size_t i;
    int n;
    const char *dir = cur_game_dir(st);
    GetLocalTime(&t);
    for(i=0;i<sizeof(safe)-1 && dir[i];i++){
        char c = dir[i];
        safe[i] = (c=='\\'||c=='/'||c==':'||c==' ') ? '_' : c;
    }
    safe[i] = 0;
    if(!safe[0]) snprintf(safe, sizeof(safe), "GAME");
    snprintf(stem, sizeof(stem),
             "sessions\\%s_%04d%02d%02d_%02d%02d%02d",
             safe, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    snprintf(st->replay_path, sizeof(st->replay_path), "%s.pfr", stem);
    for(n=2; n<100; n++){
        FILE *f = fopen(st->replay_path, "rb");
        if(!f) break;
        fclose(f);
        snprintf(st->replay_path, sizeof(st->replay_path), "%s_%d.pfr", stem, n);
    }
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
    if(read_trainer_cfg(r->dir)){
        if(why) snprintf(why, n, "Trainer is enabled for '%s' - replay needs it off.", r->dir);
        return 0;
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

/* Fill the path field on mode entry: a fresh record target in record mode,
 * else the install's last-used session file, else empty.  Never clobbers a
 * user-typed/picked path.  Install switches keep the current path, except
 * record mode which re-targets to the new install - a record target naming
 * another install would only confuse. */
static void restore_session_path(HWND h, LaunchState *st, int install_switch){
    char saved[512];
    if(st->path_custom) return;
    if(install_switch && st->mode == LAUNCH_REPLAY) return;
    /* Record always gets a fresh target.  The remembered path is a
     * convenience for replay - it pre-fills the file you last recorded or
     * replayed - but as a record target it aims the next recording at the
     * last one and overwrites it, wearing that session's timestamp while
     * it does so. */
    read_session_path(cur_game_dir(st), saved, sizeof(saved));
    if(st->mode == LAUNCH_RECORD) default_record_path(st);
    else if(saved[0]) snprintf(st->replay_path, sizeof(st->replay_path), "%s", saved);
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
        st->cheat_enable = 0;
        CheckDlgButton(h, ID_CHEAT_ENABLE, BST_UNCHECKED);
        if(st->hCheatEnable) EnableWindow(st->hCheatEnable, FALSE);
        /* A saved trainer-on must not survive into the session either: the
         * mode owns it, so it goes off on commit (see ID_LAUNCH). */
        restore_session_path(h, st, 0);
    } else {
        if(st->hCheatEnable) EnableWindow(st->hCheatEnable, TRUE);
        /* Leaving record/replay restores the saved trainer state into the
         * checkbox; play mode neither forces nor forbids it. */
        st->cheat_enable = read_trainer_cfg(cur_game_dir(st));
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
    PfCfg c;
    int i;
    cfg_read(dir, &c);
    st->sound = read_sound_is_sb(dir);
    st->quality = read_sound_quality(dir);   /* SOUND.CFG wins over c.quality */
    st->volume = c.volume;
    st->bass = c.bass;
    st->treble = c.treble;
    st->oomph = c.oomph;
    st->headphone = c.headphone;
    st->cheat_enable = c.trainer;
    st->fullscreen = c.fullscreen;
    if(st->hSound){
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        SendMessageA(st->hQuality,CB_SETCURSEL,st->quality,0);
        SendMessageA(st->hVolume,TBM_SETPOS,TRUE,st->volume);
        set_vol_label(st);
        SendMessageA(st->hBass,CB_SETCURSEL,eq_db_to_idx(st->bass),0);
        SendMessageA(st->hTreble,CB_SETCURSEL,eq_db_to_idx(st->treble),0);
        SendMessageA(st->hOomph,CB_SETCURSEL,oomph_db_to_idx(st->oomph),0);
        CheckDlgButton(h,ID_HEADPHONE,st->headphone?BST_CHECKED:BST_UNCHECKED);
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
        /* Enhancement: host DSP only (src/sound.c), downstream of -wav.
         * Flat/Off is the old sound; like volume, never recorded. */
        gy = y; gh = 70;
        c = CreateWindowExA(0,"BUTTON","Audio enhancement",WS_CHILD|WS_VISIBLE|BS_GROUPBOX,
                            12,gy,416,gh,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"STATIC","Bass:",WS_CHILD|WS_VISIBLE,
                            24,gy+23,52,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hBass = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            76,gy+20,110,200,h,(HMENU)ID_BASS,cs->hInstance,0);
        SendMessageA(st->hBass,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<9;i++)
            SendMessageA(st->hBass,CB_ADDSTRING,0,(LPARAM)eq_labels[i]);
        SendMessageA(st->hBass,CB_SETCURSEL,eq_db_to_idx(st->bass),0);
        c = CreateWindowExA(0,"STATIC","Treble:",WS_CHILD|WS_VISIBLE,
                            218,gy+23,52,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hTreble = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            272,gy+20,110,200,h,(HMENU)ID_TREBLE,cs->hInstance,0);
        SendMessageA(st->hTreble,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<9;i++)
            SendMessageA(st->hTreble,CB_ADDSTRING,0,(LPARAM)eq_labels[i]);
        SendMessageA(st->hTreble,CB_SETCURSEL,eq_db_to_idx(st->treble),0);
        c = CreateWindowExA(0,"STATIC","Oomph:",WS_CHILD|WS_VISIBLE,
                            24,gy+47,52,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hOomph = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            76,gy+44,110,200,h,(HMENU)ID_OOMPH,cs->hInstance,0);
        SendMessageA(st->hOomph,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<5;i++)
            SendMessageA(st->hOomph,CB_ADDSTRING,0,(LPARAM)oomph_labels[i]);
        SendMessageA(st->hOomph,CB_SETCURSEL,oomph_db_to_idx(st->oomph),0);
        st->hHeadphone = CreateWindowExA(0,"BUTTON","Headphone mode",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            218,gy+44,164,20,h,(HMENU)ID_HEADPHONE,cs->hInstance,0);
        SendMessageA(st->hHeadphone,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_HEADPHONE,st->headphone?BST_CHECKED:BST_UNCHECKED);
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
        } else if(id==ID_BASS && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hBass,CB_GETCURSEL,0,0);
            if(sel != CB_ERR) st->bass = eq_idx_to_db((int)sel);
        } else if(id==ID_TREBLE && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hTreble,CB_GETCURSEL,0,0);
            if(sel != CB_ERR) st->treble = eq_idx_to_db((int)sel);
        } else if(id==ID_OOMPH && HIWORD(w)==CBN_SELCHANGE){
            LRESULT sel = SendMessageA(st->hOomph,CB_GETCURSEL,0,0);
            if(sel != CB_ERR) st->oomph = oomph_idx_to_db((int)sel);
        } else if(id==ID_HEADPHONE){
            st->headphone = IsDlgButtonChecked(h,ID_HEADPHONE)==BST_CHECKED;
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
            { LRESULT sel = SendMessageA(st->hBass,CB_GETCURSEL,0,0);
              if(sel != CB_ERR) st->bass = eq_idx_to_db((int)sel); }
            { LRESULT sel = SendMessageA(st->hTreble,CB_GETCURSEL,0,0);
              if(sel != CB_ERR) st->treble = eq_idx_to_db((int)sel); }
            { LRESULT sel = SendMessageA(st->hOomph,CB_GETCURSEL,0,0);
              if(sel != CB_ERR) st->oomph = oomph_idx_to_db((int)sel); }
            write_sound_cfg(dir, st->sound, st->quality);
            for(i=0;i<6;i++){
                LRESULT sel = SendMessageA(st->hOpt[i],CB_GETCURSEL,0,0);
                st->cfg[i] = (uint8_t)(sel==CB_ERR ? cfg_option_defaults[i] : sel);
            }
            /* Everything the dialog owns lands in one file, in one write
             * (src/cfg.c).  Read first so a key this dialog does not show
             * survives, and because that is what imports an install still
             * carrying the old per-setting files. */
            { PfCfg c;
              cfg_read(dir, &c);
              c.volume = st->volume;
              c.bass = st->bass;
              c.treble = st->treble;
              c.oomph = st->oomph;
              c.headphone = st->headphone != 0;
              c.quality = st->quality;
              memcpy(c.options, st->cfg, 6);
              /* The trainer is incompatible with recording: the checkbox
               * was unchecked and greyed on mode entry, and main() refuses
               * as a backstop.  The session runs with it off. */
              c.trainer = (st->mode == LAUNCH_RECORD) ? 0 : (st->cheat_enable != 0);
              c.fullscreen = st->fullscreen != 0;
              c.start_table = st->start_table;
              if(st->mode == LAUNCH_RECORD && st->replay_path[0])
                  snprintf(c.session, sizeof(c.session), "%s", st->replay_path);
              cfg_write(dir, &c); }
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
    /* Persist the dialog position.  WM_DESTROY fires on every teardown path
     * (Launch, Quit, X), so one hook here covers them all; EXITSIZEMOVE
     * covers a kill/crash after a drag.  Read under the unaware context the
     * window was created in: mixing aware-thread geometry with this window
     * rescales the coordinates, so the save would never match the restore. */
    case WM_DESTROY:
        launch_save_pos(h);
        return 0;
    case WM_EXITSIZEMOVE:
        launch_save_pos(h);
        return 0;
    }
    return DefWindowProcA(h,m,w,l);
}

/* Current dialog position into the global file, read the way it was
 * written: unaware, like the window (see show_launcher). */
static void launch_save_pos(HWND h){
    HMODULE u = GetModuleHandleA("user32.dll");
    void *(WINAPI *setthread)(void*) = u ?
        (void*(WINAPI*)(void*))GetProcAddress(u, "SetThreadDpiAwarenessContext") : NULL;
    void *prev = setthread ? setthread((void*)(INT_PTR)-1) : NULL;
    RECT rc;
    if(GetWindowRect(h, &rc))
        last_save_launchpos((int)rc.left, (int)rc.top);
    if(setthread) setthread(prev ? prev : (void*)(INT_PTR)-4);
}

/* The installation the last showing launched.  main() comes back here when
 * it has to refuse a launch, and the picker should reopen on what was
 * chosen rather than on the first runnable install. */
static char last_dir[512] = "";

/* Persistent memory of the last launched installation, across runs.
 * One tiny host-only file next to the exe (a file can never be mistaken
 * for an installation - release_scan() only looks at directories): the
 * directory as picked plus the release id it detected as, so a renamed
 * folder can still land on the same version.  Missing or unreadable just
 * reads as "no memory", exactly like a first run - these are preferences,
 * never a reason to refuse.  Deliberately not called pfemu.cfg: that name
 * is already the per-install settings file (PFEMU-STATE/pfemu.cfg), and
 * two different files sharing it would only confuse. */
#define LAST_FILE "pfemu-last.cfg"
/* Window positions live apart from install memory on purpose
 * (pfemu-winpos.cfg): a read that finds nothing must only mean "center the
 * window", never wipe the remembered installation - and vice versa.  Two
 * tiny files that cannot clobber each other beat one clever one. */
#define LAST_WINPOS "pfemu-winpos.cfg"

/* Absolute path next to the exe.  A relative path would follow the process
 * CWD, which is not stable: a shortcut's "Start in" directory, a CLI run
 * from another folder, or any future file dialog can point launch-time and
 * quit-time at two different files, so a position saved on quit is never
 * found again on launch (or vice versa).  The exe's own directory never
 * moves under a running process. */
static void last_path(char *out, size_t n, const char *name){
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
                snprintf(out + dlen, n - dlen, "\\%s", name);
                return;
            }
        }
    }
    snprintf(out, n, "%s", name);   /* degraded, but never a failure */
}

/* Install memory only: dir + release.  Written outright on Launch, so there
 * is no read whose failure could ever blank it. */
static void last_save(const RelResult *r){
    char path[1080];
    FILE *f;
    if(!r || !r->dir[0]) return;
    last_path(path, sizeof(path), LAST_FILE);
    f = fopen(path, "w");
    if(!f) return;
    fprintf(f, "# Last installation launched from the pfemu launcher.\n");
    fprintf(f, "dir=%s\n", r->dir);
    fprintf(f, "release=%s\n", r->rel ? r->rel->id : "");
    fclose(f);
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
    last_path(path, sizeof(path), LAST_WINPOS);
    f = fopen(path, "r");
    if(!f) return;
    while(fgets(line, sizeof(line), f)){
        char *k = line, *eq, *e;
        while(*k==' '||*k=='\t') k++;
        e = k + strlen(k);
        while(e > k && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
        if(!*k || *k=='#' || *k==';') continue;
        eq = strchr(k, '=');
        if(!eq) continue;
        *eq = 0;
        { char *v = eq + 1;
          while(*v==' '||*v=='\t') v++;
          if(!strcmp(k, "win_x")){ o->win_x = atoi(v); o->have_win |= 1; }
          else if(!strcmp(k, "win_y")){ o->win_y = atoi(v); o->have_win |= 2; }
          else if(!strcmp(k, "launch_x")){ o->launch_x = atoi(v); o->have_launch |= 1; }
          else if(!strcmp(k, "launch_y")){ o->launch_y = atoi(v); o->have_launch |= 2; } }
    }
    fclose(f);
    if(o->have_win != 3) o->have_win = 0;   /* need the full pair to restore */
    if(o->have_launch != 3) o->have_launch = 0;
}

static void winpos_write(const WinPos *o){
    char path[1080];
    FILE *f;
    last_path(path, sizeof(path), LAST_WINPOS);
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

/* Center a w*h window in the work area of the monitor with the mouse cursor.
 * That is the screen the user is looking at; the primary monitor's full size
 * is the wrong answer twice over on a multi-monitor desk (wrong monitor, and
 * the centered rect ends up under the taskbar).  Falls back to the primary
 * monitor when the cursor position is unavailable. */
void center_on_cursor_monitor(int w, int h, int *ox, int *oy){
    POINT pt;
    HMONITOR hm = NULL;
    MONITORINFO mi;
    int sw, sh;
    if(GetCursorPos(&pt))
        hm = MonitorFromPoint(pt, MONITOR_DEFAULTTONULL);
    if(!hm)
        hm = MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    mi.cbSize = sizeof(mi);
    if(hm && GetMonitorInfoA(hm, &mi)){
        int aw = mi.rcWork.right - mi.rcWork.left;
        int ah = mi.rcWork.bottom - mi.rcWork.top;
        int x = mi.rcWork.left + (aw - w) / 2;
        int y = mi.rcWork.top + (ah - h) / 2;
        if(x < mi.rcWork.left) x = mi.rcWork.left;
        if(y < mi.rcWork.top) y = mi.rcWork.top;
        if(ox) *ox = x;
        if(oy) *oy = y;
        return;
    }
    sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    if(ox) *ox = (sw - w) / 2 < 0 ? 0 : (sw - w) / 2;
    if(oy) *oy = (sh - h) / 2 < 0 ? 0 : (sh - h) / 2;
}

/* Point st->sel at the remembered installation when it is still there:
 * the same directory wins outright (even if what is in it changed - the
 * detection line then says so), otherwise the first runnable install of
 * the remembered release.  Returns 1 when it moved the selection. */
static int last_restore(LaunchState *st){
    char line[600], path[1080], dir[512] = "", rel[64] = "";
    FILE *f;
    int i;
    last_path(path, sizeof(path), LAST_FILE);
    f = fopen(path, "r");
    if(!f) return 0;
    while(fgets(line, sizeof(line), f)){
        char *k = line, *eq, *e;
        while(*k==' '||*k=='\t') k++;
        e = k + strlen(k);
        while(e > k && (e[-1]==' '||e[-1]=='\t'||e[-1]=='\r'||e[-1]=='\n')) *--e = 0;
        if(!*k || *k=='#' || *k==';') continue;
        eq = strchr(k, '=');
        if(!eq) continue;
        *eq = 0;
        { char *v = eq + 1;
          while(*v==' '||*v=='\t') v++;
          if(!strcmp(k, "dir")) snprintf(dir, sizeof(dir), "%s", v);
          else if(!strcmp(k, "release")) snprintf(rel, sizeof(rel), "%s", v); }
    }
    fclose(f);
    if(dir[0]){
        for(i=0;i<st->ninst;i++)
            if(!_stricmp(st->inst[i].dir, dir)){ st->sel = i; return 1; }
    }
    if(rel[0]){
        for(i=0;i<st->ninst;i++)
            if(release_runnable(&st->inst[i]) && st->inst[i].rel &&
               !_stricmp(st->inst[i].rel->id, rel)){ st->sel = i; return 1; }
    }
    return 0;
}

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
    int winw = 458, winh = 640;   /* Extras grew a row for "Start at",
                                   then Enhancement added its own group */
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
    /* ...but the installation launched last time wins when it is still
     * there (same directory, else the same release elsewhere) - its
     * PFEMU-STATE/pfemu.cfg is then what the dialog below loads, so the
     * settings come back with it. */
    last_restore(&st);
    /* ...but a second showing (main() comes back here when a launch was
     * refused) lands on whatever was picked last, or the user would have to
     * find their installation again every time something is refused. */
    if(last_dir[0]){ int i;
      for(i=0;i<st.ninst;i++)
          if(!_stricmp(st.inst[i].dir, last_dir)){ st.sel = i; break; } }
    if(st.ninst > 1) winh += 24;
    { const char *dir = cur_game_dir(&st);
      PfCfg c;
      cfg_read(dir, &c);
      st.sound = read_sound_is_sb(dir);
      st.quality = read_sound_quality(dir);
      st.volume = c.volume;
      st.bass = c.bass;
      st.treble = c.treble;
      st.oomph = c.oomph;
      st.headphone = c.headphone;
      memcpy(st.cfg, c.options, 6);
      st.cheat_enable = c.trainer;
      st.fullscreen = c.fullscreen;
      st.start_table = c.start_table; }
    /* The picker is born DPI-virtualized (the pre-awareness behavior): its
     * layout is fixed pixels under a DPI-scaled system font, so full
     * awareness clips it whenever the font outgrows the boxes.  Set the
     * THREAD context around creation - changing an existing window's
     * context resizes it to logical units and reproduces the cutoff.  The
     * game window stays per-monitor aware (physical pixels throughout). */
    { HMODULE u = GetModuleHandleA("user32.dll");
      void *(WINAPI *setthread)(void*) = u ?
          (void*(WINAPI*)(void*))GetProcAddress(u, "SetThreadDpiAwarenessContext") : NULL;
      /* UNAWARE = (HANDLE)-1, PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
      void *prev = setthread ? setthread((void*)(INT_PTR)-1) : NULL;
      /* Explicit creation coordinates, never CW_USEDEFAULT: a window born
       * with CW_USEDEFAULT keeps a pending cascade that first-show can
       * apply over a pre-show SetWindowPos (observed: the dialog drifted
       * off the restored spot on its own).  Everything down to ShowWindow
       * runs unaware, so creation, MonitorFromPoint, GetWindowRect and
       * SetWindowPos all share the unaware window's logical pixels - mixing
       * aware-thread geometry in here rescales the coordinates and the save
       * would never match the restore. */
      { int cx, cy, have_cx = 0;
        if(last_read_launchpos(&cx, &cy)){
            HMONITOR hm0 = MonitorFromPoint(
                (POINT){cx + winw/2, cy + 12}, MONITOR_DEFAULTTONULL);
            if(hm0) have_cx = 1;
        }
        if(!have_cx) center_on_cursor_monitor(winw, winh, &cx, &cy);
        hwnd = CreateWindowExA(0,"pfemu-launcher","pfemu launcher",
                               WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
                               cx, cy, winw, winh,
                               NULL,NULL,wc.hInstance,&st);
      }
      if(hwnd){
          /* Restore the saved dialog position when there is one, else center
           * the real window (GetWindowRect, not the requested size: DPI
           * virtualization can make them differ) on the monitor with the
           * cursor.  A spot on a disconnected monitor falls back to
           * centered, and the rect is clamped into the work area. */
          RECT rc;
          int w, h, x, y;
          if(GetWindowRect(hwnd, &rc)){
              w = rc.right - rc.left; h = rc.bottom - rc.top;
              if(last_read_launchpos(&x, &y)){
                  HMONITOR hm = MonitorFromPoint(
                      (POINT){x + w/2, y + (h/2 < 16 ? h/2 : 16)},
                      MONITOR_DEFAULTTONULL);
                  if(!hm){
                      center_on_cursor_monitor(w, h, &x, &y);
                  } else {
                      MONITORINFO mi;
                      mi.cbSize = sizeof(mi);
                      if(GetMonitorInfoA(hm, &mi)){
                          if(x + w <= mi.rcWork.left || x >= mi.rcWork.right ||
                             y + h <= mi.rcWork.top || y >= mi.rcWork.bottom){
                              x = mi.rcWork.left +
                                  ((mi.rcWork.right-mi.rcWork.left)-w)/2;
                              y = mi.rcWork.top +
                                  ((mi.rcWork.bottom-mi.rcWork.top)-h)/2;
                          }
                          if(x + w > mi.rcWork.right) x = mi.rcWork.right - w;
                          if(y + h > mi.rcWork.bottom) y = mi.rcWork.bottom - h;
                          if(x < mi.rcWork.left) x = mi.rcWork.left;
                          if(y < mi.rcWork.top) y = mi.rcWork.top;
                      } else {
                          center_on_cursor_monitor(w, h, &x, &y);
                      }
                  }
              } else {
                  center_on_cursor_monitor(w, h, &x, &y);
              }
              SetWindowPos(hwnd, NULL, x, y, 0, 0, SWP_NOSIZE|SWP_NOZORDER);
          }
      }
      if(setthread) setthread(prev ? prev : (void*)(INT_PTR)-4);
    }
    if(!hwnd) return 0;
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
        /* Remember for next run (same dir, else same release): the next
         * showing restores the selection above, and the per-install
         * pfemu.cfg it loads brings the settings back with it. */
        last_save(r);
    }
    out->fullscreen = st.fullscreen;
    out->start_table = st.start_table;
    out->mode = st.mode;
    snprintf(out->replay_path, sizeof(out->replay_path), "%s", st.replay_path);
    return 1;
}
