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
#include <stdio.h>
#include "pfemu.h"

#define GAME_DIR  "FANTASY"
#define GAME_PROG "PINBALL.EXE"

#define ID_SOUND        104
#define ID_NOTE         105
#define ID_LAUNCH       106
#define ID_QUIT         107
#define ID_CHEAT_BALLS  108
#define ID_CHEAT_SPRING 109
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
 * 0x14 = quality index (0).  The gap bytes are never read by the driver. */
static void cfg_sblaster(uint8_t out[25]){
    int i;
    for(i=0;i<25;i++) out[i]=0;
    memcpy(out, "SBLASTER.SDR", 12);
    out[0x0E]=1; out[0x11]=3; out[0x14]=0;
}

void write_sound_cfg(const char *dir, int on){
    char path[600], sub[600];
    FILE *f;
    snprintf(sub, sizeof(sub), "%s/PFEMU-STATE", dir);
    CreateDirectoryA(sub, NULL);
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/SOUND.CFG", dir);
    f = fopen(path, "wb");
    if(!f) return;
    if(on){ uint8_t cfg[25]; cfg_sblaster(cfg); fwrite(cfg, 1, 25, f); }
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
 * PINBALL.CFG's own layout - just a checkbox-per-cheat starting state that
 * src/fantasies.c applies the moment a table loads. */
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

/* ------------------------------------------------------------------ UI */
typedef struct {
    int sound;              /* checkbox state */
    uint8_t cfg[6];         /* PINBALL.CFG option bytes */
    int cheat_balls;         /* checkbox state: infinite balls */
    int cheat_spring;         /* checkbox state: ball control mode */
    int done;               /* dialog finished */
    int ok;                 /* 1 = launch, 0 = quit */
    HWND hSound, hOpt[6], hCheatBalls, hCheatSpring;
    HFONT hFont;
} LaunchState;

static LRESULT CALLBACK launch_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    LaunchState *st = (LaunchState*)(INT_PTR)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch(m){
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)l;
        HWND c;
        int i, y;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        st = (LaunchState*)cs->lpCreateParams;
        st->hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        c = CreateWindowExA(0,"STATIC","Pinball Fantasies",WS_CHILD|WS_VISIBLE,
                            12,12,336,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        st->hSound = CreateWindowExA(0,"BUTTON","Sound on (SoundBlaster)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,36,324,20,h,(HMENU)ID_SOUND,cs->hInstance,0);
        SendMessageA(st->hSound,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        c = CreateWindowExA(0,"STATIC",
                            "On: SoundBlaster 220h/IRQ 7. Off: silent.\r\n"
                            "Writes SOUND.CFG, game files stay pristine.",
                            WS_CHILD|WS_VISIBLE,
                            24,60,324,28,h,(HMENU)ID_NOTE,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y = 96;
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
        c = CreateWindowExA(0,"STATIC","Trainer cheats (RAZOR DoX, 1994):",
                            WS_CHILD|WS_VISIBLE,24,y,324,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        y += 20;
        st->hCheatBalls = CreateWindowExA(0,"BUTTON","Infinite balls",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,y,324,20,h,(HMENU)ID_CHEAT_BALLS,cs->hInstance,0);
        SendMessageA(st->hCheatBalls,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_CHEAT_BALLS,st->cheat_balls?BST_CHECKED:BST_UNCHECKED);
        y += 24;
        st->hCheatSpring = CreateWindowExA(0,"BUTTON","Ball control mode",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,y,324,20,h,(HMENU)ID_CHEAT_SPRING,cs->hInstance,0);
        SendMessageA(st->hCheatSpring,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_CHEAT_SPRING,st->cheat_spring?BST_CHECKED:BST_UNCHECKED);
        y += 30;
        c = CreateWindowExA(0,"BUTTON","Launch",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                            184,y+8,76,24,h,(HMENU)ID_LAUNCH,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Quit",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            272,y+8,76,24,h,(HMENU)ID_QUIT,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if(id==ID_SOUND){
            st->sound = IsDlgButtonChecked(h,ID_SOUND)==BST_CHECKED;
        } else if(id==ID_CHEAT_BALLS){
            st->cheat_balls = IsDlgButtonChecked(h,ID_CHEAT_BALLS)==BST_CHECKED;
        } else if(id==ID_CHEAT_SPRING){
            st->cheat_spring = IsDlgButtonChecked(h,ID_CHEAT_SPRING)==BST_CHECKED;
        } else if(id==ID_LAUNCH){
            int i;
            DWORD at = GetFileAttributesA(GAME_DIR);
            if(at==INVALID_FILE_ATTRIBUTES || !(at&FILE_ATTRIBUTE_DIRECTORY)){
                MessageBoxA(h,"Game directory not found.\n"
                              "Run from the pfemu folder.",
                            "pfemu",MB_OK|MB_ICONERROR);
                return 0;
            }
            write_sound_cfg(GAME_DIR, st->sound);
            for(i=0;i<6;i++){
                LRESULT sel = SendMessageA(st->hOpt[i],CB_GETCURSEL,0,0);
                st->cfg[i] = (uint8_t)(sel==CB_ERR ? cfg_pinball_defaults[i] : sel);
            }
            write_pinball_cfg(GAME_DIR, st->cfg);
            write_cheats_cfg(GAME_DIR, st->cheat_balls, st->cheat_spring);
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
    LaunchState st;
    int sw, sh;
    const int winw = 372, winh = 400;
    memset(&wc,0,sizeof(wc));
    memset(&st,0,sizeof(st));
    wc.lpfnWndProc = launch_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "pfemu-launcher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassA(&wc);
    st.sound = read_sound_is_sb(GAME_DIR);
    read_pinball_cfg(GAME_DIR, st.cfg);
    read_cheats_cfg(GAME_DIR, &st.cheat_balls, &st.cheat_spring);
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
    out->dir = GAME_DIR;
    out->prog = GAME_PROG;
    return 1;
}
