/* Win32 launcher: game picker + sound toggle, shown once at startup.
 *
 * The emulator used to boot straight into Pinball Fantasies.  This dialog
 * lets the user pick the game (Fantasies/Dreams/Illusions) and, for the
 * games with Frontline-style sound drivers, turn SoundBlaster sound on or
 * off without running the SETSOUND utility: the checkbox writes SOUND.CFG
 * directly into the game's PFEMU-STATE/ overlay directory, so the installed
 * files are never touched and the game picks it up on its next boot.
 *
 * main() skips the dialog for explicit/automated runs (see -nolauncher,
 * -p, -setup, -secs handling there).
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include "pfemu.h"

#define ID_GAME_FAN  101
#define ID_GAME_DRM  102
#define ID_GAME_ILL  103
#define ID_SOUND     104
#define ID_NOTE      105
#define ID_LAUNCH    106
#define ID_QUIT      107

typedef struct {
    const char *dir;      /* game directory (FANTASY/DREAMS/ILLUSION) */
    const char *prog;     /* program to boot */
    int has_sound_toggle; /* checkbox applies to this game */
} GameDef;

static const GameDef games[] = {
    { "FANTASY",  "PINBALL.EXE", 1 },
    /* Dreams boots PD.EXE directly: DREAMS.COM is only a BAT2EXEC memory
     * check (CHKMEM, 530k gate), meaningless under emulation. */
    { "DREAMS",   "PD.EXE",       0 },
    /* Illusions: listed for planning; boot support is not there yet. */
    { "ILLUSION", "illusion.exe", 1 },
};

typedef struct {
    int sel;        /* index into games[] */
    int sound;      /* checkbox state */
    int done;       /* dialog finished */
    int ok;         /* 1 = launch, 0 = quit */
    HWND hSound, hNote;
    HFONT hFont;
} LaunchState;

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

/* ------------------------------------------------------------------ UI */
static void note_for(HWND hNote, int sel){
    if(sel==1)
        SetWindowTextA(hNote, "Dreams: sound is chosen in-game (F1/F2 menu).");
    else
        SetWindowTextA(hNote, "On: SoundBlaster 220h/IRQ 7. Off: silent.\r\n"
                              "Writes SOUND.CFG, game files stay pristine.");
}

static LRESULT CALLBACK launch_proc(HWND h, UINT m, WPARAM w, LPARAM l){
    LaunchState *st = (LaunchState*)(INT_PTR)GetWindowLongPtrA(h, GWLP_USERDATA);
    switch(m){
    case WM_CREATE: {
        CREATESTRUCTA *cs = (CREATESTRUCTA*)l;
        HWND c;
        SetWindowLongPtrA(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        st = (LaunchState*)cs->lpCreateParams;
        st->hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
        c = CreateWindowExA(0,"STATIC","Game:",WS_CHILD|WS_VISIBLE,
                            12,12,336,16,h,0,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Pinball Fantasies",
                            WS_CHILD|WS_VISIBLE|WS_GROUP|WS_TABSTOP|BS_AUTORADIOBUTTON,
                            24,32,324,20,h,(HMENU)ID_GAME_FAN,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Pinball Dreams",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON,
                            24,54,324,20,h,(HMENU)ID_GAME_DRM,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Pinball Illusions (not yet supported)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTORADIOBUTTON|WS_DISABLED,
                            24,76,324,20,h,(HMENU)ID_GAME_ILL,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckRadioButton(h,ID_GAME_FAN,ID_GAME_ILL,ID_GAME_FAN);
        st->hSound = CreateWindowExA(0,"BUTTON","Sound on (SoundBlaster)",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
                            24,104,324,20,h,(HMENU)ID_SOUND,cs->hInstance,0);
        SendMessageA(st->hSound,WM_SETFONT,(WPARAM)st->hFont,0);
        CheckDlgButton(h,ID_SOUND,st->sound?BST_CHECKED:BST_UNCHECKED);
        st->hNote = CreateWindowExA(0,"STATIC","",
                            WS_CHILD|WS_VISIBLE,
                            24,128,324,32,h,(HMENU)ID_NOTE,cs->hInstance,0);
        SendMessageA(st->hNote,WM_SETFONT,(WPARAM)st->hFont,0);
        note_for(st->hNote,0);
        c = CreateWindowExA(0,"BUTTON","Launch",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_DEFPUSHBUTTON,
                            184,176,76,24,h,(HMENU)ID_LAUNCH,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        c = CreateWindowExA(0,"BUTTON","Quit",
                            WS_CHILD|WS_VISIBLE|WS_TABSTOP,
                            272,176,76,24,h,(HMENU)ID_QUIT,cs->hInstance,0);
        SendMessageA(c,WM_SETFONT,(WPARAM)st->hFont,0);
        return 0; }
    case WM_COMMAND: {
        int id = LOWORD(w);
        if(id==ID_GAME_FAN || id==ID_GAME_DRM){
            st->sel = (id==ID_GAME_DRM)?1:0;
            CheckRadioButton(h,ID_GAME_FAN,ID_GAME_ILL,id);
            EnableWindow(st->hSound, games[st->sel].has_sound_toggle);
            note_for(st->hNote, st->sel);
        } else if(id==ID_SOUND){
            st->sound = IsDlgButtonChecked(h,ID_SOUND)==BST_CHECKED;
        } else if(id==ID_LAUNCH){
            DWORD at = GetFileAttributesA(games[st->sel].dir);
            if(at==INVALID_FILE_ATTRIBUTES || !(at&FILE_ATTRIBUTE_DIRECTORY)){
                MessageBoxA(h,"Game directory not found.\n"
                              "Run from the pfemu folder.",
                            "pfemu",MB_OK|MB_ICONERROR);
                return 0;
            }
            if(games[st->sel].has_sound_toggle)
                write_sound_cfg(games[st->sel].dir, st->sound);
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
    memset(&wc,0,sizeof(wc));
    memset(&st,0,sizeof(st));
    wc.lpfnWndProc = launch_proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.lpszClassName = "pfemu-launcher";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE+1);
    RegisterClassA(&wc);
    st.sel = 0;
    st.sound = read_sound_is_sb(games[0].dir);
    hwnd = CreateWindowExA(0,"pfemu-launcher","pfemu launcher",
                           WS_OVERLAPPED|WS_CAPTION|WS_SYSMENU,
                           CW_USEDEFAULT,CW_USEDEFAULT,372,244,
                           NULL,NULL,wc.hInstance,&st);
    if(!hwnd) return 0;
    sw = GetSystemMetrics(SM_CXSCREEN); sh = GetSystemMetrics(SM_CYSCREEN);
    SetWindowPos(hwnd,NULL,(sw-372)/2,(sh-244)/2,0,0,SWP_NOSIZE|SWP_NOZORDER);
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
    out->dir = games[st.sel].dir;
    out->prog = games[st.sel].prog;
    return 1;
}
