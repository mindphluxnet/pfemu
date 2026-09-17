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
 * This build supports Pinball Fantasies only; sibling-game (Dreams/
 * Illusions) launcher support has been removed.
 *
 * main() skips the dialog for explicit/automated runs (see -nolauncher,
 * -p, -setup, -secs handling there).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <stdio.h>
#include "pfemu.h"

/* Which release is in front of us is not something this dialog decides any
 * more, and not something it reads off a directory name.  src/release.c
 * hashes each candidate directory's five program files and identifies the
 * build from that; the dialog just shows the answer and, when more than one
 * installation is sitting side by side, lets the user pick which directory
 * to boot.  See docs/VERSIONS.md for why the old two-hardcoded-directories,
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
 * table handoff (WRITEUP-PHASE2.md Sec 2.2, 5.13.1) - but this launcher
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

/* Trainer cheats, ported from trainer/PINTRN.COM (see src/fantasies.c for
 * the reverse-engineering writeup and the actual patch logic).  Separate
 * 2-byte file from pfemu_options.cfg above since these aren't part of
 * PINBALL.CFG's own layout - just a starting state that src/fantasies.c
 * applies the moment a table loads.  One checkbox in the UI ("Enable
 * trainer") drives both bytes together - infinite balls and ball control
 * mode are still two independent patches underneath (and the '1'/'2'
 * hotkeys still toggle them independently in-game), but there's no real
 * reason to make the user tick two boxes to turn "the trainer" on, so both
 * get written identically here.  Reading them back separately (rather than
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
    "1 - 12000 Hz (lowest)",
    "2 - 16000 Hz",
    "3 - 20000 Hz",
    "4 - 21000 Hz",
    "5 - 21000 Hz (highest)"
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
    HFONT hFont;
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

/* The one read-only line that replaced the version radio buttons: whatever
 * the detector concluded about the selected directory.  Launch is enabled
 * only for a release that was actually recognised - an unknown or mixed
 * installation must never get a known release's memory layout poked into it,
 * and the Details button is there to say exactly what was wrong. */
static void show_detection(HWND h, LaunchState *st){
    const RelResult *r = cur_inst(st);
    char line[300];
    HWND btn = GetDlgItem(h, ID_LAUNCH);
    if(!r) snprintf(line, sizeof(line), "No game found. Put one release's files in GAME\\.");
    else if(release_runnable(r)) snprintf(line, sizeof(line), "Detected: %s", r->summary);
    else snprintf(line, sizeof(line), "%s: %s", release_state_name(r->state), r->summary);
    if(st->hDetected) SetWindowTextA(st->hDetected, line);
    if(btn) EnableWindow(btn, r && release_runnable(r));
    if(st->hDetails) EnableWindow(st->hDetails, r != NULL);
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
    show_detection(h, st);
}

static LRESULT CALLBACK launch_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    LaunchState *st = (LaunchState*)(INT_PTR)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch(m){
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)l;
        HWND c;
        int i, y, base;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        st = (LaunchState*)cs->lpCreateParams;
        st->hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        c = CreateWindowExA(0,"STATIC","Pinball Fantasies",WS_CHILD|WS_VISIBLE,
                            12,12,336,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        base = 0;
        if(st->ninst > 1){
            /* Only shown when several installations sit side by side, and it
             * picks a *directory*, not a version - what is in each one is
             * whatever its hashes said it is. */
            int k;
            c = CreateWindowExA(0,"STATIC","Installation:",WS_CHILD|WS_VISIBLE,
                                24,35,80,16,h,0,cs->hInstance,0);
            SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
            st->hInstall = CreateWindowExA(0,"COMBOBOX","",
                                WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                                108,32,240,240,h,(HMENU)ID_INSTALL,cs->hInstance,0);
            SendMessageA(st->hInstall,WM_SETFONT,(WPARAM)st->hFont,0);
            for(k=0;k<st->ninst;k++){
                char item[200];
                snprintf(item,sizeof(item),"%s  -  %s",
                         st->inst[k].dir, st->inst[k].summary);
                SendMessageA(st->hInstall,CB_ADDSTRING,0,(LPARAM)item);
            }
            SendMessageA(st->hInstall,CB_SETCURSEL,st->sel,0);
            base = 28;
        }
        /* The detection result, read-only.  There is nothing here for the
         * user to choose: the files decide.  Details prints the whole report,
         * which is the only place a failure explains itself. */
        st->hDetected = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_ENDELLIPSIS,
                            24,34+base,258,16,h,0,cs->hInstance,0);
        SendMessageA(st->hDetected,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hDetails = CreateWindowExA(0,"BUTTON","Details",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            288,31+base,60,22,h,(HMENU)ID_DETAILS,cs->hInstance,0);
        SendMessageA(st->hDetails,WM_SETFONT,(WPARAM)st->hFont,0);
        base += 22;
        st->hSound = CreateWindowExA(0,"BUTTON","Sound on (SoundBlaster)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,36+base,324,20,h,(HMENU)ID_SOUND,cs->hInstance,0);
        SendMessageA(st->hSound,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        c = CreateWindowExA(0,"STATIC",
                            "On: SoundBlaster 220h/IRQ 7. Off: silent.\r\n"
                            "Writes SOUND.CFG, game files stay pristine.",
                            WS_CHILD|WS_VISIBLE,
                            24,60+base,324,28,h,(HMENU)ID_NOTE,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y = 92+base;
        /* Quality: SETSOUND's five-notch setting, written straight to
         * SOUND.CFG byte 0x14 for the driver to read. */
        c = CreateWindowExA(0,"STATIC","Quality:",WS_CHILD|WS_VISIBLE,
                            24,y+3,100,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hQuality = CreateWindowExA(0,"COMBOBOX","",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|CBS_DROPDOWNLIST,
                            128,y,180,200,h,(HMENU)ID_QUALITY,cs->hInstance,0);
        SendMessageA(st->hQuality,WM_SETFONT,(WPARAM)st->hFont,0);
        for(i=0;i<5;i++)
            SendMessageA(st->hQuality,CB_ADDSTRING,0,(LPARAM)quality_labels[i]);
        SendMessageA(st->hQuality,CB_SETCURSEL,st->quality,0);
        y += 24;
        c = CreateWindowExA(0,"STATIC",
                            "Driver mixing rate. Higher costs emulated 386 time.",
                            WS_CHILD|WS_VISIBLE,24,y,324,14,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y += 20;
        /* Volume: host-side only, applied where waveOut is fed. */
        c = CreateWindowExA(0,"STATIC","Volume:",WS_CHILD|WS_VISIBLE,
                            24,y+6,100,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hVolume = CreateWindowExA(0,TRACKBAR_CLASSA,"",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_HORZ|TBS_AUTOTICKS,
                            124,y,180,28,h,(HMENU)ID_VOLUME,cs->hInstance,0);
        SendMessageA(st->hVolume,TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
        SendMessageA(st->hVolume,TBM_SETTICFREQ,25,0);
        SendMessageA(st->hVolume,TBM_SETPAGESIZE,0,10);
        SendMessageA(st->hVolume,TBM_SETPOS,TRUE,st->volume);
        st->hVolLabel = CreateWindowExA(0,"STATIC","",WS_CHILD|WS_VISIBLE|SS_RIGHT,
                            306,y+6,42,16,h,(HMENU)ID_VOLLABEL,cs->hInstance,0);
        SendMessageA(st->hVolLabel,WM_SETFONT,(WPARAM)st->hFont,0);
        set_vol_label(st);
        y += 36;
        for(i=0;i<6;i++){
            int k;
            c = CreateWindowExA(0,"STATIC",opts[i].label,WS_CHILD|WS_VISIBLE,
                                24,y+3,100,16,h,0,cs->hInstance,0);
            SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
            st->hOpt[i] = CreateWindowExA(0,"COMBOBOX","",
                                WS_CHILD|WS_VISIBLE|WS_TABSTOP|WS_VSCROLL|
                                CBS_DROPDOWNLIST,
                                128,y,120,200,h,(HMENU)(INT_PTR)(ID_OPT_FIRST+i),cs->hInstance,0);
            SendMessageA(st->hOpt[i],WM_SETFONT,(WPARAM)st->hFont,0);
            for(k=0;k<opts[i].n;k++)
                SendMessageA(st->hOpt[i],CB_ADDSTRING,0,(LPARAM)opts[i].values[k]);
            SendMessageA(st->hOpt[i],CB_SETCURSEL,st->cfg[i],0);
            y += 26;
        }
        y += 6;
        st->hCheatEnable = CreateWindowExA(0,"BUTTON","Enable trainer (RAZOR DoX, 1994)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,y,324,20,h,(HMENU)ID_CHEAT_ENABLE,cs->hInstance,0);
        SendMessageA(st->hCheatEnable,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_CHEAT_ENABLE,st->cheat_enable?BST_CHECKED:BST_UNCHECKED);
        y += 20;
        c = CreateWindowExA(0,"STATIC",
                            "Infinite balls, and full ball control from the\r\n"
                            "down-arrow key ('1'/'2' toggle each in-game too).",
                            WS_CHILD|WS_VISIBLE,24,y,324,28,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y += 34;
        st->hFullscreen = CreateWindowExA(0,"BUTTON","Start in fullscreen",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,y,324,20,h,(HMENU)ID_FULLSCREEN,cs->hInstance,0);
        SendMessageA(st->hFullscreen,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_FULLSCREEN,st->fullscreen?BST_CHECKED:BST_UNCHECKED);
        y += 30;
        c = CreateWindowExA(0,"BUTTON","Launch",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                            184,y+8,76,24,h,(HMENU)ID_LAUNCH,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Quit",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            272,y+8,76,24,h,(HMENU)ID_QUIT,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        show_detection(h, st);   /* needs the Launch button to exist */
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
            /* The full report, verbatim and selectable (Ctrl+C copies a
             * message box whole).  For a release we do not know this is also
             * the intake format: five sizes and hashes the user can send on
             * without having run anything. */
            const RelResult *r = cur_inst(st);
            if(r) MessageBoxA(h, r->detail, "pfemu - detection details", MB_OK);
        } else if(id==ID_LAUNCH){
            int i;
            const RelResult *r = cur_inst(st);
            const char *dir;
            if(!release_runnable(r)) return 0;
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
            write_cheats_cfg(dir, st->cheat_enable, st->cheat_enable);
            write_fullscreen_cfg(dir, st->fullscreen);
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

/* Modal launcher.  Returns 1 with *out filled when the user picks Launch,
 * 0 when they quit (caller should exit without booting). */
int show_launcher(LaunchChoice *out){
    WNDCLASSA wc;
    HWND hwnd;
    MSG msg;
    /* static: it now carries a RelResult per installation, which is a lot of
     * report text to put on the stack for a dialog that runs once. */
    static LaunchState st;
    int sw, sh;
    int winw = 372, winh = 528;
    INITCOMMONCONTROLSEX icc;
    memset(&wc,0,sizeof(wc));
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
    RegisterClassA(&wc);
    /* Every directory that holds an INTRO.PRG, identified by content.  GAME
     * comes first when it exists, then the rest alphabetically, so the
     * default selection is stable rather than filesystem-order. */
    st.ninst = release_scan(st.inst, MAX_INSTALLS);
    st.sel = 0;
    /* Prefer landing on one that can actually be launched. */
    { int i;
      for(i=0;i<st.ninst;i++)
          if(release_runnable(&st.inst[i])){ st.sel = i; break; } }
    if(st.ninst > 1) winh += 28;
    { const char *dir = cur_game_dir(&st);
      int balls, spring;
      st.sound = read_sound_is_sb(dir);
      st.quality = read_sound_quality(dir);
      st.volume = read_volume_cfg(dir);
      read_pinball_cfg(dir, st.cfg);
      read_cheats_cfg(dir, &balls, &spring);
      st.cheat_enable = balls || spring;
      st.fullscreen = read_fullscreen_cfg(dir); }
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
    if(!st.ok) return 0;
    {   /* The boot program comes from the detected release, not from a
         * constant: Power Pack's is PF.EXE, the other two ship PINBALL.EXE.
         * ID_LAUNCH already refused anything not recognised. */
        const RelResult *r = cur_inst(&st);
        snprintf(out->dir, sizeof(out->dir), "%s", r->dir);
        snprintf(out->prog, sizeof(out->prog), "%s",
                 r->boot[0] ? r->boot : r->rel->boot);
    }
    out->fullscreen = st.fullscreen;
    return 1;
}
