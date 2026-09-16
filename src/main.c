/* Win32 host: window, framebuffer presentation, keyboard, main emulation loop */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pfemu.h"

extern void  emu_advance(void);
extern double emu_time;
extern double emu_ips;
extern double emu_inv_ips;
extern void  dev_tick(void);
extern void  set_sreg(int s, uint16_t v);
extern int   vga_get_mode(void);
extern int   vga_force256, vga_nodbl;

uint8_t vga_font8x16[256*16];
uint8_t vga_font8x8[256*8];

int trace_level = 0;
static FILE *trace_fp = NULL;

void trc(const char *fmt, ...){
    va_list ap;
    if(!trace_level) return;
    va_start(ap, fmt);
    vfprintf(trace_fp ? trace_fp : stderr, fmt, ap);
    va_end(ap);
    if(trace_fp) fflush(trace_fp);
}

/* -------------------------------------------------------------- window  */
static HWND hwnd;
static HDC  hdc;
static BITMAPINFO bmi;
static uint32_t fb[1024*768];
static int fbw = 320, fbh = 200;
static int running = 1;
static int win_w = 960, win_h = 600;
static int integer_scale = 0;
static int fullscreen = 0;
static int screenshot_pending = 0;
/* Mute state for the keypad-* toggle, kept out here because the exit path
 * needs it: see the volume keys in wndproc() and the save in main(). */
static int vol_premute = -1, vol_muted = 0;
static LONG windowed_style;
static RECT windowed_rect;

/* ----------------------------------------------------------------- OSD --
 * Host-only on-screen notification, drawn straight into the presented
 * framebuffer after vga_render(), so it is independent of whatever video
 * page or mode the game is using and there is nothing the guest can see or
 * overwrite.  It started out as the trainer's confirmation message and lived
 * in src/fantasies.c; the volume keys want the same thing and are not game
 * behaviour, so it sits here with the rest of the host presentation and
 * fantasies.c calls in like any other caller. */
static char osd_text[48] = "";
static double osd_until = 0;

void osd_show(const char *text){
    snprintf(osd_text, sizeof(osd_text), "%s", text);
    osd_until = emu_time + 1.6;
}

void osd_clear(void){
    osd_text[0] = 0;
    osd_until = 0;
}

void osd_draw(uint32_t *fb_, int w, int h){
    int scale, len, tw, th, x0, y0, i, x, y;
    if(!osd_text[0] || emu_time >= osd_until) return;
    if(!fb_ || w <= 0 || h <= 0) return;
    len = (int)strlen(osd_text);
    scale = (w >= 160 && (len+1)*8*2 + 8 <= w) ? 2 : 1;
    tw = len*8*scale + 8*scale;
    if(tw > w) tw = w;
    th = 8*scale + 6*scale;
    x0 = (w - tw) / 2;
    if(x0 < 0) x0 = 0;
    y0 = h - th - 6*scale;
    if(y0 < 0) y0 = 0;
    for(y=0; y<th; y++){
        for(x=0; x<tw; x++){
            int px = x0+x, py = y0+y;
            if(px>=0 && px<w && py>=0 && py<h) fb_[py*w+px] = 0x00181818u;
        }
    }
    for(i=0; i<len; i++){
        uint8_t ch = (uint8_t)osd_text[i];
        int cx = x0 + 4*scale + i*8*scale;
        int cy = y0 + 3*scale;
        int r, c2, sx, sy;
        for(r=0; r<8; r++){
            uint8_t bits = vga_font8x8[ch*8+r];
            for(c2=0; c2<8; c2++){
                if(!((bits >> (7-c2)) & 1)) continue;
                for(sy=0; sy<scale; sy++){
                    for(sx=0; sx<scale; sx++){
                        int px = cx + c2*scale + sx, py = cy + r*scale + sy;
                        if(px>=0 && px<w && py>=0 && py<h) fb_[py*w+px] = 0x00FFE040u;
                    }
                }
            }
        }
    }
}

static void set_fullscreen(int on){
    if(on == fullscreen) return;
    if(on){
        MONITORINFO mi;
        mi.cbSize = sizeof(mi);
        windowed_style = GetWindowLongA(hwnd, GWL_STYLE);
        GetWindowRect(hwnd, &windowed_rect);
        if(GetMonitorInfoA(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi)){
            SetWindowLongA(hwnd, GWL_STYLE, WS_POPUP|WS_VISIBLE);
            SetWindowPos(hwnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                         mi.rcMonitor.right-mi.rcMonitor.left,
                         mi.rcMonitor.bottom-mi.rcMonitor.top,
                         SWP_FRAMECHANGED);
            fullscreen = 1;
        }
    } else {
        SetWindowLongA(hwnd, GWL_STYLE, windowed_style);
        SetWindowPos(hwnd, NULL, windowed_rect.left, windowed_rect.top,
                     windowed_rect.right-windowed_rect.left,
                     windowed_rect.bottom-windowed_rect.top,
                     SWP_NOZORDER|SWP_FRAMECHANGED);
        fullscreen = 0;
    }
}

void plat_set_fullscreen(int on){ set_fullscreen(on); }

extern void kbd_release_all(void);
static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_DESTROY: case WM_CLOSE: running = 0; PostQuitMessage(0); return 0;
    case WM_SIZE: win_w = LOWORD(l); win_h = HIWORD(l); return 0;
    case WM_KILLFOCUS: kbd_release_all(); break;
    case WM_ACTIVATE: if(LOWORD(w) == WA_INACTIVE) kbd_release_all(); break;
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        /* Scroll Lock quits: F12 belongs to the game (its INT 9 maps scan
         * code 58h), and so do both shifts, both alts, both controls, the
         * arrows and space. */
        if(w == VK_SCROLL){ running = 0; return 0; }
        /* Alt+Enter toggles fullscreen; bit 30 filters key-repeat so holding
         * it doesn't flap the window every auto-repeat interval. */
        if(m == WM_SYSKEYDOWN && w == VK_RETURN && !(l & (1<<30))){
            set_fullscreen(!fullscreen);
            return 0;
        }
        /* F11 saves a screenshot; bit 30 filters key-repeat like Alt+Enter
         * above.  Not Print Screen: Windows 11 intercepts that itself and
         * launches Snipping Tool before this window ever sees it.  F11 isn't
         * one of the keys the game reads (see the scan-code list above). */
        if(m == WM_KEYDOWN && w == VK_F11 && !(l & (1<<30))){
            screenshot_pending = 1;
            return 0;
        }
        /* Volume: - and + (main row or keypad) in 5% steps, keypad * mutes
         * and restores.  The launcher's slider only sets the starting level,
         * so this is how the right one gets found without quitting first.
         * Key-repeat is deliberately left on: holding - to fade out is the
         * point.  Nothing emulated moves - audio_volume is host sink gain.
         *
         * The table's own INT 9 handler reads only the flippers, the plunger,
         * space and F11/F12 (WRITEUP-PHASE2.md 5.11), so none of these
         * collide there; unhandled keys do still reach the game through the
         * BIOS buffer, so high-score name entry loses - and + (it keeps
         * every letter, and Backspace, which is why Backspace is not the
         * mute key). */
        if(m == WM_KEYDOWN && (w == VK_OEM_MINUS || w == VK_SUBTRACT ||
                               w == VK_OEM_PLUS  || w == VK_ADD ||
                               w == VK_MULTIPLY)){
            if(w == VK_MULTIPLY){
                if(l & (1<<30)) return 0;          /* mute doesn't auto-repeat */
                if(audio_volume > 0){
                    vol_premute = audio_volume; vol_muted = 1; audio_volume = 0;
                } else {
                    audio_volume = vol_premute > 0 ? vol_premute : AUDIO_VOLUME_DEFAULT;
                    vol_muted = 0;
                }
            } else {
                int up = (w == VK_OEM_PLUS || w == VK_ADD);
                audio_volume += up ? 5 : -5;
                if(audio_volume < 0) audio_volume = 0;
                if(audio_volume > 100) audio_volume = 100;
                vol_muted = 0;
                audio_volume_dirty = 1;            /* main() writes it on exit */
            }
            { char msg[32];
              /* Muted reads as MUTED rather than 0%: a level of zero reached
               * by riding - down is a different thing from the toggle, and
               * only one of the two comes back with the same key. */
              if(vol_muted) snprintf(msg, sizeof(msg), "VOLUME: MUTED");
              else snprintf(msg, sizeof(msg), "VOLUME: %d%%", audio_volume);
              osd_show(msg); }
            fprintf(stderr, "[snd] volume %d%%\n", audio_volume);
            return 0;
        }
        if(sc) kbd_key(sc | (ext?0xE000:0), 1);
        return 0; }
    case WM_SYSKEYUP: case WM_KEYUP: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        if(sc) kbd_key(sc | (ext?0xE000:0), 0);
        return 0; }
    }
    return DefWindowProc(h,m,w,l);
}

/* Confirmed via -t trace: Windows can simply never deliver a WM_KEYUP for a
 * held modifier key - no WM_KILLFOCUS, no other message loss, the release
 * just never arrives at wndproc. Fantasies' flippers are bound to the Shift
 * keys, so a swallowed release there strands the shared flipper bit up
 * exactly like a lost break byte would, except dev.c's queue/PIC logic never
 * even saw an event to lose - the OS never sent one. kbd_release_all()
 * already recovers this on focus loss; this is the same idea run every
 * frame instead of only then, by checking the handful of scancodes the
 * flipper fix cares about against live hardware state and forcing a break
 * the moment they disagree, instead of waiting for the window to lose
 * focus (which may never happen before the next press). */
static void kbd_reconcile_physical(void){
    static const struct { int sc, ext; int vk; } keys[] = {
        {0x2A,0,VK_LSHIFT},   {0x36,0,VK_RSHIFT},
        {0x1D,0,VK_LCONTROL}, {0x1D,1,VK_RCONTROL},
        {0x38,0,VK_LMENU},    {0x38,1,VK_RMENU},
    };
    size_t i;
    for(i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
        unsigned idx = (unsigned)keys[i].sc | (keys[i].ext ? 0x80u : 0u);
        if(kbd_held_get(idx) && !(GetAsyncKeyState(keys[i].vk) & 0x8000)){
            trc("[kbd] reconcile: %s%02X marked held but not physically down - forcing break\n",
                keys[i].ext?"E0 ":"", keys[i].sc);
            kbd_key(keys[i].sc | (keys[i].ext?0xE000:0), 0);
        }
    }
}

/* Build 8x16 and 8x8 character bitmaps from a host fixed-pitch font so text
 * mode (DOS messages, the sound-setup screen) is readable. */
static void build_fonts(void){
    HDC mdc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontA(16,8,0,0,FW_NORMAL,0,0,0,OEM_CHARSET,
                          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                          NONANTIALIASED_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
    BITMAPINFO bi; void *bits = NULL; HBITMAP bm;
    int c,y,x;
    memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(mdc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    SelectObject(mdc,bm);
    SelectObject(mdc,f);
    SetBkColor(mdc, RGB(0,0,0));
    SetTextColor(mdc, RGB(255,255,255));
    for(c=0;c<256;c++){
        RECT r = {0,0,8,16};
        char ch = (char)c;
        uint32_t *px = (uint32_t*)bits;
        FillRect(mdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(c >= 32) TextOutA(mdc,0,0,&ch,1);
        GdiFlush();
        for(y=0;y<16;y++){
            uint8_t row = 0;
            for(x=0;x<8;x++) if((px[y*8+x] & 0xFF) > 96) row |= (uint8_t)(0x80>>x);
            vga_font8x16[c*16+y] = row;
        }
        for(y=0;y<8;y++) vga_font8x8[c*8+y] = vga_font8x16[c*16+y*2];
    }
    DeleteObject(bm); DeleteObject(f); DeleteDC(mdc);
}

/* Windows' StickyKeys/FilterKeys/ToggleKeys accessibility shortcuts are the
 * real cause of the "flipper gets stuck up" reports: Pinball Fantasies'
 * default flipper keys are the Shift keys, and holding one down for the ~8s
 * FilterKeys threshold - an ordinary "trap the ball on the flipper" move, not
 * frantic play - makes the OS itself swallow the eventual key-up (and/or pop
 * a system dialog) before our window ever sees WM_KEYUP. No byte is lost in
 * dev.c's keyboard queue; the break never arrives from the OS at all, so
 * kbd_held[] (and the guest's shared flipper bit) never clears. Standard
 * fix, per Microsoft's own guidance for games that bind gameplay to Shift:
 * disable the shortcut-activation of these features while the window has
 * focus, restoring whatever the user had on exit. */
static STICKYKEYS saved_sticky = { sizeof(STICKYKEYS), 0 };
static TOGGLEKEYS saved_toggle = { sizeof(TOGGLEKEYS), 0 };
static FILTERKEYS saved_filter = { sizeof(FILTERKEYS), 0 };
static void restore_accessibility_shortcuts(void){
    SystemParametersInfoA(SPI_SETSTICKYKEYS, sizeof(STICKYKEYS), &saved_sticky, 0);
    SystemParametersInfoA(SPI_SETTOGGLEKEYS, sizeof(TOGGLEKEYS), &saved_toggle, 0);
    SystemParametersInfoA(SPI_SETFILTERKEYS, sizeof(FILTERKEYS), &saved_filter, 0);
}
static void suppress_accessibility_shortcuts(void){
    STICKYKEYS sk; TOGGLEKEYS tk; FILTERKEYS fk;
    SystemParametersInfoA(SPI_GETSTICKYKEYS, sizeof(sk), &sk, 0);
    SystemParametersInfoA(SPI_GETTOGGLEKEYS, sizeof(tk), &tk, 0);
    SystemParametersInfoA(SPI_GETFILTERKEYS, sizeof(fk), &fk, 0);
    saved_sticky = sk; saved_toggle = tk; saved_filter = fk;
    atexit(restore_accessibility_shortcuts);
    if(!(sk.dwFlags & SKF_STICKYKEYSON)){
        sk.dwFlags &= (DWORD)~(SKF_HOTKEYACTIVE|SKF_CONFIRMHOTKEY);
        SystemParametersInfoA(SPI_SETSTICKYKEYS, sizeof(sk), &sk, 0);
    }
    if(!(tk.dwFlags & TKF_TOGGLEKEYSON)){
        tk.dwFlags &= (DWORD)~(TKF_HOTKEYACTIVE|TKF_CONFIRMHOTKEY);
        SystemParametersInfoA(SPI_SETTOGGLEKEYS, sizeof(tk), &tk, 0);
    }
    if(!(fk.dwFlags & FKF_FILTERKEYSON)){
        fk.dwFlags &= (DWORD)~(FKF_HOTKEYACTIVE|FKF_CONFIRMHOTKEY);
        SystemParametersInfoA(SPI_SETFILTERKEYS, sizeof(fk), &fk, 0);
    }
}

void plat_init(const char *title){
    WNDCLASSA wc;
    RECT r;
    suppress_accessibility_shortcuts();
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "pfemu";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    r.left=0; r.top=0; r.right=win_w; r.bottom=win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("pfemu", title, WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right-r.left, r.bottom-r.top, NULL,NULL,wc.hInstance,NULL);
    hdc = GetDC(hwnd);
    memset(&bmi,0,sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    build_fonts();
}

int plat_pump(void){
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    return running;
}

/* Seed present window, as a fraction of vtotal, used only until the redraw
 * bands have been learned from the running game (fantasies_present_window(),
 * src/fantasies.c - see docs #28).  These bounds come from the original
 * -balldbg measurement: the earliest mid-frame band seen across the four
 * shipped tables starts at line 222.7/527 = 0.422, and the vblank band ends
 * by 522/527. */
#define PRESENT_PHASE_LO 0.114   /* line  60/527 */
#define PRESENT_PHASE_HI 0.379   /* line 200/527 */
int present_phaselock = 1;       /* -nophaselock reverts to the wall timer */

void plat_present(const uint32_t *pix, int w, int h){
    int dw = win_w, dh = win_h, dx = 0, dy = 0;
    double ar = (double)w / (double)h * (w==320 && h==200 ? 1.2 : 1.0);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    if((double)dw/dh > ar){ dw = (int)(dh*ar); dx = (win_w-dw)/2; }
    else { dh = (int)(dw/ar); dy = (win_h-dh)/2; }
    if(integer_scale){ }
    if(dx>0){ RECT r={0,0,dx,win_h}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
              r.left=dx+dw; r.right=win_w; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
    if(dy>0){ RECT r={0,0,win_w,dy}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
              r.top=dy+dh; r.bottom=win_h; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, dx,dy,dw,dh, 0,0,w,h, pix, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

double plat_time(void){
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
void plat_sleep_ms(int ms){ Sleep(ms); }

static void save_ppm(const char *path, const uint32_t *pix, int w, int h){
    FILE *f = fopen(path,"wb");
    int i;
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(i=0;i<w*h;i++){
        uint8_t rgb[3];
        rgb[0]=(uint8_t)(pix[i]>>16); rgb[1]=(uint8_t)(pix[i]>>8); rgb[2]=(uint8_t)pix[i];
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}

/* F11 screenshots: timestamped filename, written via save_png() (src/png.c). */
static void take_screenshot(const uint32_t *pix, int w, int h){
    SYSTEMTIME st;
    char path[96];
    int n;
    CreateDirectoryA("screenshots", NULL); /* ok if it already exists */
    GetLocalTime(&st);
    for(n=0; n<100; n++){
        if(n==0)
            sprintf(path, "screenshots/pfemu_%04d%02d%02d_%02d%02d%02d.png",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            sprintf(path, "screenshots/pfemu_%04d%02d%02d_%02d%02d%02d_%d.png",
                    st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, n);
        if(GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) break;
    }
    if(save_png(path, pix, w, h)) fprintf(stderr, "[pfemu] screenshot saved: %s\n", path);
    else fprintf(stderr, "[pfemu] screenshot failed: %s\n", path);
}

/* "t:scancode:updown,..." - drive the keyboard from a script for testing */
static void run_keyscript(const char *s, double now){
    static int done[256];
    const char *p = s;
    int idx = 0;
    while(*p){
        double t = atof(p);
        const char *c1 = strchr(p,':');
        if(!c1) break;
        {
            int sc = (int)strtol(c1+1,NULL,16);
            const char *c2 = strchr(c1+1,':');
            int dn = c2 ? atoi(c2+1) : 1;
            if(now >= t && !done[idx&255]){ done[idx&255]=1; kbd_key(sc,dn); }
        }
        p = strchr(p,',');
        if(!p) break;
        p++; idx++;
    }
}

static unsigned long irq_count[32];

/* ------------------------------------------------------------ main loop */
int main(int argc, char **argv){
    const char *dir = NULL;   /* -d, the launcher, or release_scan() */
    const char *prog = NULL;  /* -p/-setup, or the detected release's boot file */
    const char *force_release = NULL; /* -release ID: skip detection's verdict */
    int list_releases = 0;    /* -releases: print the detection report and exit */
    RelResult rel;
    LaunchChoice lc;
    int no_launcher = 0;      /* -nolauncher: skip the picker dialog */
    int explicit_prog = 0;    /* -p / -setup names the program directly */
    int start_fullscreen = 0; /* -fullscreen, or the launcher's checkbox */
    /* Windows-subsystem binary: no console of its own, so double-clicking
     * shows only the UI.  When started from a console, reattach to it so
     * CLI output (-secs stats, traces) still works — but never steal a
     * redirected stderr, so `2>file` log capture keeps working. */
    { HANDLE he = GetStdHandle(STD_ERROR_HANDLE);
      if(he == NULL || he == INVALID_HANDLE_VALUE ||
         GetFileType(he) == FILE_TYPE_UNKNOWN){
          if(AttachConsole(ATTACH_PARENT_PROCESS)){
              freopen("CONOUT$", "w", stdout);
              freopen("CONOUT$", "w", stderr);
          }
      } }
    double t0, last_present = 0;
    double max_secs = 0;
    const char *shotfile = NULL;
    const char *keyscript = NULL;
    double shot_every = 0, next_shot = 0;
    double speed = 1.0;
    int vol_override = -1;    /* -vol N overrides the saved slider position */
    unsigned long mem_lo = 0;
    int shot_n = 0;
    int i;

    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-d") && i+1<argc) dir = argv[++i];
        else if(!strcmp(argv[i],"-p") && i+1<argc){ prog = argv[++i]; explicit_prog = 1; }
        /* -setup: boot the sound-configuration utility instead of the game.
         * Equivalent to -p SETSOUND.EXE, but discoverable.  Pick SoundBlaster
         * (base 220h, IRQ 7), answer its questions, and it writes SOUND.CFG;
         * the write goes to PFEMU-STATE/ via the DOS overlay, so the
         * installed files stay pristine.  The game then uses it on next boot. */
        else if(!strcmp(argv[i],"-setup")){ prog = "SETSOUND.EXE"; explicit_prog = 1; }
        else if(!strcmp(argv[i],"-t")){ trace_level = 1; trace_fp = fopen("pfemu.log","w"); }
        else if(!strcmp(argv[i],"-ips") && i+1<argc) emu_ips = atof(argv[++i]);
        else if(!strcmp(argv[i],"-secs") && i+1<argc) max_secs = atof(argv[++i]);
        else if(!strcmp(argv[i],"-shot") && i+1<argc) shotfile = argv[++i];
        else if(!strcmp(argv[i],"-keys") && i+1<argc) keyscript = argv[++i];
        else if(!strcmp(argv[i],"-shotevery") && i+1<argc) shot_every = atof(argv[++i]);
        else if(!strcmp(argv[i],"-force256")) vga_force256 = 1;
        else if(!strcmp(argv[i],"-nodbl")) vga_nodbl = 1;
        else if(!strcmp(argv[i],"-oldtiming")){ extern int vga_old_timing; vga_old_timing = 1; }
        else if(!strcmp(argv[i],"-dosdbg")){ extern int dos_log_all; dos_log_all = 1; }
        else if(!strcmp(argv[i],"-pll") && i+1<argc){ extern int pll_dbg; pll_dbg = atoi(argv[++i]); }
        else if(!strcmp(argv[i],"-xring")){ extern int x_on; x_on = 1; }
        else if(!strcmp(argv[i],"-iotrace") && i+1<argc){ extern int io_trace; io_trace = atoi(argv[++i]); }
        else if(!strcmp(argv[i],"-wav") && i+1<argc){ extern const char *wav_path; wav_path = argv[++i]; }
        else if(!strcmp(argv[i],"-snddbg")){ extern int sound_debug; sound_debug = 1; }
        else if(!strcmp(argv[i],"-vol") && i+1<argc) vol_override = atoi(argv[++i]);
        else if(!strcmp(argv[i],"-dmairq")){ extern int sb_dmairq; sb_dmairq = 1; }
        else if(!strcmp(argv[i],"-nopatch")){ extern int dos_no_patch; dos_no_patch = 1; }
        else if(!strcmp(argv[i],"-mem") && i+1<argc){ mem_lo = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-balldbg")){ balldbg_on = 1; }
        else if(!strcmp(argv[i],"-nophaselock")){ present_phaselock = 0; }
        else if(!strcmp(argv[i],"-nolatch")){ vga_latch_start = 0; }
        else if(!strcmp(argv[i],"-noballsync")){ vga_ballsync = 0; }
        else if(!strcmp(argv[i],"-trapexit")){ extern int dos_trap_exit; extern int x_on; dos_trap_exit = 1; x_on = 1; }
        else if(!strcmp(argv[i],"-intwatch") && i+1<argc){ extern int int_watch; int_watch = (int)strtol(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-trap") && i+2<argc){ extern uint32_t x_trap_lo, x_trap_hi; extern int x_on;
            x_on = 1; x_trap_lo = strtoul(argv[++i],NULL,16); x_trap_hi = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-speed") && i+1<argc) speed = atof(argv[++i]);
        else if(!strcmp(argv[i],"-nolauncher")) no_launcher = 1;
        else if(!strcmp(argv[i],"-fullscreen")) start_fullscreen = 1;
        else if(!strcmp(argv[i],"-flipdbg")) vga_flipdbg = 1;
        else if(!strcmp(argv[i],"-dmd")) vga_dmdlog = 1;
        else if(!strcmp(argv[i],"-matdbg")){ extern int pit0_m0_log;
            mat_dbg = 1; pit0_m0_log = 60; }
        else if(!strcmp(argv[i],"-pitm0")){ extern int pit_m0_exact; pit_m0_exact = 1; }
        else if(!strcmp(argv[i],"-nopitm0")){ extern int pit_m0_exact; pit_m0_exact = 0; }
        else if(!strcmp(argv[i],"-paldbg")) vga_paldbg = 1;
        else if(!strcmp(argv[i],"-vscan") && i+1<argc) vscan_step = strtoull(argv[++i],NULL,10);
        /* -releases: hash every installation found and print the full report.
         * This is also the intake format for a version not in the database -
         * it prints the five sizes and SHA-256s without running any of it. */
        else if(!strcmp(argv[i],"-releases")) list_releases = 1;
        /* -release ID: treat the directory as that release whatever the
         * hashes say.  For development, and for an installation that is a
         * known build with something harmless changed.  It is the one way to
         * get release metadata applied to code that was not recognised, so
         * it is a switch the user has to type, never a fallback. */
        else if(!strcmp(argv[i],"-release") && i+1<argc) force_release = argv[++i];
    }
    if(emu_ips <= 0.0) emu_ips = 6000000.0;
    emu_inv_ips = 1.0 / emu_ips;

    if(list_releases){
        static RelResult found[8];
        int n, k;
        /* -d narrows it to one directory; otherwise report every one found. */
        if(dir) n = release_detect(dir, &found[0]) == 0 ? 1 : 0;
        else n = release_scan(found, 8);
        if(!n){
            printf("No Pinball Fantasies installation found.\n"
                   "Put one release's files directly in a directory named GAME.\n");
            return 1;
        }
        for(k=0;k<n;k++){
            printf("=== %s: %s ===\n%s", found[k].dir, found[k].summary,
                   found[k].detail);
            if(k+1 < n) printf("\n");
        }
        return 0;
    }

    /* Game picker, unless the run is explicit or automated: -p/-setup name
     * the program, -secs means a headless benchmark, -nolauncher forces the
     * old behaviour (boot dir/prog straight away).  In picker mode -d is
     * ignored: the directory comes from the selected installation. */
    if(!no_launcher && !explicit_prog && max_secs <= 0.0){
        if(!show_launcher(&lc)) return 0;
        dir = lc.dir; prog = lc.prog;
        if(lc.fullscreen) start_fullscreen = 1;
    }
    /* No -d and no launcher: take the first installation the detector finds
     * (GAME, then the rest alphabetically), so a headless run needs no more
     * arguments than an interactive one. */
    if(!dir){
        static RelResult found[8];
        int n = release_scan(found, 8), k;
        for(k=0;k<n;k++) if(release_runnable(&found[k])){ dir = found[k].dir; break; }
        if(!dir) dir = n ? found[0].dir : "GAME";
    }

    /* Identify the release by content.  The launcher already did this for its
     * own choice; -d and -nolauncher runs land here having done nothing, and
     * every path needs the descriptor, so it simply runs again - hashing 2.4 MB
     * is not worth the bookkeeping to avoid. */
    release_detect(dir, &rel);
    if(force_release){
        const Release *fr = release_by_id(force_release);
        if(!fr){
            int k;
            fprintf(stderr,"unknown release id '%s'; known ids are:", force_release);
            for(k=0;k<release_count();k++) fprintf(stderr," %s", release_at(k)->id);
            fprintf(stderr,"\n");
            return 1;
        }
        fprintf(stderr,"[release] forced to '%s' (detected: %s)\n",
                fr->id, release_state_name(rel.state));
        rel.rel = fr;
        rel.state = REL_RECOGNIZED;
        if(!rel.boot[0]) snprintf(rel.boot, sizeof(rel.boot), "%s", fr->boot);
    }
    /* The launcher refuses to start an unrecognised installation.  The command
     * line does not: booting one unpatched is how a newly found release gets
     * tried in the first place.  It runs with every Fantasies-specific fix
     * off, which is the only safe thing to do with an intro whose memory
     * layout is unknown - say so rather than let it look like a bad port. */
    if(!release_runnable(&rel))
        fprintf(stderr,"[release] %s: %s\n"
                       "[release] booting with all Pinball Fantasies fixes off."
                       " Run -releases for the full report.\n",
                release_state_name(rel.state), rel.summary);
    /* The boot program is the release's, not a constant: Power Pack renamed
     * PINBALL.EXE to PF.EXE.  -p still overrides for direct table/intro boots. */
    if(!prog) prog = rel.boot[0] ? rel.boot : "PINBALL.EXE";

    /* Output level: -vol wins, else whatever the launcher's slider was left
     * at for this install (read after the dialog, which has just written it).
     * Headless and -nolauncher runs land on the same saved value, so a
     * scripted run sounds like an interactive one. */
    audio_volume = vol_override >= 0 ? vol_override : read_volume_cfg(dir);
    if(audio_volume < 0) audio_volume = 0;
    if(audio_volume > 100) audio_volume = 100;

    /* Arm the Fantasies session.  Nothing Fantasies-specific runs unless the
     * directory's five program hashes identified an actual release, so no
     * Fantasies-only behaviour can leak into a sibling game however its files
     * happen to be named - and, just as importantly, no intro's memory layout
     * can be poked into a different intro's code. */
    fantasies_begin_session(dir, prog, release_runnable(&rel) ? rel.rel : NULL);

    ram = (uint8_t*)calloc(RAM_SIZE,1);
    if(!ram){ fprintf(stderr,"out of memory\n"); return 1; }

    cpu_reset();
    vga_init();
    dev_init();
    bios_init();
    dos_init(dir);
    plat_init("Pinball Fantasies - pfemu");
    if(start_fullscreen) plat_set_fullscreen(1);

    /* Hand-built boot: park the CPU on a HLT in ROM, then EXEC the program. */
    ram[0xFFFF0] = 0xF4;
    set_sreg(S_CS,0xF000); cpu.eip = 0xFFF0;
    set_sreg(S_SS,0x0050); REG16(R_ESP) = 0x0100;
    set_sreg(S_DS,0x0000); set_sreg(S_ES,0x0000);
    cpu.iflag = 1;
    /* fabricate an IRET frame so a terminating child has something to return to */
    {
        uint32_t sp;
        REG16(R_ESP) -= 6;
        sp = cpu.sbase[S_SS] + REG16(R_ESP);
        mem_w16(sp+0, 0xFFF0);
        mem_w16(sp+2, 0xF000);
        mem_w16(sp+4, 0x0202);
    }
    if(dos_exec(prog, 0, 0, 0, 0) != 0){
        fprintf(stderr,"could not load %s from %s\n", prog, dir);
        return 1;
    }

    t0 = plat_time();
    { unsigned long long pres_last_frame = 0; int pending_present = 0;
    while(plat_pump() && !cpu.shutdown){
        double wall = plat_time() - t0;
        double real = wall * speed;
        if(max_secs > 0 && wall > max_secs) break;
        kbd_reconcile_physical();
        if(keyscript) run_keyscript(keyscript, real);
        int guard = 0;
        while(emu_time < real && !cpu.shutdown && guard < 10000){
            int n;
            if(cpu.halted){
                /* idle: jump the clock forward to the next scheduled event */
                cpu.cycles += (uint64_t)(emu_ips / 10000.0);
                dev_tick();
            } else {
                /* Run up to 256 instructions, but never past the next timer
                 * deadline: IRQ0 has to land on the instruction it is due on,
                 * not up to a batch later.  The deadline clamp keeps timer
                 * precision identical to the old 64-instruction batch while
                 * the bigger batch amortises dev_tick/pic_pending over 4x
                 * the work.  Worst-case IRQ latency (~43 us at 6 MIPS) is
                 * far below anything the game can observe (PIT tick 55 ms). */
                uint64_t dl = dev_next_deadline();
                int lim = 256;
                /* A deadline that is already here (dl == cpu.cycles, because
                 * dev_next_deadline() truncates the remaining instruction
                 * count down) used to fail the `dl > cpu.cycles` test and fall
                 * through to a full 256-instruction batch - so every timer
                 * interrupt was serviced about a batch late.  That is the
                 * ~51-tick systematic overshoot -matdbg measured, and the
                 * sound driver subtracts it from the delay it programs next
                 * (see pit_count() in dev.c).  Run a single instruction
                 * instead and let dev_tick() pick it up: the unarmed window
                 * that follows is bounded separately, so this cannot turn
                 * into single-stepping. */
                if(dl <= cpu.cycles) lim = 1;
                else if(dl - cpu.cycles < 256) lim = (int)(dl - cpu.cycles);
                if(lim < 1) lim = 1;
                for(n=0;n<lim;n++) cpu_step();
                dev_tick();
                vga_vscan_poll();
            }
            if(cpu.iflag){
                int v = pic_pending();
                if(v >= 0){
                    /* IRQ0 latency: how long after the one-shot came due the
                     * guest's handler actually starts.  The sound driver's ISR
                     * reads the counter to subtract exactly this from its next
                     * delay, so a large one corrupts its schedule (dev.c). */
                    if(v == 8){
                        extern double pit0_due, pit0_raise_t;
                        extern double pit0_lat_sum, pit0_lat_max;
                        extern double pit0_over_sum, pit0_over_max;
                        extern double pit0_wait_sum, pit0_wait_max;
                        extern unsigned long pit0_lat_n;
                        if(pit0_due >= 0.0){
                            double lat  = (emu_now()    - pit0_due)     * 1193182.0;
                            double over = (pit0_raise_t - pit0_due)     * 1193182.0;
                            double wait = (emu_now()    - pit0_raise_t) * 1193182.0;
                            if(lat  < 0.0) lat  = 0.0;
                            if(over < 0.0) over = 0.0;
                            if(wait < 0.0) wait = 0.0;
                            pit0_lat_n++;
                            pit0_lat_sum  += lat;  if(lat  > pit0_lat_max)  pit0_lat_max  = lat;
                            pit0_over_sum += over; if(over > pit0_over_max) pit0_over_max = over;
                            pit0_wait_sum += wait; if(wait > pit0_wait_max) pit0_wait_max = wait;
                            pit0_due = -1.0;
                        }
                    }
                    irq_count[v&31]++; cpu_interrupt(v, 0);
                }
            }
            /* Decide the present phase HERE, not after the catch-up loop.
             * This loop advances emulated time in ~10 us batches, but one
             * outer iteration can cover most of a frame (plat_sleep_ms(1)
             * is coarse on Windows), so testing the phase only out there
             * overshot the window constantly: measured with -balldbg,
             * 55% of presents were taken by the fell-behind fallback at an
             * arbitrary phase, which is why phase-locking only halved the
             * dropped-ball rate instead of removing it.  Breaking out on
             * the batch that enters the window costs one deferred batch
             * and lands the sample where it was aimed. */
            if(present_phaselock && !pending_present){
                double per2, inv2, hde2; int vt2, vd2, vrs2, vre2;
                unsigned long long idx2;
                vga_timing_cached(&per2, &inv2, &vt2, &vd2, &vrs2, &vre2, &hde2);
                idx2 = per2 > 0.0 ? (unsigned long long)(emu_time / per2) : 0ULL;
                if(idx2 != pres_last_frame){
                    double f2 = (vt2 > 0) ? vga_scanline_now(NULL) / (double)vt2 : 0.0;
                    double lo = PRESENT_PHASE_LO, hi = PRESENT_PHASE_HI;
                    int in;
                    /* the seed constants only stand until the bands have been
                     * learned from the game itself (src/fantasies.c) */
                    fantasies_present_window(&lo, &hi);
                    in = (lo <= hi) ? (f2 >= lo && f2 <= hi)
                                    : (f2 >= lo || f2 <= hi);   /* span may wrap */
                    if(in){
                        pending_present = 1;
                        break;
                    }
                }
            }
            guard++;
        }
        if(emu_time < real - 0.25*speed) { t0 = plat_time() - emu_time/speed; }  /* fell behind */

        /* No duplicate presents: the game renders at 59.71 Hz but the wall
         * timer runs at 60 Hz, so every ~3.4 s a frame went out twice
         * (scroll judder).  Gate on the emulated frame index so each frame
         * presents at most once.
         *
         * Then pick WHERE in the emulated frame to sample it.  The engine has
         * no sprite double-buffer: PUTTHEBALL erases the ball, blits the
         * flippers, and redraws it, and vga_render - which snapshots all of
         * VRAM at one instant, with no beam - drops the ball entirely if it
         * samples inside that gap.  The game hides the gap from a CRT by
         * racing the beam (docs §26), redrawing a lower-half ball during
         * vblank and deferring an upper-half one to the mid-frame raster
         * interrupt, so the erase/redraw is always in the half the beam is
         * not painting.  Measured with -balldbg over 1305 redraws, those two
         * bands sit at lines 231-260 and 491-521 of 527, leaving two ~7.4 ms
         * quiet spans against a 0.97 ms worst-case gap.
         *
         * Sample in the early-frame span (lines ~521-231, wrapping).  That is
         * the CRT-faithful one: a high ball is painted by the beam before the
         * mid-frame redraw, so showing the pre-redraw state is exactly what
         * hardware displayed, and a low ball is unchanged across both spans.
         * Sampling the other span would show a high ball one tick early,
         * which also makes it step backwards when it crosses mid-screen.
         *
         * This is what docs §16 got wrong: phase-locking is sound, but it
         * locked to vsync, which is precisely when a lower-half ball is being
         * redrawn.  -nophaselock restores the old drifting wall timer. */
        { double per, inv, hde; int vt, vd, vrs, vre;
          unsigned long long idx;
          int go, fell_behind = 0;
          vga_timing_cached(&per, &inv, &vt, &vd, &vrs, &vre, &hde);
          (void)inv; (void)vt; (void)vd; (void)vrs; (void)vre; (void)hde;
          idx = per > 0.0 ? (unsigned long long)(emu_time / per) : 0ULL;
          if(!present_phaselock){
            go = (plat_time() - last_present > 1.0/60.0) && idx != pres_last_frame;
          }else{
            /* the catch-up loop above picked the moment; only take it late
             * (without a phase of our choosing) if a whole frame was missed */
            fell_behind = (idx > pres_last_frame + 1) && !pending_present;
            go = (idx != pres_last_frame) && (pending_present || fell_behind);
            if(go && plat_time() - last_present < 1.0/240.0) go = 0;
          }
          pending_present = 0;
          if(go){
            pres_last_frame = idx;
            last_present = plat_time();
            fantasies_ballgap_present(fell_behind);
            vga_render(fb, &fbw, &fbh);
            osd_draw(fb, fbw, fbh);
            plat_present(fb, fbw, fbh);
            if(shot_every > 0 && real >= next_shot){
                char nm[64];
                next_shot = real + shot_every;
                sprintf(nm, "seq%03d.ppm", shot_n++);
                save_ppm(nm, fb, fbw, fbh);
            }
            if(screenshot_pending){
                screenshot_pending = 0;
                take_screenshot(fb, fbw, fbh);
            }
          } }
        plat_sleep_ms(1);
    }
    }

    /* Whatever -/+ settled on outlives the session: the launcher's slider
     * sets the starting level, and having to go back to the dialog to make a
     * level stick would defeat the point of the keys.  First thing after the
     * loop, so a wobble anywhere in the exit report below can't lose it.
     *
     * Muting is deliberately not a saved preference - quitting while muted
     * saves the level the mute is hiding, so the next session isn't silent
     * for no visible reason.  Riding - all the way down to 0 does save 0:
     * that one is explicit, and the launcher shows it as 0%.
     *
     * -vol on its own never writes; only an in-window change does. */
    if(audio_volume_dirty)
        write_volume_cfg(dir, vol_muted && vol_premute > 0 ? vol_premute : audio_volume);

    vga_render(fb,&fbw,&fbh);
    plat_present(fb,fbw,fbh);
    if(shotfile) save_ppm(shotfile, fb, fbw, fbh);
    { extern void vga_dump(void); vga_dump(); }
    { extern unsigned long st1_calls, st1_bit0, st1_bit3;
      printf("[pfemu] 3DA reads=%lu  bit0(blank)=%lu  bit3(vsync)=%lu\n",
             st1_calls, st1_bit0, st1_bit3); }
    { extern void st1_report(void); st1_report(); }
    fantasies_ballgap_report();
    fantasies_matrix_report();
    { extern unsigned long vsync_edges;
      printf("[pfemu] vsync edges seen = %lu (%.1f/s)\n",
             vsync_edges, vsync_edges/(emu_time>0?emu_time:1)); }
    { extern double vga_frame_hz(void); printf("[pfemu] CRT refresh = %.2f Hz\n", vga_frame_hz()); }
    { extern unsigned long vga_startaddr_changes;
      printf("[pfemu] page flips=%lu (%.1f/s of emulated time)\n",
             vga_startaddr_changes, vga_startaddr_changes/(emu_time>0?emu_time:1)); }
    { extern unsigned long vga_ar14_switches, vga_ar14_overrides, vga_mode_resets;
      printf("[pfemu] AR14 switches=%lu overrides=%lu mode-resets=%lu\n",
             vga_ar14_switches, vga_ar14_overrides, vga_mode_resets); }
    { extern unsigned long kbd_port60_reads; extern uint8_t pic_imr(void);
      printf("[pfemu] port60 reads=%lu  master IMR=%02X\n", kbd_port60_reads, pic_imr()); }
    printf("[pfemu] irqs: int8=%lu int9=%lu ticks=%u iflag=%d halted=%d cs:ip=%04X:%04X\n",
           irq_count[8], irq_count[9], *(unsigned short*)&ram[0x46C],
           (int)cpu.iflag, cpu.halted, cpu.sreg[S_CS], (unsigned)cpu.eip);
    { uint32_t sstop = cpu.sbase[S_SS] + REG16(R_ESP); int i;
      /* Top of the guest stack: a near caller's return IP sits at [SP],
       * so a hang inside a helper (vsync wait, decode loop) still names
       * its call site. */
      printf("[pfemu] ss:sp=%04X:%04X stack:", cpu.sreg[S_SS], REG16(R_ESP));
      for(i=0;i<16;i++) printf(" %04X", mem_r16((sstop + (uint32_t)(i*2)) & 0xFFFFF));
      printf("\n"); }
    if(mem_lo){ int k; printf("[mem] %05lX:\n", mem_lo);
        for(k=0;k<256;k++){ if((k&15)==0) printf("  %05lX:", mem_lo+k);
            printf(" %02X", ram[(mem_lo+k)&0xFFFFF]); if((k&15)==15) printf("\n"); } }
    { extern void dev_state_dump(void); dev_state_dump(); }
    { extern uint32_t x_ring[]; extern uint16_t x_cs[], x_ip[];
      extern unsigned x_pos; extern int x_on;
      if(x_on){
          /* print the ring oldest-first, collapsing straight-line runs so the
           * interesting thing - the jump that left real code - stands out */
          unsigned n = x_pos < 8192 ? x_pos : 8192, k;
          uint32_t prev = 0xFFFFFFFFu;
          printf("[xring] last %u instruction addresses (jumps only):\n", n);
          for(k=0;k<n;k++){
              unsigned idx = (x_pos - n + k) & 8191;
              uint32_t lin = x_ring[idx];
              if(prev == 0xFFFFFFFFu || lin < prev || lin > prev + 15)
                  printf("  %04X:%04X  lin=%05X\n", x_cs[idx], x_ip[idx], lin);
              prev = lin;
          }
      } }
    printf("[pfemu] stopped: emu_time=%.3fs instructions=%llu mode=%02Xh %dx%d\n",
           emu_time, (unsigned long long)cpu.cycles, vga_get_mode(), fbw, fbh);
    /* hex window around the final CS:IP - enough to disassemble whatever loop
     * the guest was spinning in when we stopped */
    { uint32_t lin = cpu.sbase[S_CS] + cpu.eip; uint32_t s = lin > 0x60 ? lin-0x60 : 0; int i;
      printf("[pfemu] code @ linear %05X (cs:ip %04X:%04X):\n", (unsigned)lin,
             cpu.sreg[S_CS], (unsigned)cpu.eip);
      for(i=0;i<0xC0;i++){
          if((i&15)==0) printf("  %05X:", (unsigned)(s+i));
          printf(" %02X", ram[(s+i)&0xFFFFF]);
          if((i&15)==15) printf("\n");
      } }
    { extern void wav_close(void); extern void plat_audio_close(void);
      wav_close(); plat_audio_close(); }
    if(trace_fp) fclose(trace_fp);
    return 0;
}
