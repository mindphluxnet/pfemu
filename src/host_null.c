/* Null host: the other end of the split in src/run.c.
 *
 * Answers every plat_ entry point with the least the session driver needs and
 * nothing more - no window, no audio device, no keyboard.  A run under this
 * host is driven entirely by what is already on the command line (-replay,
 * -keys, -untilemu, -secs) and reports itself through stderr, -wav, -shot and
 * -shotevery.
 *
 * What it must NOT do is change what the guest sees.  Everything below is
 * either inert or host-side by construction:
 *
 *   - plat_pump() never ends the session; only the guest, -untilemu, -secs or
 *     the replay footer do, exactly as in a windowed run that is never
 *     touched.
 *   - plat_present() drops the frame.  vga_render() has already run in the
 *     loop and only reads guest state, so nothing is lost but the picture.
 *   - plat_kbd_reconcile() does nothing.  It exists to push live host key
 *     state into the guest, and there is no keyboard here; run.c already
 *     skips it during replay, where the guest must see only the recorded
 *     events.
 *   - plat_audio_init/push are src/sound.c's, not this file's.  waveOutOpen()
 *     below fails, so sound.c leaves hwo NULL and runs silent - a path it
 *     already had.  The -wav capture is tapped upstream of it and is
 *     unaffected, which is what lets the headless build produce the sample
 *     hash the footer carries.
 *   - plat_time() is a monotonic host clock.  It paces the loop and nothing
 *     else; the guest's clock is cpu.cycles.  Verification unthrottles the
 *     loop anyway (-speed), and whether that is truly guest-invisible is one
 *     of the things docs/VERIFY.md wants measured rather than assumed.
 */
#ifndef _WIN32

#include "compat.h"
#include <time.h>
#include "pfemu.h"


int plat_pump(void){ return 1; }
void plat_init(const char *title){ (void)title; }
void plat_save_window_pos(void){}
void plat_present(const uint32_t *pix, int w, int h){ (void)pix; (void)w; (void)h; }
void plat_set_fullscreen(int on){ (void)on; }
void plat_kbd_reconcile(void){}
void plat_screenshot(const uint32_t *pix, int w, int h){ (void)pix; (void)w; (void)h; }
void plat_early_init(void){}
void plat_shutdown(void){}
void plat_fail_msg(const char *msg){ (void)msg; }   /* run.c already did stderr */
void osd_show(const char *text){ (void)text; }
void osd_clear(void){}

/* The launcher is a Win32 dialog.  run.c only starts it on an interactive
 * run - and an interactive run is not a thing this host can have, so any
 * path that reaches here was a mistake worth reporting rather than a silent
 * exit. */
int run_launcher(void){
    fprintf(stderr, "[pfemu] headless build: no launcher."
                    " Name the session with -replay, or the install with -d.\n");
    return 0;
}

void plat_present_rect(int w, int h, int *dw, int *dh, int *dx, int *dy){
    if(dw) *dw = w; if(dh) *dh = h;
    if(dx) *dx = 0; if(dy) *dy = 0;
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
    nanosleep(&ts, NULL);
}

int plat_abspath(const char *in, char *out, size_t n){
    return GetFullPathNameA(in, (DWORD)n, out, NULL) ? 1 : 0;
}

/* --- waveOut: present so src/sound.c links, and always unavailable -------- */
UINT waveOutOpen(HWAVEOUT *h, UINT dev, const WAVEFORMATEX *fmt,
                 void *cb, void *inst, DWORD flags){
    (void)dev; (void)fmt; (void)cb; (void)inst; (void)flags;
    if(h) *h = NULL;
    return 1;   /* anything but MMSYSERR_NOERROR: sound.c then runs silent */
}
UINT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n){ (void)h;(void)hdr;(void)n; return 0; }
UINT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n){ (void)h;(void)hdr;(void)n; return 0; }
UINT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n){ (void)h;(void)hdr;(void)n; return 0; }
UINT waveOutReset(HWAVEOUT h){ (void)h; return 0; }
UINT waveOutClose(HWAVEOUT h){ (void)h; return 0; }

int main(int argc, char **argv){
    plat_early_init();
    return emu_main(argc, argv);
}

#endif /* !_WIN32 */
