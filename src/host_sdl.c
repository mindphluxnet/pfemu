/* SDL2 host: the game window, keyboard and sound on Linux.
 *
 * The third answer to the plat_ entry points, next to src/main.c (Win32) and
 * src/host_null.c (headless).  It follows main.c as closely as SDL allows,
 * because a player should get the same game on both: the same keys do the
 * same things, the picture is letterboxed the same way, and the REC/PLAY
 * badges and the OSD sit in the same corners.  Where it differs, it says so.
 *
 * Nothing here reaches the guest except through kbd_key(), exactly as in
 * main.c: the emulator is the same code on every host, and the headless
 * build has already shown that Linux and Windows agree on it byte for byte
 * (docs/VERIFY.md).  A recording made here is the same kind of file as one
 * made on Windows.
 *
 * Three pieces:
 *
 *   - The window.  An SDL renderer with one streaming texture, scaled
 *     nearest-neighbour like StretchDIBits under COLORONCOLOR.  No vsync: the
 *     loop in src/run.c paces itself, and a blocking present would pace it
 *     a second time.  The badges and the OSD are drawn with the VGA font from
 *     src/vgafont.c, in window pixels after the picture, so like on Windows
 *     they never reach the framebuffer and never show up in a capture.
 *
 *   - The keyboard.  Windows hands main.c the PC scan code in lParam; SDL
 *     hands over a USB HID code, which is also positional, so one table maps
 *     it back to scan code set 1 (with E0 for the extended keys).  The host
 *     keys are matched the way main.c matches them: by position for the
 *     keypad and function keys, by symbol for - and + on the main row,
 *     because VK_OEM_MINUS and VK_OEM_PLUS follow the layout too.
 *
 *   - The sound.  src/sound.c talks to waveOut, and src/compat.h already
 *     declares that API on POSIX so that sound.c compiles unchanged.  Here it
 *     is implemented for real on an SDL audio callback, on waveOut's own
 *     model: waveOutWrite() queues a buffer, the callback plays the queue in
 *     order and sets WHDR_DONE on each buffer it has finished.  That is all
 *     sound.c looks at, so it runs unchanged too.
 *
 * run_launcher() is here as well, and it is not a launcher (see there).
 */
#ifndef _WIN32

#include "compat.h"
#define SDL_MAIN_HANDLED          /* main() below is ours, as on every host */
#include <SDL.h>
#include <time.h>
#include <errno.h>
#include "pfemu.h"

extern void kbd_release_all(void);
extern uint8_t vga_font8x16[256*16];

/* -------------------------------------------------------------- window  */
static SDL_Window   *win;
static SDL_Renderer *ren;
static SDL_Texture  *tex;
static int tex_w, tex_h;
static int running = 1;
static int win_w = 960, win_h = 600;   /* renderer output, physical pixels */
static int fullscreen = 0;
static int winpos_x, winpos_y, winpos_have;   /* windowed spot, for fullscreen */
static double winpos_dirty = 0;               /* when the window last moved */

/* ----------------------------------------------------------------- OSD --
 * Same notice as src/main.c: host-only, wall-clock lifetime, bottom
 * centre, kept out of captures. */
static char osd_text[48] = "";
static double osd_until = 0;

void osd_show(const char *text){
    snprintf(osd_text, sizeof(osd_text), "%s", text);
    osd_until = plat_time() + 1.6;
}

void osd_clear(void){
    osd_text[0] = 0;
    osd_until = 0;
}

/* Text in the VGA font, s window pixels per font pixel.  Collected into one
 * batch of rectangles per call; an OSD line is a few hundred at most. */
static int text_w(const char *t, int s){ return (int)strlen(t) * 8 * s; }
static void draw_text(int x, int y, int s, const char *t, Uint8 r, Uint8 g, Uint8 b){
    SDL_Rect px[512];
    int n = 0, i, row, col;
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for(i = 0; t[i]; i++){
        const uint8_t *gl = vga_font8x16 + (unsigned char)t[i] * 16;
        for(row = 0; row < 16; row++)
            for(col = 0; col < 8; col++){
                if(!(gl[row] & (0x80 >> col))) continue;
                px[n].x = x + (i*8 + col) * s; px[n].y = y + row * s;
                px[n].w = s; px[n].h = s;
                if(++n == (int)(sizeof(px)/sizeof(px[0]))){
                    SDL_RenderFillRects(ren, px, n); n = 0;
                }
            }
    }
    if(n) SDL_RenderFillRects(ren, px, n);
}

static void fill(int x, int y, int w, int h, Uint8 r, Uint8 g, Uint8 b){
    SDL_Rect rc = { x, y, w, h };
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    SDL_RenderFillRect(ren, &rc);
}

/* A filled circle of diameter d, one row at a time. */
static void dot(int x, int y, int d, Uint8 r, Uint8 g, Uint8 b){
    int row;
    double rad = d / 2.0;
    SDL_SetRenderDrawColor(ren, r, g, b, 255);
    for(row = 0; row < d; row++){
        double dy = row + 0.5 - rad;
        int half = (int)(SDL_sqrt(rad*rad - dy*dy) + 0.5);
        SDL_Rect rc = { x + (int)rad - half, y + row, 2*half, 1 };
        if(half > 0) SDL_RenderFillRect(ren, &rc);
    }
}

/* Font scale: the Windows badges are 15 px Segoe UI at any window size.
 * The VGA font is 16 px at 1x; 2x on a screen tall enough to want it. */
static int ui_scale(void){ return win_h >= 1400 ? 2 : 1; }

/* REC while recording (red), PLAY while replaying (green): the same corner
 * and colours as rec_draw_dc() in src/main.c. */
static void rec_draw(void){
    const char *t;
    Uint8 dr, dg, db, ir, ig, ib;
    int s = ui_scale(), boxw, boxh, x0, y0, th;
    if(replay_is_recording()){ t = "REC"; dr=0xE0; dg=0x40; db=0x40; ir=0xFF; ig=0xE0; ib=0x40; }
    else if(replay_is_replaying()){ t = "PLAY"; dr=0x40; dg=0xC0; db=0x60; ir=0x70; ig=0xFF; ib=0x90; }
    else return;
    th = 16 * s;
    boxw = (6+10+6+8) * s + text_w(t, s);
    boxh = th + 12 * s;
    if(win_w < boxw+8 || win_h < boxh+8) return;
    x0 = win_w - boxw - 6*s;
    y0 = 6*s;
    fill(x0, y0, boxw, boxh, 0x14, 0x14, 0x14);
    dot(x0 + 6*s, y0 + boxh/2 - 5*s, 10*s, dr, dg, db);
    draw_text(x0 + (6+10+6)*s, y0 + (boxh-th)/2, s, t, ir, ig, ib);
}

static void osd_draw(void){
    int s = ui_scale(), boxw, boxh, x0, y0, th;
    if(!osd_text[0] || plat_time() >= osd_until) return;
    th = 16 * s;
    boxw = text_w(osd_text, s) + 24 * s;
    boxh = th + 12 * s;
    if(win_w < boxw+16 || win_h < boxh+16) return;
    x0 = (win_w - boxw) / 2;
    y0 = win_h - boxh - 12*s;
    fill(x0, y0, boxw, boxh, 0x14, 0x14, 0x14);
    draw_text(x0 + 12*s, y0 + (boxh-th)/2, s, osd_text, 0xFF, 0xE0, 0x40);
}

static void set_fullscreen(int on){
    if(!win || on == fullscreen) return;
    if(on){
        SDL_GetWindowPosition(win, &winpos_x, &winpos_y);
        winpos_have = 1;
    }
    if(SDL_SetWindowFullscreen(win, on ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0) == 0)
        fullscreen = on;
}

void plat_set_fullscreen(int on){ set_fullscreen(on); }

/* ------------------------------------------------------------ keyboard --
 * SDL scancode (USB HID usage, positional) -> PC scan code set 1, with
 * 0xE000 for the E0-prefixed keys, as kbd_key() takes them.  Only keys a PC
 * keyboard of the time had; anything else maps to 0 and is not passed on,
 * like a zero scan code in main.c. */
static unsigned short pc_code(SDL_Scancode sc){
    static const unsigned short map[SDL_NUM_SCANCODES] = {
        [SDL_SCANCODE_ESCAPE]=0x01,
        [SDL_SCANCODE_1]=0x02, [SDL_SCANCODE_2]=0x03, [SDL_SCANCODE_3]=0x04,
        [SDL_SCANCODE_4]=0x05, [SDL_SCANCODE_5]=0x06, [SDL_SCANCODE_6]=0x07,
        [SDL_SCANCODE_7]=0x08, [SDL_SCANCODE_8]=0x09, [SDL_SCANCODE_9]=0x0A,
        [SDL_SCANCODE_0]=0x0B, [SDL_SCANCODE_MINUS]=0x0C, [SDL_SCANCODE_EQUALS]=0x0D,
        [SDL_SCANCODE_BACKSPACE]=0x0E, [SDL_SCANCODE_TAB]=0x0F,
        [SDL_SCANCODE_Q]=0x10, [SDL_SCANCODE_W]=0x11, [SDL_SCANCODE_E]=0x12,
        [SDL_SCANCODE_R]=0x13, [SDL_SCANCODE_T]=0x14, [SDL_SCANCODE_Y]=0x15,
        [SDL_SCANCODE_U]=0x16, [SDL_SCANCODE_I]=0x17, [SDL_SCANCODE_O]=0x18,
        [SDL_SCANCODE_P]=0x19, [SDL_SCANCODE_LEFTBRACKET]=0x1A,
        [SDL_SCANCODE_RIGHTBRACKET]=0x1B, [SDL_SCANCODE_RETURN]=0x1C,
        [SDL_SCANCODE_LCTRL]=0x1D,
        [SDL_SCANCODE_A]=0x1E, [SDL_SCANCODE_S]=0x1F, [SDL_SCANCODE_D]=0x20,
        [SDL_SCANCODE_F]=0x21, [SDL_SCANCODE_G]=0x22, [SDL_SCANCODE_H]=0x23,
        [SDL_SCANCODE_J]=0x24, [SDL_SCANCODE_K]=0x25, [SDL_SCANCODE_L]=0x26,
        [SDL_SCANCODE_SEMICOLON]=0x27, [SDL_SCANCODE_APOSTROPHE]=0x28,
        [SDL_SCANCODE_GRAVE]=0x29, [SDL_SCANCODE_LSHIFT]=0x2A,
        [SDL_SCANCODE_BACKSLASH]=0x2B,
        [SDL_SCANCODE_Z]=0x2C, [SDL_SCANCODE_X]=0x2D, [SDL_SCANCODE_C]=0x2E,
        [SDL_SCANCODE_V]=0x2F, [SDL_SCANCODE_B]=0x30, [SDL_SCANCODE_N]=0x31,
        [SDL_SCANCODE_M]=0x32, [SDL_SCANCODE_COMMA]=0x33, [SDL_SCANCODE_PERIOD]=0x34,
        [SDL_SCANCODE_SLASH]=0x35, [SDL_SCANCODE_RSHIFT]=0x36,
        [SDL_SCANCODE_KP_MULTIPLY]=0x37, [SDL_SCANCODE_LALT]=0x38,
        [SDL_SCANCODE_SPACE]=0x39, [SDL_SCANCODE_CAPSLOCK]=0x3A,
        [SDL_SCANCODE_F1]=0x3B, [SDL_SCANCODE_F2]=0x3C, [SDL_SCANCODE_F3]=0x3D,
        [SDL_SCANCODE_F4]=0x3E, [SDL_SCANCODE_F5]=0x3F, [SDL_SCANCODE_F6]=0x40,
        [SDL_SCANCODE_F7]=0x41, [SDL_SCANCODE_F8]=0x42, [SDL_SCANCODE_F9]=0x43,
        [SDL_SCANCODE_F10]=0x44, [SDL_SCANCODE_NUMLOCKCLEAR]=0x45,
        [SDL_SCANCODE_SCROLLLOCK]=0x46,
        [SDL_SCANCODE_KP_7]=0x47, [SDL_SCANCODE_KP_8]=0x48, [SDL_SCANCODE_KP_9]=0x49,
        [SDL_SCANCODE_KP_MINUS]=0x4A,
        [SDL_SCANCODE_KP_4]=0x4B, [SDL_SCANCODE_KP_5]=0x4C, [SDL_SCANCODE_KP_6]=0x4D,
        [SDL_SCANCODE_KP_PLUS]=0x4E,
        [SDL_SCANCODE_KP_1]=0x4F, [SDL_SCANCODE_KP_2]=0x50, [SDL_SCANCODE_KP_3]=0x51,
        [SDL_SCANCODE_KP_0]=0x52, [SDL_SCANCODE_KP_PERIOD]=0x53,
        [SDL_SCANCODE_NONUSBACKSLASH]=0x56,   /* the < > | key left of Z */
        [SDL_SCANCODE_F11]=0x57, [SDL_SCANCODE_F12]=0x58,
        /* E0 */
        [SDL_SCANCODE_KP_ENTER]=0xE01C, [SDL_SCANCODE_RCTRL]=0xE01D,
        [SDL_SCANCODE_KP_DIVIDE]=0xE035, [SDL_SCANCODE_PRINTSCREEN]=0xE037,
        [SDL_SCANCODE_RALT]=0xE038,
        [SDL_SCANCODE_HOME]=0xE047, [SDL_SCANCODE_UP]=0xE048,
        [SDL_SCANCODE_PAGEUP]=0xE049, [SDL_SCANCODE_LEFT]=0xE04B,
        [SDL_SCANCODE_RIGHT]=0xE04D, [SDL_SCANCODE_END]=0xE04F,
        [SDL_SCANCODE_DOWN]=0xE050, [SDL_SCANCODE_PAGEDOWN]=0xE051,
        [SDL_SCANCODE_INSERT]=0xE052, [SDL_SCANCODE_DELETE]=0xE053,
        [SDL_SCANCODE_LGUI]=0xE05B, [SDL_SCANCODE_RGUI]=0xE05C,
        [SDL_SCANCODE_APPLICATION]=0xE05D,
    };
    return (sc >= 0 && sc < SDL_NUM_SCANCODES) ? map[sc] : 0;
}

/* The volume and enhancement keys, as in main.c's wndproc: - and + in 5%
 * steps (key-repeat on, holding - fades out), keypad * mutes and restores,
 * keypad / toggles the enhancement bypass.  Host-only, live on replay. */
static void vol_key(int kind){
    char msg[32];
    if(kind == '*'){
        if(audio_volume > 0){
            vol_premute = audio_volume; vol_muted = 1; audio_volume = 0;
        } else {
            audio_volume = vol_premute > 0 ? vol_premute : AUDIO_VOLUME_DEFAULT;
            vol_muted = 0;
        }
    } else {
        audio_volume += kind == '+' ? 5 : -5;
        if(audio_volume < 0) audio_volume = 0;
        if(audio_volume > 100) audio_volume = 100;
        vol_muted = 0;
        audio_volume_dirty = 1;            /* run.c writes it on exit */
    }
    if(vol_muted) snprintf(msg, sizeof(msg), "VOLUME: MUTED");
    else snprintf(msg, sizeof(msg), "VOLUME: %d%%", audio_volume);
    osd_show(msg);
    fprintf(stderr, "[snd] volume %d%%\n", audio_volume);
}

static void key_down(const SDL_KeyboardEvent *e){
    SDL_Scancode sc = e->keysym.scancode;
    SDL_Keycode sym = e->keysym.sym;
    int alt = (e->keysym.mod & KMOD_ALT) != 0;
    int rep = e->repeat != 0;
    int pc = pc_code(sc);
    /* Scroll Lock quits: F12 belongs to the game. */
    if(sc == SDL_SCANCODE_SCROLLLOCK){ running = 0; return; }
    /* Alt+Enter toggles fullscreen, not on replay. */
    if(alt && (sym == SDLK_RETURN || sym == SDLK_KP_ENTER)){
        if(!rep && !replay_is_replaying()) set_fullscreen(!fullscreen);
        return;
    }
    /* The rest are WM_KEYDOWN in main.c, which Windows does not send while
     * Alt is held; with Alt they go to the game, as there. */
    if(!alt){
        if(sc == SDL_SCANCODE_F11){
            if(!rep && !replay_is_replaying()) screenshot_pending = 1;
            return;
        }
        if(sc == SDL_SCANCODE_KP_MULTIPLY){ if(!rep) vol_key('*'); return; }
        if(sc == SDL_SCANCODE_KP_PLUS || sym == SDLK_PLUS || sym == SDLK_EQUALS){
            vol_key('+'); return;
        }
        if(sc == SDL_SCANCODE_KP_MINUS || sym == SDLK_MINUS){ vol_key('-'); return; }
        if(sc == SDL_SCANCODE_KP_DIVIDE){
            if(!rep){
                char msg[32];
                audio_enh_bypass = !audio_enh_bypass;
                snprintf(msg, sizeof(msg), "ENHANCEMENT: %s",
                         audio_enh_bypass ? "OFF" : "ON");
                osd_show(msg);
                fprintf(stderr, "[snd] enhancement bypass %s\n",
                        audio_enh_bypass ? "on" : "off");
            }
            return;
        }
        if(sc == SDL_SCANCODE_F6 || sc == SDL_SCANCODE_F8){
            if(!rep){
                if(sc == SDL_SCANCODE_F6) snap_save_pending = 1;
                else snap_load_pending = 1;
            }
            return;
        }
    }
    /* Key-repeat makes go through, like Windows' typematic WM_KEYDOWNs. */
    if(pc && !replay_is_replaying()) kbd_key(pc, 1);
}

/* See plat_kbd_reconcile() in src/main.c for why this exists.  SDL's key
 * state follows the same events that reach kbd_key(), so this rarely finds
 * anything here; it stays for the same guarantee. */
void plat_kbd_reconcile(void){
    static const struct { int sc, ext; SDL_Scancode s; } keys[] = {
        {0x2A,0,SDL_SCANCODE_LSHIFT}, {0x36,0,SDL_SCANCODE_RSHIFT},
        {0x1D,0,SDL_SCANCODE_LCTRL},  {0x1D,1,SDL_SCANCODE_RCTRL},
        {0x38,0,SDL_SCANCODE_LALT},   {0x38,1,SDL_SCANCODE_RALT},
    };
    const Uint8 *st;
    size_t i;
    if(!win) return;
    st = SDL_GetKeyboardState(NULL);
    for(i=0;i<sizeof(keys)/sizeof(keys[0]);i++){
        unsigned idx = (unsigned)keys[i].sc | (keys[i].ext ? 0x80u : 0u);
        if(kbd_held_get(idx) && !st[keys[i].s]){
            trc("[kbd] reconcile: %s%02X marked held but not physically down - forcing break\n",
                keys[i].ext?"E0 ":"", keys[i].sc);
            kbd_key(keys[i].sc | (keys[i].ext?0xE000:0), 0);
        }
    }
}

/* ------------------------------------------------------------- lifecycle */
/* Wayland in a Wayland session.  SDL2 picks X11 first even there, which
 * means XWayland, and on X11 SDL opens a GLX context even for the software
 * renderer.  On a desktop whose GLX is broken - measured on one: Ubuntu,
 * GNOME on Wayland, a GTX 960 that Mesa had no driver for, glxinfo failing
 * too - Xlib then ends the process on BadValue from X_GLXCreateContext
 * before SDL can report anything.  Native Wayland needs no GLX and played
 * cleanly on the same machine.  Only a default: SDL_VIDEODRIVER set by the
 * user still wins, and plat_init() falls back to X11 if Wayland fails.
 * Set through the environment rather than a hint so the exec'd game
 * inherits it, and so SDL versions older than SDL_HINT_VIDEODRIVER
 * (2.0.22) read it too. */
static int chose_wayland = 0;

void plat_early_init(void){
    const char *st = getenv("XDG_SESSION_TYPE");
    /* Nearest-neighbour scaling, as StretchDIBits under COLORONCOLOR. */
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "0");
    /* Leave the compositor on: this is a windowed game first. */
    SDL_SetHint(SDL_HINT_VIDEO_X11_NET_WM_BYPASS_COMPOSITOR, "0");
    if(!getenv("SDL_VIDEODRIVER") && st && !strcmp(st, "wayland") &&
       getenv("WAYLAND_DISPLAY")){
        setenv("SDL_VIDEODRIVER", "wayland", 1);
        chose_wayland = 1;
    }
}

/* 1 when (x, y) is inside some display's usable area. */
static int point_visible(int x, int y){
    int i, n = SDL_GetNumVideoDisplays();
    for(i = 0; i < n; i++){
        SDL_Rect r;
        if(SDL_GetDisplayUsableBounds(i, &r) == 0 &&
           x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return 1;
    }
    return 0;
}

/* No window means no game: end the session rather than run it unseen.
 * The headless build is the one for runs without a display. */
static void no_window(const char *what){
    char msg[300];
    snprintf(msg, sizeof(msg), "No %s: %s\n(pfemu-headless runs without a display.)",
             what, SDL_GetError());
    fprintf(stderr, "[sdl] %s\n", msg);
    plat_fail_msg(msg);
    running = 0;
}

void plat_init(const char *title){
    int x = SDL_WINDOWPOS_CENTERED, y = SDL_WINDOWPOS_CENTERED, sx, sy;
    if(SDL_InitSubSystem(SDL_INIT_VIDEO) != 0 && chose_wayland){
        /* Our choice, not the user's: try what SDL would have taken. */
        fprintf(stderr, "[sdl] wayland: %s; trying x11\n", SDL_GetError());
        setenv("SDL_VIDEODRIVER", "x11", 1);
        chose_wayland = 0;
    }
    if(!SDL_WasInit(SDL_INIT_VIDEO) && SDL_InitSubSystem(SDL_INIT_VIDEO) != 0){
        no_window("video"); return;
    }
    /* The saved spot, unless it is on no display any more (a monitor
     * unplugged, a resolution change): then centred, like main.c. */
    if(last_read_winpos(&sx, &sy) && point_visible(sx + 960/2, sy + 16)){
        x = sx; y = sy;
    }
    win = SDL_CreateWindow(title, x, y, 960, 600,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if(!win){ no_window("window"); return; }
    SDL_SetWindowMinimumSize(win, 320, 200);
    /* The game reads keys, never text; without this SDL may bring up an
     * input method over the playfield. */
    SDL_StopTextInput();
    ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    if(!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    if(!ren){ no_window("renderer"); return; }
    { SDL_RendererInfo ri;
      if(SDL_GetRendererInfo(ren, &ri) == 0)
          fprintf(stderr, "[sdl] video %s, renderer %s%s\n",
                  SDL_GetCurrentVideoDriver(), ri.name,
                  (ri.flags & SDL_RENDERER_PRESENTVSYNC) ? " (vsync)" : ""); }
    SDL_GetRendererOutputSize(ren, &win_w, &win_h);
    { int px, py, ww, wh;
      SDL_GetWindowPosition(win, &px, &py);
      SDL_GetWindowSize(win, &ww, &wh);
      fprintf(stderr, "[sdl] window at %d,%d size %dx%d, output %dx%d%s,"
                      " %d display(s)\n", px, py, ww, wh, win_w, win_h,
              (x == SDL_WINDOWPOS_CENTERED) ? " (centred)" : " (saved spot)",
              SDL_GetNumVideoDisplays()); }
}

/* Presents: how many, and what SDL_RenderPresent costs.  A black or absent
 * picture with presents counted means the frames reach SDL and stop
 * there. */
static unsigned long pres_n;
static double pres_total, pres_max;

/* Persist the windowed position for the next run: the same keys as on
 * Windows, in a file of its own (pfemu-winpos-sdl.cfg; src/cfg.c says why).
 * Fullscreen keeps the spot from before it.  Under Wayland a window has no position to read, and SDL
 * reports 0,0; restoring that is harmless, because Wayland places the
 * window itself anyway. */
void plat_save_window_pos(void){
    int x, y;
    if(!win) return;
    if(fullscreen){
        if(!winpos_have) return;
        x = winpos_x; y = winpos_y;
    } else SDL_GetWindowPosition(win, &x, &y);
    last_save_winpos(x, y);
    winpos_dirty = 0;
}

int plat_pump(void){
    SDL_Event e;
    if(!win) return running;
    while(SDL_PollEvent(&e)){
        switch(e.type){
        case SDL_QUIT:
            plat_save_window_pos();
            running = 0;
            break;
        case SDL_WINDOWEVENT:
            switch(e.window.event){
            case SDL_WINDOWEVENT_SIZE_CHANGED:
                if(ren) SDL_GetRendererOutputSize(ren, &win_w, &win_h);
                break;
            /* Focus-loss releases are host leakage (docs/REPLAY.md section
             * 2.4): suppressed on replay, where no live keyboard reaches
             * the guest. */
            case SDL_WINDOWEVENT_FOCUS_LOST:
                if(!replay_is_replaying()) kbd_release_all();
                break;
            /* Windows saves when a drag ends (WM_EXITSIZEMOVE).  SDL has no
             * such event, only a stream of moves, so save once they stop. */
            case SDL_WINDOWEVENT_MOVED:
                if(!fullscreen) winpos_dirty = plat_time();
                break;
            }
            break;
        case SDL_KEYDOWN:
            key_down(&e.key);
            break;
        case SDL_KEYUP: {
            int pc = pc_code(e.key.keysym.scancode);
            if(pc && !replay_is_replaying()) kbd_key(pc, 0);
            break; }
        }
    }
    if(winpos_dirty > 0 && plat_time() - winpos_dirty > 0.5)
        plat_save_window_pos();
    return running;
}

/* Where the picture lands inside the window: aspect-corrected and centred,
 * with black bars on whichever axis is left over.  The same arithmetic as
 * src/main.c, so F11 screenshots come out the same size on both. */
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
    SDL_Rect d;
    if(!ren) return;
    if(!tex || tex_w != w || tex_h != h){
        if(tex) SDL_DestroyTexture(tex);
        /* 0x00RRGGBB per pixel: what main.c hands GDI as 32-bit BI_RGB. */
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGB888,
                                SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
        if(!tex) return;
    }
    SDL_UpdateTexture(tex, NULL, pix, w * 4);
    plat_present_rect(w, h, &dw, &dh, &dx, &dy);
    SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
    SDL_RenderClear(ren);
    d.x = dx; d.y = dy; d.w = dw; d.h = dh;
    SDL_RenderCopy(ren, tex, NULL, &d);
    /* Screen-space overlays go last, in window pixels. */
    osd_draw();
    rec_draw();
    {   double t = plat_time(), dt;
        SDL_RenderPresent(ren);
        dt = plat_time() - t;
        if(!pres_n)
            fprintf(stderr, "[sdl] first frame %dx%d into %dx%d at %d,%d\n",
                    w, h, dw, dh, dx, dy);
        pres_n++;
        pres_total += dt;
        if(dt > pres_max) pres_max = dt;
    }
}

double plat_time(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

void plat_sleep_ms(int ms){
    struct timespec ts;
    if(ms <= 0) return;
    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while(nanosleep(&ts, &ts) != 0 && errno == EINTR) {}
}

/* F11, as in src/main.c: saved at the size the picture has on screen,
 * nearest-neighbour from the framebuffer, black bars left out. */
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
            snprintf(path, sizeof(path), "screenshots/pfemu_%04d%02d%02d_%02d%02d%02d.png",
                     st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        else
            snprintf(path, sizeof(path), "screenshots/pfemu_%04d%02d%02d_%02d%02d%02d_%d.png",
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

void plat_shutdown(void){
    if(win){
        Uint32 f = SDL_GetWindowFlags(win);
        fprintf(stderr, "[sdl] video: %lu presents, SDL_RenderPresent mean %.2f ms"
                        " max %.1f ms; window%s%s%s%s\n",
                pres_n, pres_n ? pres_total / pres_n * 1000.0 : 0.0, pres_max * 1000.0,
                (f & SDL_WINDOW_SHOWN) ? " shown" : "",
                (f & SDL_WINDOW_HIDDEN) ? " hidden" : "",
                (f & SDL_WINDOW_MINIMIZED) ? " minimized" : "",
                (f & SDL_WINDOW_INPUT_FOCUS) ? " focused" : " unfocused");
    }
    if(tex){ SDL_DestroyTexture(tex); tex = NULL; }
    if(ren){ SDL_DestroyRenderer(ren); ren = NULL; }
    if(win){ SDL_DestroyWindow(win); win = NULL; }
    SDL_Quit();
}

/* run.c has already put the text on stderr; this adds the box a desktop
 * start needs, which on Linux has no terminal to read it in. */
void plat_fail_msg(const char *msg){
    if(from_launcher)
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "pfemu", msg, win);
}

int plat_abspath(const char *in, char *out, size_t n){
    return GetFullPathNameA(in, (DWORD)n, out, NULL) ? 1 : 0;
}

/* ---------------------------------------------------------------- sound --
 * waveOut on an SDL callback.  The queue holds header pointers in the order
 * they were written; the callback copies from the head and marks each
 * header WHDR_DONE as it finishes it.  src/sound.c reuses a header only once
 * it is done, which is the same contract it has with Windows.
 *
 * One thing waveOut does that a callback does not: when its queue runs dry
 * it stops, and the next buffer written starts it again, so the lead the
 * writer had is back.  An SDL device keeps running and asks every 21 ms
 * whatever is there.  Fed in real time with no lead at all, it came up
 * empty on 143 of 2227 calls in a 47 s run under WSLg - 2.8 s of silence cut
 * into the sound in slivers, heard as crackling.  So the callback plays
 * silence until PRIME_MS of sound is queued, at the start and again after
 * any underrun, and only then plays the queue.  One gap and a fresh lead,
 * instead of a crackle on every late frame.  The 64 ms are two device
 * buffers plus one of sound.c's pushes, well inside what its twelve
 * buffers can hold. */
#define WQ_MAX 64
#define PRIME_MS 64
static SDL_AudioDeviceID adev;
static WAVEHDR *wq[WQ_MAX];
static int wq_head, wq_n;
static DWORD wq_off;              /* bytes of wq[wq_head] already played */
static DWORD wq_bytes;            /* bytes queued and not yet played */
static DWORD prime_bytes;         /* the lead to build before playing */
static int primed;

/* What the device actually does with the queue, reported at close: how
 * regularly SDL asks, how much it takes per second of wall time, and how
 * often the queue was empty when it asked.  Written by the callback only,
 * read after the device is closed. */
static struct {
    unsigned long calls, starved_calls, primes;
    double bytes, silence_bytes, wait_bytes;
    double t_first, t_last, gap_max;
    int len_min, len_max;
} ast;
static int adev_frame = 4;        /* bytes per frame in the opened format */
static int adev_freq = 48000;

static void SDLCALL audio_cb(void *user, Uint8 *out, int len){
    double now = plat_time();
    (void)user;
    if(ast.calls){
        if(now - ast.t_last > ast.gap_max) ast.gap_max = now - ast.t_last;
    } else ast.t_first = now;
    ast.t_last = now;
    ast.calls++;
    ast.bytes += len;
    if(!ast.len_min || len < ast.len_min) ast.len_min = len;
    if(len > ast.len_max) ast.len_max = len;
    if(!primed){
        /* Eight headers queued counts as a lead too: sound.c has twelve, and
         * a wait that could only end in bytes would never end if they were
         * all short ones. */
        if(wq_bytes < prime_bytes && wq_n < 8){     /* still building the lead */
            ast.wait_bytes += len;
            memset(out, 0, (size_t)len);
            return;
        }
        primed = 1;
        ast.primes++;
    }
    while(len > 0){
        WAVEHDR *h;
        DWORD left;
        int k;
        if(!wq_n){                                          /* underrun */
            ast.starved_calls++;
            ast.silence_bytes += len;
            memset(out, 0, (size_t)len);
            primed = 0;
            return;
        }
        h = wq[wq_head];
        left = h->dwBufferLength - wq_off;
        k = left < (DWORD)len ? (int)left : len;
        memcpy(out, h->lpData + wq_off, (size_t)k);
        out += k; len -= k; wq_off += (DWORD)k;
        wq_bytes -= (DWORD)k;
        if(wq_off >= h->dwBufferLength){
            h->dwFlags |= WHDR_DONE;
            wq_head = (wq_head + 1) % WQ_MAX;
            wq_n--;
            wq_off = 0;
        }
    }
}

UINT waveOutOpen(HWAVEOUT *h, UINT dev, const WAVEFORMATEX *fmt,
                 void *cb, void *inst, DWORD flags){
    SDL_AudioSpec want, have;
    (void)dev; (void)cb; (void)inst; (void)flags;
    if(h) *h = NULL;
    if(!fmt || fmt->wBitsPerSample != 16) return 1;
    if(!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0){
        fprintf(stderr, "[sdl] no audio: %s\n", SDL_GetError());
        return 1;
    }
    SDL_zero(want);
    want.freq = (int)fmt->nSamplesPerSec;
    want.format = AUDIO_S16SYS;
    want.channels = (Uint8)fmt->nChannels;
    want.samples = 1024;
    want.callback = audio_cb;
    /* No allowed changes: SDL converts to whatever the device wants, and
     * sound.c keeps producing exactly what it asked for. */
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if(!adev){
        fprintf(stderr, "[sdl] no audio device: %s\n", SDL_GetError());
        return 1;
    }
    wq_head = wq_n = 0;
    wq_off = 0;
    wq_bytes = 0;
    primed = 0;
    prime_bytes = (DWORD)((long)want.freq * PRIME_MS / 1000) * (DWORD)(2 * want.channels);
    SDL_PauseAudioDevice(adev, 0);
    adev_frame = 2 * want.channels;
    adev_freq = want.freq;
    memset(&ast, 0, sizeof(ast));
    fprintf(stderr, "[sdl] audio %s: asked %d Hz %d ch %d frames, device has"
                    " %d Hz %d ch %d frames (format %04X)\n",
            SDL_GetCurrentAudioDriver(), want.freq, want.channels, want.samples,
            have.freq, have.channels, have.samples, (unsigned)have.format);
    if(h) *h = (HWAVEOUT)(intptr_t)adev;
    return MMSYSERR_NOERROR;
}

UINT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n){ (void)h;(void)hdr;(void)n; return 0; }
UINT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n){ (void)h;(void)hdr;(void)n; return 0; }

UINT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n){
    (void)h; (void)n;
    if(!adev || !hdr) return 1;
    SDL_LockAudioDevice(adev);
    if(wq_n < WQ_MAX){
        hdr->dwFlags &= ~(DWORD)WHDR_DONE;
        wq[(wq_head + wq_n) % WQ_MAX] = hdr;
        wq_n++;
        wq_bytes += hdr->dwBufferLength;
    } else hdr->dwFlags |= WHDR_DONE;     /* cannot happen with NBUF 12 */
    SDL_UnlockAudioDevice(adev);
    return 0;
}

UINT waveOutReset(HWAVEOUT h){
    (void)h;
    if(!adev) return 0;
    SDL_LockAudioDevice(adev);
    while(wq_n){
        wq[wq_head]->dwFlags |= WHDR_DONE;
        wq_head = (wq_head + 1) % WQ_MAX;
        wq_n--;
    }
    wq_off = 0;
    wq_bytes = 0;
    primed = 0;
    SDL_UnlockAudioDevice(adev);
    return 0;
}

UINT waveOutClose(HWAVEOUT h){
    (void)h;
    if(!adev) return 0;
    SDL_CloseAudioDevice(adev);
    adev = 0;
    /* The rate is what SDL took from the queue per second of wall time, in
     * the format sound.c writes: 48000 when the device keeps pace.  A gap
     * far above one device buffer means SDL asks in bursts. */
    if(ast.calls > 1){
        double span = ast.t_last - ast.t_first;
        fprintf(stderr, "[sdl] audio: %lu callbacks over %.2fs, %.0f frames/s taken"
                        " (%d expected), %d-%d bytes per call, max gap %.1f ms,"
                        " %lu underruns (%.0f ms cut), lead of %d ms built %lu times"
                        " (%.0f ms waiting for it)\n",
                ast.calls, span, span > 0 ? ast.bytes / adev_frame / span : 0.0,
                adev_freq, ast.len_min, ast.len_max, ast.gap_max * 1000.0,
                ast.starved_calls, ast.silence_bytes / adev_frame * 1000.0 / adev_freq,
                PRIME_MS, ast.primes, ast.wait_bytes / adev_frame * 1000.0 / adev_freq);
    }
    return 0;
}

/* ------------------------------------------------------------- starter --
 * There is no launcher on Linux yet.  What the Windows launcher does before
 * a game besides showing a dialog is done here, without one, and then this
 * process becomes the game, the way the launcher's child does:
 *
 *   1. offer the GOG import, once (src/gog.c; the same question);
 *   2. pick the installation: the one launched last (pfemu-last.cfg), else
 *      a copy just imported, else the first that can run;
 *   3. make sure the game has a SOUND.CFG.  The Windows launcher writes one
 *      on every Launch; without one the game stops 1.3 s in.  A fresh
 *      install gets SoundBlaster on - the launcher's checkbox starts off
 *      there, but here there is no checkbox to tick;
 *   4. exec this program again with -nolauncher -launched -d <dir>, plus
 *      -fullscreen and -table from the install's pfemu.cfg, which is
 *      exactly the command line the launcher's spawn_game() builds for a
 *      Play session.  A fresh process per session, for the same reason as
 *      there: nothing from before can reach a recording.
 *
 * Settings are the install's PFEMU-STATE/pfemu.cfg, as the launcher left
 * them or as edited by hand; the game reads them the same way on both. */
static int ask_yes_no(const char *title, const char *msg){
    const SDL_MessageBoxButtonData buttons[] = {
        { SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "No" },
        { SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Yes" },
    };
    SDL_MessageBoxData d;
    int hit = -1;
    SDL_zero(d);
    d.flags = SDL_MESSAGEBOX_INFORMATION;
    d.title = title;
    d.message = msg;
    d.numbuttons = 2;
    d.buttons = buttons;
    if(SDL_ShowMessageBox(&d, &hit) != 0) return -1;   /* could not ask */
    return hit == 1;
}

static int gog_offer(const RelResult *inst, int n){
    char image[1024], msg[1400], err[800];
    int yes;
    if(!gog_candidate(inst, n, image, sizeof(image))) return 0;
    gog_offer_text(msg, sizeof(msg), image);
    yes = ask_yes_no("pfemu - GOG version found", msg);
    if(yes < 0){
        /* No display to ask on: not a No, so ask again next time. */
        fprintf(stderr, "[gog] %s is installed; not imported (no dialog: %s)\n",
                image, SDL_GetError());
        return 0;
    }
    if(!yes){ gog_decline(image); return 0; }
    if(cdimage_import(image, GOG_DIR, err, sizeof(err)) != 0){
        snprintf(msg, sizeof(msg), "The GOG version could not be imported:\n\n%s", err);
        fprintf(stderr, "[gog] %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "pfemu - GOG import", msg, NULL);
        return 0;
    }
    fprintf(stderr, "[gog] imported %s into %s: %s\n", image, GOG_DIR, err);
    return 1;
}

int run_launcher(void){
    static RelResult inst[8];
    char exe[1024], table[8], cwd[1024], msg[1600];
    const char *argv[12];
    int n, i, sel = -1, argc = 0, imported, last;
    PfCfg c;
    n = release_scan(inst, 8);
    imported = gog_offer(inst, n);
    if(imported) n = release_scan(inst, 8);
    for(i = 0; i < n; i++)
        if(release_runnable(&inst[i])){ sel = i; break; }
    last = last_pick(inst, n);
    if(last >= 0 && release_runnable(&inst[last])) sel = last;
    if(imported)
        for(i = 0; i < n; i++)
            if(!_stricmp(inst[i].dir, GOG_DIR) && release_runnable(&inst[i])){ sel = i; break; }
    if(sel < 0){
        if(!getcwd(cwd, sizeof(cwd))) snprintf(cwd, sizeof(cwd), ".");
        snprintf(msg, sizeof(msg),
                 "No Pinball Fantasies installation found in\n\n    %s\n\n"
                 "Put one release's files in a folder there (GAME, for example), "
                 "or install the GOG.com version and start pfemu again.\n\n"
                 "An image of the Deluxe CD can be imported with\n"
                 "    pfemu -import <image> <folder>",
                 cwd);
        fprintf(stderr, "[pfemu] %s\n", msg);
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, "pfemu", msg, NULL);
        return 1;
    }
    if(!sound_cfg_exists(inst[sel].dir)){
        cfg_read(inst[sel].dir, &c);
        write_sound_cfg(inst[sel].dir, 1, c.quality);
        fprintf(stderr, "[pfemu] %s had no SOUND.CFG; wrote SoundBlaster, quality %d\n",
                inst[sel].dir, c.quality);
    }
    cfg_read(inst[sel].dir, &c);
    last_save(&inst[sel]);
    if(!GetModuleFileNameA(NULL, exe, (DWORD)sizeof(exe))){
        fprintf(stderr, "[pfemu] cannot find this program's own path\n");
        return 1;
    }
    argv[argc++] = exe;
    argv[argc++] = "-nolauncher";
    argv[argc++] = "-launched";
    argv[argc++] = "-d";
    argv[argc++] = inst[sel].dir;
    if(c.fullscreen) argv[argc++] = "-fullscreen";
    if(c.start_table >= 1 && c.start_table <= 4){
        snprintf(table, sizeof(table), "%d", c.start_table);
        argv[argc++] = "-table";
        argv[argc++] = table;
    }
    argv[argc] = NULL;
    fprintf(stderr, "[pfemu] starting %s (%s)\n", inst[sel].dir,
            inst[sel].rel ? inst[sel].rel->id : "?");
    fflush(stderr);
    execv(exe, (char *const *)argv);
    fprintf(stderr, "[pfemu] cannot start %s: %s\n", exe, strerror(errno));
    return 1;
}

int main(int argc, char **argv){
    plat_early_init();
    return emu_main(argc, argv);
}

#endif /* !_WIN32 */
