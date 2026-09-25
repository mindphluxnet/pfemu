/* -video: a replay (or any session) as a video stream, for an encoder.
 *
 * pfemu writes raw frames and a WAV; it does not encode.  The frames go to a
 * file that is meant to be a FIFO with ffmpeg on the other end (see
 * tools/render-video.sh), so pfemu stays free of codec libraries and the
 * encoder is whatever the machine running it has.
 *
 * Everything here observes the run and never alters it, the rule -shotevery
 * learned the hard way (REPLAY.md, Validation): no batch is shortened or
 * moved, nothing the guest can read is touched, and vga_render() only reads
 * guest state.  A replay with -video gives the same verdict, capture hash and
 * wav hash as one without.  And since every decision below is taken on the
 * emulated clock, two runs of one replay write byte-identical streams.
 *
 * WHEN a frame is sampled is the whole difficulty.  The engine has no sprite
 * double buffer, and a sample taken while PUTTHEBALL has the ball erased
 * shows a table without a ball (run.c, the present gate).  So a frame is
 * taken once per guest frame, in the same quiet window the window's present
 * path aims for (present_phase_in()), and never at the wall-clock moments
 * that path is also gated on.
 *
 * The OUTPUT is a constant-rate stream, because an encoder and an audio track
 * want one.  Its rate is the table's own CRT rate, 25.175 MHz / (800 x 527) =
 * 59.713 Hz, so on a table every guest frame is written exactly once.  Other
 * screens run at other rates (the menu's 480-line timing, 70 Hz text), and
 * there the stream repeats or skips the odd frame - counted in the report.
 * Output frame k is written at emulated time (k + 1/2) periods, after the
 * guest frame it shows was sampled, so the picture trails the sound by a
 * fraction of a frame.
 *
 * The sound is NOT -wav.  -wav holds the samples the card played, back to
 * back, with no silence for the time it was not playing (between program
 * loads, for one) and no record of a rate change: right for a hash, useless
 * as a soundtrack.  This one is placed on the emulated clock, gaps filled
 * with silence, resampled to 48 kHz, and padded to the video's length.
 *
 * A PAUSE is cut short.  While the table is paused the picture does not
 * change and the card plays nothing (measured: one frame hash and no samples
 * for all 84 s of a pause), and the dot matrix says GAME PAUSED.  So the
 * first PAUSE_HOLD seconds of a pause stay in the video, enough to read
 * that, and the rest is left out of picture and sound alike, by moving
 * t_origin, which both hang on.  The signal is the game's own PAUSEFLAG
 * (fantasies_paused()), a read of guest RAM, taken on the emulated clock
 * like everything else here.  This is a deliberate edit of the video, not
 * of the run: the verdict and -wav are unchanged, and each cut is reported
 * on a line of its own, so whoever publishes the video can say so.
 * -videokeeppause keeps pauses whole. */
#include "pfemu.h"
#ifndef _WIN32
#include <signal.h>
#endif

#define VID_PER   (421600.0 / 25175000.0)   /* 800 x 527 dots at 25.175 MHz */
#define AUD_HZ    48000
#define NAT_MAX   (1024 * 768)              /* run.c's framebuffer */
#define PAUSE_HOLD 2.0                      /* seconds of a pause that stay */

int video_on = 0;
static const char *vid_path, *aud_path;
static int vid_scale = 2, vid_w, vid_h;
static FILE *vid_fp, *aud_fp;
static int vid_failed = 0;

static uint32_t *nat;                /* the latest guest frame, native size */
static int nat_w, nat_h;
static uint32_t *out;                /* the same, scaled and letterboxed */
static int out_dirty = 1;
static int *xmap, *ymap;
static int map_w = -1, map_h = -1, map_dx, map_dy, map_dw, map_dh;

static int started = 0;
static double t_origin;              /* emulated time of output frame 0 */
static unsigned long long cap_idx;   /* guest frame index of the latest sample */
static int have_cap = 0;
static unsigned long long k_out;     /* output frames written */
static unsigned long n_cap, n_late, n_dup, n_drop, cap_since_emit;
static double t_wall;                /* host seconds spent here, writes included */

static unsigned long long a_out;     /* audio samples written */
static double a_pos = -1.0;          /* resampler position in input samples */
static int16_t a_prev;
static unsigned long a_gaps;

static int keep_pause = 0;           /* -videokeeppause */
static double pause_t = -1.0;        /* emulated time the pause began, or -1 */
static double pause_cut;             /* seconds of this pause left out so far */
static double pause_at;              /* where in the video the cut is */
static unsigned long n_pause;        /* pauses cut */
static double cut_total;

void video_arm(const char *frames, const char *wav){ vid_path = frames; aud_path = wav; }
void video_set_scale(int n){ vid_scale = n; }
void video_keep_pause(void){ keep_pause = 1; }
int  video_failed(void){ return vid_failed; }

static void put_le32(uint8_t *p, uint32_t v){
    p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24);
}

static void wav_header(FILE *f, uint32_t samples){
    uint8_t h[44];
    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4); put_le32(h+4, 36 + samples*2);
    memcpy(h+8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;           /* PCM, mono */
    put_le32(h+24, AUD_HZ); put_le32(h+28, AUD_HZ*2);
    h[32] = 2; h[34] = 16;
    memcpy(h+36, "data", 4); put_le32(h+40, samples*2);
    fwrite(h, 1, 44, f);
}

static void fail(const char *what){
    if(vid_failed) return;
    vid_failed = 1;
    fprintf(stderr, "[video] %s after %llu frames; the run goes on without"
                    " video\n", what, k_out);
}

int video_open(void){
    if(!vid_path) return 0;
    if(vid_scale < 1 || vid_scale > 4){
        fprintf(stderr, "[video] -videoscale takes 1-4\n");
        return -1;
    }
    vid_w = 320 * vid_scale; vid_h = 240 * vid_scale;
    nat  = (uint32_t*)calloc(NAT_MAX, sizeof(uint32_t));
    out  = (uint32_t*)calloc((size_t)vid_w * vid_h, sizeof(uint32_t));
    xmap = (int*)malloc(sizeof(int) * (size_t)vid_w);
    ymap = (int*)malloc(sizeof(int) * (size_t)vid_h);
    if(!nat || !out || !xmap || !ymap){
        fprintf(stderr, "[video] out of memory\n");
        return -1;
    }
#ifndef _WIN32
    /* An encoder that dies must not take the verdict down with it: the
     * write fails instead, and the run carries on. */
    signal(SIGPIPE, SIG_IGN);
#endif
    /* Opening a FIFO blocks until the encoder opens the other end. */
    vid_fp = fopen(vid_path, "wb");
    if(!vid_fp){
        fprintf(stderr, "[video] cannot open '%s'\n", vid_path);
        return -1;
    }
    setvbuf(vid_fp, NULL, _IOFBF, 1 << 20);
    if(aud_path){
        aud_fp = fopen(aud_path, "wb");
        if(!aud_fp){
            fprintf(stderr, "[video] cannot open '%s'\n", aud_path);
            return -1;
        }
        wav_header(aud_fp, 0);                  /* sizes patched at close */
    }
    video_on = 1;
    fprintf(stderr, "[video] %dx%d bgr0 at 25175000/421600 fps to '%s'%s%s\n",
            vid_w, vid_h, vid_path, aud_path ? ", sound to " : "",
            aud_path ? aud_path : "");
    return 0;
}

/* Where a w x h picture lands in the output: the arithmetic of
 * plat_present_rect(), so the video is framed the way the window is -
 * 320x200 stretched to 4:3, everything else at its own shape, centred. */
static void build_maps(int w, int h){
    double ar = (double)w / (double)h * (w==320 && h==200 ? 1.2 : 1.0);
    int x, y;
    map_dw = vid_w; map_dh = vid_h; map_dx = 0; map_dy = 0;
    if((double)vid_w / vid_h > ar){ map_dw = (int)(vid_h*ar); map_dx = (vid_w-map_dw)/2; }
    else { map_dh = (int)(vid_w/ar); map_dy = (vid_h-map_dh)/2; }
    if(map_dw < 1) map_dw = 1;
    if(map_dh < 1) map_dh = 1;
    for(x = 0; x < map_dw; x++) xmap[x] = x * w / map_dw;
    for(y = 0; y < map_dh; y++) ymap[y] = y * h / map_dh;
    map_w = w; map_h = h;
    memset(out, 0, sizeof(uint32_t) * (size_t)vid_w * vid_h);   /* the bars */
}

static void scale_frame(void){
    int x, y;
    if(nat_w != map_w || nat_h != map_h) build_maps(nat_w, nat_h);
    for(y = 0; y < map_dh; y++){
        const uint32_t *src = nat + (size_t)ymap[y] * nat_w;
        uint32_t *dst = out + (size_t)(map_dy + y) * vid_w + map_dx;
        for(x = 0; x < map_dw; x++) dst[x] = src[xmap[x]];
    }
}

static void emit(void){
    if(out_dirty){ if(have_cap) scale_frame(); out_dirty = 0; }
    if(cap_since_emit == 0 && k_out > 0) n_dup++;
    if(cap_since_emit > 1) n_drop += cap_since_emit - 1;
    cap_since_emit = 0;
    /* 0x00RRGGBB words are B,G,R,0 in memory on every host pfemu builds
     * for, which is ffmpeg's bgr0 */
    if(!vid_failed && fwrite(out, sizeof(uint32_t), (size_t)vid_w * vid_h, vid_fp)
                      != (size_t)vid_w * vid_h)
        fail("the frame pipe closed");
    k_out++;
}

/* A pause is over, or the run is: report its cut. */
static void pause_end(void){
    if(pause_cut > 0.0){
        n_pause++; cut_total += pause_cut;
        fprintf(stderr, "[video] pause cut: %.3f s at %.3f s of the video"
                        " (%.3f s of the replay)\n",
                pause_cut, pause_at, pause_t + PAUSE_HOLD);
    }
    pause_t = -1.0; pause_cut = 0.0;
}

/* Past PAUSE_HOLD into a pause, every emulated second is left out: t_origin
 * moves with the clock, so no frame falls due and no sound is placed. */
static void pause_track(double now){
    double want;
    if(keep_pause) return;
    if(fantasies_paused() != 1){
        if(pause_t >= 0.0) pause_end();
        return;
    }
    if(pause_t < 0.0){ pause_t = now; pause_cut = 0.0; }
    want = now - pause_t - PAUSE_HOLD;
    if(want <= pause_cut) return;
    if(pause_cut == 0.0) pause_at = pause_t + PAUSE_HOLD - t_origin;
    t_origin += want - pause_cut;
    pause_cut = want;
    /* the frames sampled meanwhile are not skipped ones */
    if(cap_since_emit > 1) cap_since_emit = 1;
}

/* Called after every batch of the main loop, before the present-phase test
 * that may end it. */
void video_poll(void){
    double per, inv, hde, now = emu_now(), t0 = plat_time();
    int vt, vd, vrs, vre;
    unsigned long long idx;
    if(!started){
        started = 1;
        /* a -load session starts mid-stream; the video starts with it */
        t_origin = (double)(unsigned long long)(now / VID_PER) * VID_PER;
    }
    vga_timing_cached(&per, &inv, &vt, &vd, &vrs, &vre, &hde);
    (void)inv; (void)vt; (void)vd; (void)vrs; (void)vre; (void)hde;
    idx = per > 0.0 ? (unsigned long long)(now / per) : 0ULL;
    if(!have_cap || idx != cap_idx){
        /* the quiet window, or - once a whole guest frame has gone by
         * without reaching it - whatever this moment shows */
        int late = have_cap && idx > cap_idx + 1;
        if(!have_cap || present_phase_in() || late){
            vga_render(nat, &nat_w, &nat_h);
            cap_idx = idx; have_cap = 1; out_dirty = 1;
            cap_since_emit++; n_cap++;
            if(late) n_late++;
        }
    }
    pause_track(now);
    while(now - t_origin >= ((double)k_out + 0.5) * VID_PER) emit();
    t_wall += plat_time() - t0;
}

/* The latest sampled frame, for a window that is also showing the session:
 * a second vga_render() at wall-clock moments would feed its render-path
 * latches (the menu's AR14 split) a history that differs run to run. */
int video_last_frame(uint32_t *dst, int *w, int *h){
    if(!have_cap) return 0;
    memcpy(dst, nat, sizeof(uint32_t) * (size_t)nat_w * nat_h);
    *w = nat_w; *h = nat_h;
    return 1;
}

static void aud_write(const int16_t *s, size_t n){
    if(n && fwrite(s, sizeof(int16_t), n, aud_fp) != n) fail("the sound file write failed");
    a_out += n;
}

static void aud_pad_to(unsigned long long target){
    static const int16_t zero[1024];
    while(a_out < target){
        unsigned long long n = target - a_out;
        aud_write(zero, (size_t)(n > 1024 ? 1024 : n));
    }
}

/* n samples the card played at `rate`, the first one due at emulated time
 * t_first (src/sound.c, sb_tick).  The stream is continuous as long as the
 * card keeps playing; a start more than 20 ms past where the output stands
 * is a gap, filled with silence. */
void video_audio_feed(const int16_t *s, int n, double rate, double t_first){
    int16_t buf[1024];
    int m = 0;
    double step, want;
    if(!aud_fp || !started || n <= 0 || rate <= 0.0) return;
    /* nothing was measured playing in a pause; were it, it has no place */
    if(pause_cut > 0.0) return;
    want = (t_first - t_origin) * AUD_HZ;
    if(want > (double)a_out + 0.020 * AUD_HZ){
        aud_pad_to((unsigned long long)want);
        a_prev = 0; a_pos = -1.0;
        a_gaps++;
    }
    /* linear interpolation, s[-1] being the previous call's last sample */
    step = rate / AUD_HZ;
    while(a_pos < (double)(n - 1)){
        int i = (int)(a_pos < 0.0 ? -1 : a_pos);
        double f = a_pos - i;
        int a = i < 0 ? a_prev : s[i], b = s[i + 1];
        buf[m++] = (int16_t)(a + (b - a) * f);
        if(m == 1024){ aud_write(buf, (size_t)m); m = 0; }
        a_pos += step;
    }
    aud_write(buf, (size_t)m);
    a_pos -= n;
    a_prev = s[n - 1];
}

/* After the loop: write what is due, bring the sound to the video's length,
 * close both, and say what it cost. */
void video_close(void){
    double t0 = plat_time();
    if(!video_on) return;
    if(started){
        double now = emu_now();
        pause_track(now);
        while(now - t_origin >= ((double)k_out + 0.5) * VID_PER) emit();
        if(pause_t >= 0.0) pause_end();
    }
    if(vid_fp && fclose(vid_fp) != 0) fail("the frame pipe closed");
    vid_fp = NULL;
    if(aud_fp){
        aud_pad_to((unsigned long long)((double)k_out * VID_PER * AUD_HZ + 0.5));
        fflush(aud_fp);
        fseek(aud_fp, 0, SEEK_SET);
        wav_header(aud_fp, (uint32_t)a_out);
        if(fclose(aud_fp) != 0) fail("the sound file write failed");
        aud_fp = NULL;
    }
    t_wall += plat_time() - t0;
    fprintf(stderr, "[video] %llu frames (%.3f s) %dx%d: %lu guest frames"
                    " sampled, %lu late, %lu repeated, %lu skipped; sound"
                    " %llu samples, %lu gaps; %lu pauses cut, %.3f s;"
                    " %.2f s of host time%s\n",
            k_out, (double)k_out * VID_PER, vid_w, vid_h, n_cap, n_late,
            n_dup, n_drop, a_out, a_gaps, n_pause, cut_total, t_wall,
            vid_failed ? " - FAILED" : "");
    video_on = 0;
}
