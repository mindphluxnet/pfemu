/* Win32 host: window, framebuffer presentation, keyboard, console and DPI
 * setup.  The emulation loop itself is in src/run.c. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <timeapi.h>
#include <stdarg.h>
#include "pfemu.h"
#include "../res/resource.h"

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


/* -------------------------------------------------------------- window  */
static HWND hwnd;
static HDC  hdc;
static BITMAPINFO bmi;
static int running = 1;
static int win_w = 960, win_h = 600;
static int integer_scale = 0;
static int fullscreen = 0;
static LONG windowed_style;
static RECT windowed_rect;
/* Presentation back buffer: the game frame, bars and badges are composed
 * here and reach the screen in a single BitBlt, so the compositor can never
 * catch the window half-drawn (which is what made the first screen-space
 * REC badge flicker: bar-erase and badge were separate ops on the visible
 * surface).  Rebuilt on resize. */
static HDC back_dc = NULL;
static HBITMAP back_bmp = NULL;
static int back_w = 0, back_h = 0;
static void back_drop(void){
    if(back_bmp){ DeleteObject(back_bmp); back_bmp = NULL; }
    if(back_dc){ DeleteDC(back_dc); back_dc = NULL; }
    back_w = back_h = 0;
}
static int back_ensure(void){
    if(back_dc && back_w == win_w && back_h == win_h) return 1;
    if(win_w <= 0 || win_h <= 0) return 0;
    back_drop();
    back_dc = CreateCompatibleDC(hdc);
    if(!back_dc) return 0;
    back_bmp = CreateCompatibleBitmap(hdc, win_w, win_h);
    if(!back_bmp){ back_drop(); return 0; }
    SelectObject(back_dc, back_bmp);
    back_w = win_w; back_h = win_h;
    return 1;
}


/* ----------------------------------------------------------------- OSD --
 * Host-only on-screen notification (volume level, trainer toggles),
 * composed into the back buffer in screen space like the session badges:
 * independent of whatever video page or mode the game is using, nothing
 * the guest can see or overwrite, crisp system text, and invisible to the
 * -shotevery/-shot/screenshot captures.  It started out as the trainer's
 * confirmation message and lived in src/fantasies.c; the volume keys want
 * the same thing and are not game behaviour, so it sits here with the rest
 * of the host presentation and fantasies.c calls in like any other caller. */
static char osd_text[48] = "";
static double osd_until = 0;

/* Lifetime is WALL time, not emu_time.  The OSD is host UI, and the things
 * that most need to report themselves are exactly the ones that move the
 * emulated clock: a seek advances emu_time past the deadline before a single
 * frame is presented, so the message was retired before it was ever drawn,
 * and a pause stops emu_time so it would hang on screen forever.  Neither is
 * visible to the guest either way - the text is composed into the back
 * buffer and kept out of captures. */
void osd_show(const char *text){
    snprintf(osd_text, sizeof(osd_text), "%s", text);
    osd_until = plat_time() + 1.6;
}

void osd_clear(void){
    osd_text[0] = 0;
    osd_until = 0;
}

/* Persistent session badges (docs/REPLAY.md polish): a tiny REC badge while
 * -record / launcher Record mode is running, a green PLAY twin while
 * replaying.  Host-only - never into guest state, never logged, zero effect
 * on the session - and drawn in SCREEN space into the back buffer (see
 * plat_present below), so they sit in the true window/monitor corner rather
 * than the stretched game image's corner, stay crisp (no stretch blur),
 * compose tear-free in one blit, and can never leak into
 * -shotevery/-shot/screenshot captures (those read the framebuffer, which
 * badges never touch).  Deliberately minimal and static (no blink,
 * one corner): they must never cover playfield or distract.  The transient
 * OSD notice below joins them in screen space (bottom-center). */
/* Badge typeface and swatches, created once (process-lifetime objects, like
 * the back buffer itself).  Segoe UI is the system font on every supported
 * Windows; the stock GUI font is the fallback, never a failure. */
static HFONT badge_font = NULL;
static HBRUSH badge_box = NULL, badge_red = NULL, badge_green = NULL;
static void badge_gdi_init(void){
    if(badge_font) return;
    badge_font = CreateFontA(-15, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH|FF_DONTCARE,
                             "Segoe UI");
    if(!badge_font) badge_font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    badge_box = CreateSolidBrush(RGB(0x14,0x14,0x14));
    badge_red = CreateSolidBrush(RGB(0xE0,0x40,0x40));
    badge_green = CreateSolidBrush(RGB(0x40,0xC0,0x60));
}
static void rec_draw_dc(HDC dc){
    /* REC while recording (red), PLAY while replaying (green): same corner,
     * same rules, readable at a glance.  Real GDI text (anti-aliased, measured
     * for its box), not hand-plotted pixels - this runs in screen space, so
     * there is no resolution to match and no reason to look retro. */
    const char *t;
    COLORREF ink;
    HBRUSH dot;
    int boxw, boxh, x0, y0, tlen;
    SIZE sz;
    RECT r;
    HFONT oldf;
    int oldbk;
    COLORREF oldtx;
    if(replay_is_recording()){ t = "REC"; dot = badge_red; ink = RGB(0xFF,0xE0,0x40); }
    else if(replay_is_replaying()){ t = "PLAY"; dot = badge_green; ink = RGB(0x70,0xFF,0x90); }
    else return;
    badge_gdi_init();
    if(!badge_font || !badge_box || !dot) return;
    tlen = (int)strlen(t);
    oldf = (HFONT)SelectObject(dc, badge_font);
    GetTextExtentPoint32A(dc, t, tlen, &sz);
    boxw = 6+10+6+sz.cx+8;
    boxh = sz.cy+12;
    if(win_w < boxw+8 || win_h < boxh+8){ SelectObject(dc, oldf); return; }
    x0 = win_w - boxw - 6;
    y0 = 6;
    r.left = x0; r.top = y0; r.right = x0+boxw; r.bottom = y0+boxh;
    FillRect(dc, &r, badge_box);
    {   HBRUSH oldb = (HBRUSH)SelectObject(dc, dot);
        HPEN oldp = (HPEN)SelectObject(dc, GetStockObject(NULL_PEN));
        int cy = y0 + boxh/2;
        Ellipse(dc, x0+6, cy-5, x0+16, cy+5);
        SelectObject(dc, oldp);
        SelectObject(dc, oldb);
    }
    oldbk = SetBkMode(dc, TRANSPARENT);
    oldtx = SetTextColor(dc, ink);
    TextOutA(dc, x0+6+10+6, y0+(boxh-sz.cy)/2, t, tlen);
    SetTextColor(dc, oldtx);
    SetBkMode(dc, oldbk);
    SelectObject(dc, oldf);
}

void osd_draw_screen(HDC dc){
    int len, boxw, boxh, x0, y0;
    SIZE sz;
    RECT r;
    HFONT oldf;
    int oldbk;
    COLORREF oldtx;
    if(!osd_text[0] || plat_time() >= osd_until) return;
    badge_gdi_init();
    if(!badge_font || !badge_box) return;
    len = (int)strlen(osd_text);
    oldf = (HFONT)SelectObject(dc, badge_font);
    GetTextExtentPoint32A(dc, osd_text, len, &sz);
    boxw = sz.cx + 24;
    boxh = sz.cy + 12;
    if(win_w < boxw+16 || win_h < boxh+16){ SelectObject(dc, oldf); return; }
    x0 = (win_w - boxw) / 2;
    y0 = win_h - boxh - 12;
    r.left = x0; r.top = y0; r.right = x0+boxw; r.bottom = y0+boxh;
    FillRect(dc, &r, badge_box);
    oldbk = SetBkMode(dc, TRANSPARENT);
    oldtx = SetTextColor(dc, RGB(0xFF,0xE0,0x40));
    TextOutA(dc, x0+12, y0+(boxh-sz.cy)/2, osd_text, len);
    SetTextColor(dc, oldtx);
    SetBkMode(dc, oldbk);
    SelectObject(dc, oldf);
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
    case WM_DESTROY: case WM_CLOSE:
        /* Save here as well as after the main loop: the exit reports below
         * must not be able to lose the position, whatever fails in them. */
        plat_save_window_pos();
        running = 0; PostQuitMessage(0); return 0;
    case WM_SIZE: win_w = LOWORD(l); win_h = HIWORD(l); return 0;
    /* The user finished dragging the window: persist immediately, so a kill
     * or crash after this point still keeps the spot.  Fullscreen has no
     * movable window (see plat_save_window_pos), and programmatic moves
     * never enter the modal loop that sends this. */
    case WM_EXITSIZEMOVE:
        if(!fullscreen) plat_save_window_pos();
        return 0;
    /* Focus-loss releases are host leakage (docs/REPLAY.md section 2.4):
     * suppressed on replay, where no live keyboard reaches the guest. */
    case WM_KILLFOCUS: if(!replay_is_replaying()) kbd_release_all(); break;
    case WM_ACTIVATE: if(LOWORD(w) == WA_INACTIVE && !replay_is_replaying()) kbd_release_all(); break;
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        /* Scroll Lock quits: F12 belongs to the game (its INT 9 maps scan
         * code 58h), and so do both shifts, both alts, both controls, the
         * arrows and space. */
        if(w == VK_SCROLL){ running = 0; return 0; }
        /* Alt+Enter toggles fullscreen; bit 30 filters key-repeat so holding
         * it doesn't flap the window every auto-repeat interval.  Suppressed
         * on replay: a host-only key with no guest effect and no log entry
         * (docs/REPLAY.md section 3.3); volume keys below stay live. */
        if(m == WM_SYSKEYDOWN && w == VK_RETURN && !(l & (1<<30))){
            if(!replay_is_replaying()) set_fullscreen(!fullscreen);
            return 0;
        }
        /* F11 saves a screenshot; bit 30 filters key-repeat like Alt+Enter
         * above.  Not Print Screen: Windows 11 intercepts that itself and
         * launches Snipping Tool before this window ever sees it.  F11 isn't
         * one of the keys the game reads (see the scan-code list above). */
        if(m == WM_KEYDOWN && w == VK_F11 && !(l & (1<<30))){
            /* Suppressed on replay like Alt+Enter above (a file write the
             * recording never made); validation uses -shotevery instead. */
            if(!replay_is_replaying()) screenshot_pending = 1;
            return 0;
        }
        /* Volume: - and + (main row or keypad) in 5% steps, keypad * mutes
         * and restores.  The launcher's slider only sets the starting level,
         * so this is how the right one gets found without quitting first.
         * Key-repeat is deliberately left on: holding - to fade out is the
         * point.  Nothing emulated moves - audio_volume is host sink gain.
         *
         * The table's own INT 9 handler reads only the flippers, the plunger,
         * space and F11/F12 (docs/EMULATOR.md: launch chain), so none of these
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
        /* Keypad / toggles the enhancement bypass: the launcher's
         * Bass/Treble/Oomph/Headphones vs flat (the old sound), for A/B
         * comparison without quitting.  Sits with the other audio keys;
         * key-repeat filtered so holding it doesn't flap.  Session-only -
         * the saved settings are untouched - and host-only like volume,
         * so it stays live on replay and never reaches the guest. */
        if(m == WM_KEYDOWN && w == VK_DIVIDE && !(l & (1<<30))){
            extern int audio_enh_bypass;
            audio_enh_bypass = !audio_enh_bypass;
            { char msg[32];
              snprintf(msg, sizeof(msg), "ENHANCEMENT: %s",
                       audio_enh_bypass ? "OFF" : "ON");
              osd_show(msg); }
            fprintf(stderr, "[snd] enhancement bypass %s\n",
                    audio_enh_bypass ? "on" : "off");
            return 0;
        }
        /* F6 saves / F8 loads the single-slot snapshot (Play mode only;
         * serviced in the main loop, like F11 below).  F7 is reserved for
         * the replay stepper.  Intercepted pre-guest: zero guest effect,
         * nothing logged, whether or not the game reads these keys. */
        if(m == WM_KEYDOWN && (w == VK_F6 || w == VK_F8) && !(l & (1<<30))){
            if(w == VK_F6) snap_save_pending = 1;
            else snap_load_pending = 1;
            return 0;
        }
        /* No live-keyboard merge on replay in v1 (docs/REPLAY.md section
         * 3.3): the guest sees the recorded event list and nothing else.
         * Volume keys above stay live; they never reach kbd_key(). */
        if(sc && !replay_is_replaying()) kbd_key(sc | (ext?0xE000:0), 1);
        return 0; }
    case WM_SYSKEYUP: case WM_KEYUP: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        if(sc && !replay_is_replaying()) kbd_key(sc | (ext?0xE000:0), 0);
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
void plat_kbd_reconcile(void){
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
    WNDCLASSEXA wc;
    RECT r;
    int winx, winy, have_pos = 0;
    /* 1 ms scheduling granularity for the emulation loop.  Stock Windows
     * timer resolution makes Sleep(1) wait up to 15.6 ms, which caps the
     * outer loop (and with it presents: one per iteration at most) far
     * below what a 70 Hz hi-res frame needs, reading as scroll judder on
     * top of any emulation lag.  Restored at exit next to plat_audio_close.
     * Standard practice for real-time loops; power cost lasts one session. */
    timeBeginPeriod(1);
    suppress_accessibility_shortcuts();
    memset(&wc,0,sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "pfemu";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    /* App icon (res/pfemu.ico, IDI_PFEMU): the first ICON in the .res becomes
     * the executable's file icon automatically; the class icons put the same
     * image in the title bar, taskbar and Alt+Tab switcher. */
    wc.hIcon = LoadIcon(wc.hInstance, MAKEINTRESOURCE(IDI_PFEMU));
    wc.hIconSm = (HICON)LoadImage(wc.hInstance, MAKEINTRESOURCE(IDI_PFEMU),
                                  IMAGE_ICON, 16, 16, 0);
    RegisterClassExA(&wc);
    r.left=0; r.top=0; r.right=win_w; r.bottom=win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    {   int w = r.right-r.left, h = r.bottom-r.top;
        /* Restore the global position when there is one, else open centered
         * on the monitor with the cursor.  CW_USEDEFAULT cascades from the
         * last opened window, which reads as "random" when the OS has no
         * other anchor. */
        have_pos = last_read_winpos(&winx, &winy);
        if(have_pos){
            /* A saved spot on a monitor that is no longer there (or after a
             * resolution change) must not strand the window off-screen:
             * fall back to centered when the point is on no monitor, and
             * clamp so at least the title bar stays in the work area. */
            HMONITOR hm = MonitorFromPoint(
                (POINT){winx + w/2, winy + (h/2 < 16 ? h/2 : 16)},
                MONITOR_DEFAULTTONULL);
            if(!hm){
                center_on_cursor_monitor(w, h, &winx, &winy);
            } else {
                MONITORINFO mi;
                mi.cbSize = sizeof(mi);
                if(GetMonitorInfoA(hm, &mi)){
                    if(winx + w <= mi.rcWork.left ||
                       winx >= mi.rcWork.right ||
                       winy + h <= mi.rcWork.top ||
                       winy >= mi.rcWork.bottom){
                        /* Center on the saved monitor itself: the window
                         * belongs where it was, just visible. */
                        winx = mi.rcWork.left +
                            ((mi.rcWork.right-mi.rcWork.left)-w)/2;
                        winy = mi.rcWork.top +
                            ((mi.rcWork.bottom-mi.rcWork.top)-h)/2;
                    }
                    if(winx + w > mi.rcWork.right)
                        winx = mi.rcWork.right - w;
                    if(winy + h > mi.rcWork.bottom)
                        winy = mi.rcWork.bottom - h;
                    if(winx < mi.rcWork.left) winx = mi.rcWork.left;
                    if(winy < mi.rcWork.top) winy = mi.rcWork.top;
                }
            }
        } else {
            center_on_cursor_monitor(w, h, &winx, &winy);
        }
        hwnd = CreateWindowA("pfemu", title, WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                             winx, winy,
                             w, h, NULL,NULL,wc.hInstance,NULL);
    }
    hdc = GetDC(hwnd);
    memset(&bmi,0,sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    build_fonts();
}

/* Persist the windowed position globally (pfemu-winpos.cfg) for the next
 * launch.  Fullscreen covers the monitor, so there is nothing worth
 * remembering there - keep the pre-fullscreen rect instead.  Global, so
 * replay's promise never to touch the install's real PFEMU-STATE/ is
 * unaffected. */
void plat_save_window_pos(void){
    RECT rc;
    if(!hwnd) return;
    if(fullscreen) rc = windowed_rect;
    else if(!GetWindowRect(hwnd, &rc)) return;
    last_save_winpos((int)rc.left, (int)rc.top);
}

int plat_pump(void){
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    return running;
}


/* Where the picture lands inside the window: aspect-corrected and centred,
 * with black bars on whichever axis is left over.  Shared with F11 so a
 * screenshot is the size the picture is on screen rather than the size the
 * guest happens to render at - the two drifted apart once the window stopped
 * being a fixed multiple of 320x240. */
void plat_present_rect(int w, int h, int *dw_o, int *dh_o, int *dx_o, int *dy_o){
    int dw = win_w, dh = win_h, dx = 0, dy = 0;
    double ar = (double)w / (double)h * (w==320 && h==200 ? 1.2 : 1.0);
    if(dw <= 0 || dh <= 0){ dw = w; dh = h; }        /* no window yet */
    else if((double)dw/dh > ar){ dw = (int)(dh*ar); dx = (win_w-dw)/2; }
    else { dh = (int)(dw/ar); dy = (win_h-dh)/2; }
    if(dw < 1) dw = 1;
    if(dh < 1) dh = 1;
    *dw_o = dw; *dh_o = dh; *dx_o = dx; *dy_o = dy;
}

void plat_present(const uint32_t *pix, int w, int h){
    int dw, dh, dx, dy;
    HDC dst;
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    plat_present_rect(w, h, &dw, &dh, &dx, &dy);
    if(integer_scale){ }
    /* Compose off-screen: bars, picture and badges land in the back buffer
     * and reach the window in one BitBlt, so the compositor never catches
     * a half-drawn frame (the flicker).  Captures never see this buffer -
     * they read the emulated framebuffer before badges (see the caller). */
    dst = back_ensure() ? back_dc : hdc;
    if(dst == back_dc){
        RECT full = {0,0,win_w,win_h};
        FillRect(dst, &full, (HBRUSH)GetStockObject(BLACK_BRUSH));
    } else {
        if(dx>0){ RECT r={0,0,dx,win_h}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
                  r.left=dx+dw; r.right=win_w; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
        if(dy>0){ RECT r={0,0,win_w,dy}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
                  r.top=dy+dh; r.bottom=win_h; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
    }
    SetStretchBltMode(dst, COLORONCOLOR);
    StretchDIBits(dst, dx,dy,dw,dh, 0,0,w,h, pix, &bmi, DIB_RGB_COLORS, SRCCOPY);
    /* Screen-space overlays go last, in window pixels: the OSD notice and
     * the REC/PLAY badges (see rec_draw_dc).  None of it touches the
     * framebuffer, so captures stay clean. */
    osd_draw_screen(dst);
    rec_draw_dc(dst);
    if(dst == back_dc)
        BitBlt(hdc, 0,0,win_w,win_h, dst, 0,0, SRCCOPY);
}

double plat_time(void){
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
void plat_sleep_ms(int ms){ Sleep(ms); }


/* F11 screenshots: timestamped filename, written via save_png() (src/png.c).
 *
 * Saved at the size the picture occupies on screen, not at the guest's own
 * resolution.  Those used to be the same thing; they are not any more, and a
 * 320x240 PNG out of a fullscreen session reads as a bug even though it is
 * exactly what the guest drew.  The scale is nearest-neighbour from the
 * emulated framebuffer - the same pixels StretchDIBits puts on screen under
 * COLORONCOLOR - so nothing is invented, and for 320x200 modes it also picks
 * up the 1.2 aspect correction that makes the picture look right.  Black bars
 * are left out: they are window furniture, not picture.
 *
 * Deliberately NOT what `-shot`/`-shotevery` do.  Those are validation
 * artifacts and stay at the guest's exact resolution, because tools that
 * measure pixels (tools/dmdpanel.py) want the framebuffer, not a resampling
 * of it.  Two captures with two different jobs.
 *
 * The PNG encoder writes stored (uncompressed) deflate blocks, so a
 * fullscreen shot is a few MB. */
void plat_screenshot(const uint32_t *pix, int w, int h){
    SYSTEMTIME st;
    char path[96];
    int n;
    int dw, dh, dx, dy;
    uint32_t *scaled = NULL;
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
    plat_present_rect(w, h, &dw, &dh, &dx, &dy);
    if((dw != w || dh != h) && (double)dw*dh < 64e6)
        scaled = (uint32_t*)malloc((size_t)dw * (size_t)dh * sizeof(uint32_t));
    if(scaled){
        int y, x;
        for(y = 0; y < dh; y++){
            const uint32_t *src = pix + (size_t)(y * h / dh) * w;
            uint32_t *out = scaled + (size_t)y * dw;
            for(x = 0; x < dw; x++) out[x] = src[x * w / dw];
        }
    }
    if(save_png(path, scaled ? scaled : pix, scaled ? dw : w, scaled ? dh : h))
        fprintf(stderr, "[pfemu] screenshot saved: %s (%dx%d)\n",
                path, scaled ? dw : w, scaled ? dh : h);
    else fprintf(stderr, "[pfemu] screenshot failed: %s\n", path);
    free(scaled);
}


/* ------------------------------------------------------------ main loop */

/* ------------------------------------------------------------ host entry --
 * The session driver lives in src/run.c and is platform-free.  What is left
 * here is everything that only Windows can answer: the process-wide display
 * and console setup below, the window and message pump above, and the three
 * services the loop asks for by name (a modal failure message, live keyboard
 * reconciliation, and an F11 screenshot). */

void plat_early_init(void){
    /* Windows-subsystem binary: no console of its own, so double-clicking
     * shows only the UI.  When started from a console, reattach to it so
     * CLI output (-secs stats, traces) still works - but never steal a
     * redirected stderr, so `2>file` log capture keeps working. */
    { HANDLE he = GetStdHandle(STD_ERROR_HANDLE);
      if(he == NULL || he == INVALID_HANDLE_VALUE ||
         GetFileType(he) == FILE_TYPE_UNKNOWN){
          if(AttachConsole(ATTACH_PARENT_PROCESS)){
              freopen("CONOUT$", "w", stdout);
              freopen("CONOUT$", "w", stderr);
          }
      } }
    /* DPI awareness, first thing: without it the OS virtualizes the window
     * (WM_SIZE reports logical pixels) and bitmap-scales everything up, so
     * rendering goes soft on scaled displays instead of sharp.  Aware from
     * here means every pixel ever named (WM_SIZE, blits, swapchains) is a
     * physical one on both paths.  Per-monitor V2 where the OS knows it
     * (Win10 1703+), system-aware back to Vista otherwise - nothing newer
     * is linked, so Windows 7 still runs. */
    { HMODULE u = GetModuleHandleA("user32.dll");
      FARPROC (WINAPI *setv2)(void*) = u ? (FARPROC(WINAPI*)(void*))GetProcAddress(u, "SetProcessDpiAwarenessContext") : NULL;
      /* DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 = (HANDLE)-4 */
      if(setv2) setv2((void*)(INT_PTR)-4);
      else SetProcessDPIAware();
      /* Keep the opening window its physical size: it was authored as
       * 960x600 at 96 DPI. */
      { HDC sdc = GetDC(NULL);
        int dpi = sdc ? GetDeviceCaps(sdc, LOGPIXELSX) : 96;
        if(sdc) ReleaseDC(NULL, sdc);
        if(dpi < 96) dpi = 96;
        win_w = (960 * dpi + 48) / 96;
        win_h = (600 * dpi + 48) / 96;
        fprintf(stderr, "[pfemu] dpi awareness: %d (dpi %d)\n",
                IsProcessDPIAware(), dpi);
      } }
}

/* Paired with the timeBeginPeriod(1) in plat_init(). */
void plat_shutdown(void){
    timeEndPeriod(1);
}

/* run.c has already put the text on stderr; this adds the box a windowed
 * process needs.  See the from_launcher comment in src/run.c. */
void plat_fail_msg(const char *msg){
    if(from_launcher) MessageBoxA(NULL, msg, "pfemu", MB_OK|MB_ICONEXCLAMATION);
}

int plat_abspath(const char *in, char *out, size_t n){
    return GetFullPathNameA(in, (DWORD)n, out, NULL) ? 1 : 0;
}

int main(int argc, char **argv){
    plat_early_init();
    return emu_main(argc, argv);
}
