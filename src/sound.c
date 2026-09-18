/* Sound: 8237 DMA controller, Sound Blaster DSP, and a Win32 waveOut sink.
 *
 * What the game's SBLASTER.SDR actually does.  Re-derived by disassembling
 * the driver, after an -snddbg trace disagreed with the description that used
 * to be here; offsets are into the driver's load image:
 *
 *   out 0Ah,05        mask DMA channel 1
 *   out 0Ch,00        clear the address flip-flop
 *   out 0Bh,59        mode = channel | 58h: memory->device, single transfer,
 *                     and auto-init SET                              (14BF)
 *   out 83h,08        page 08 -> buffer at 08:0020 physical
 *   out 02h,20/00     offset 0020
 *   out 03h,lo/hi     count = DMA block length - 1                   (14DB)
 *   out 226h,1 / 0    DSP reset, then poll 22Ah for AAh              (1427)
 *   out 22Ch,D1       speaker on                                     (1465)
 *   out 22Ch,40,tc    time constant, computed from the quality notch's
 *                     mixing rate (see cfg_sblaster in src/launch.c) (1473)
 *   out 22Ch,14,len   8-bit SINGLE-CYCLE DMA output                  (16ED)
 *
 * The last line is the correction.  This comment used to say the driver
 * issues 1Ch, the auto-init DSP command.  It does not: that byte appears
 * nowhere in SBLASTER.SDR, nor in any other SoundBlaster driver the game
 * ships - SB20 and SBPRO set a block size with 48h first and then also use
 * 14h, SB16 likewise.  What is auto-init here is the *controller*, not the
 * DSP: the 0Bh write above.  So the 8237 wraps the buffer on its own and the
 * driver hands the DSP a fresh 14h each time its transfer runs out, which is
 * the standard SB 1.x way of playing continuously - and why -snddbg logs one
 * "start single" line per re-arm rather than a single line at the start.
 *
 * The DSP length is deliberately huge: the driver takes the largest whole
 * multiple of the DMA block length that fits under 64 KB (1733: 0FFFFh / L
 * * L, which came out as 65520 in the traces) so that re-arming happens as
 * seldom as it can.  At 21 kHz that is one DSP interrupt every three
 * seconds, far too rare to be what paces the software MOD mixer; the
 * driver's PLL calibration against the retrace is the likelier pacer, but
 * that has not been traced and is not claimed here.
 *
 * pfemu had the interrupt wrong until this was traced.  It ended a
 * single-cycle transfer at the *controller's* terminal count, raising the
 * DSP interrupt once per DMA wrap instead of once per DSP transfer.
 * -snddbg measured the gap, and it was not small:
 *
 *   phase   DMA buffer         wrap            DSP transfer
 *   intro   10920 B @ 0020     every 0.513 s   every 3.08 s   (6x)
 *   table     840 B @ AD00     every 0.040 s   every 3.08 s   (78x)
 *
 * 65520 is exactly 6 x 10920 and 78 x 840 - the 0FFFFh / L * L formula
 * landing on a whole number of buffers both times, as intended.
 *
 * Which of the two was right could not be argued from the disassembly, since
 * an 840-byte buffer is 39.5 ms of audio and something has to refill it at
 * least that often.  It was settled by ear instead: with the DSP counting
 * its own transfer the music plays exactly as it did before, which it could
 * not do if the driver refilled that buffer from this interrupt.  So the
 * refill is paced by the driver's own retrace-locked timer, and 77 of every
 * 78 interrupts pfemu used to raise were spurious.  sb_tick now counts the
 * DSP transfer; -dmairq goes back to the controller wrap if something ever
 * turns up that wants it.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include "pfemu.h"

int sound_debug = 0;

/* -dmairq: go back to interrupting on the DMA controller's wrap rather than
 * on the DSP's own transfer count.  Kept as a way back, not as a tuning
 * knob - see the header for why the default changed. */
int sb_dmairq = 0;

/* ------------------------------------------------------------------ gain */
/* A real SB had a volume wheel on the bracket and the speakers had another;
 * the guest has neither, and the mixer writes full-scale samples, so every
 * build before this one played at whatever the Windows mixer was set to.
 * This is that missing wheel.  It lives in the host sink only: no emulated
 * state, no timing, and nothing the guest can observe depends on it, and the
 * -wav dump is written upstream of it so captures stay at the level the card
 * actually produced.  See plat_audio_push(). */
int audio_volume = AUDIO_VOLUME_DEFAULT;

/* Set when the in-window keys move the level, so main() knows to write it
 * back on the way out.  A -vol run that nobody touches leaves the saved
 * level alone. */
int audio_volume_dirty = 0;

/* Amplitude multiplier in Q15, square-law rather than linear.  Loudness
 * tracks amplitude far from proportionally, so a slider scaling amplitude
 * directly does nearly all of its audible work in the bottom third and feels
 * dead across the top; squaring spreads that out at 12 dB per halving of the
 * slider, near enough to how an audio-taper pot behaves.  100 returns exactly
 * 32768, so unity - the level every previous build played at - is still
 * reachable, and costs a memcpy rather than a multiply per sample. */
static int audio_gain_q15(void){
    int v = audio_volume;
    if(v < 0) v = 0;
    if(v > 100) v = 100;
    return (v * v * 32768) / 10000;
}

/* ------------------------------------------------------------------ 8237 */
typedef struct {
    uint16_t addr, count, base_addr, base_count;
    uint8_t  page, mode, masked;
} DMACH;
static DMACH dma[4];
static int dma_ff;                     /* low/high byte flip-flop */

static void dma_reset(void){
    int i;
    memset(dma, 0, sizeof(dma));
    for(i=0;i<4;i++) dma[i].masked = 1;
    dma_ff = 0;
}

void dma_write(uint16_t p, uint8_t v){
    switch(p){
    case 0x00: case 0x02: case 0x04: case 0x06: {        /* address */
        int c = p >> 1;
        if(!dma_ff) dma[c].base_addr = (dma[c].base_addr & 0xFF00) | v;
        else        dma[c].base_addr = (uint16_t)((dma[c].base_addr & 0x00FF) | (v << 8));
        dma[c].addr = dma[c].base_addr;
        dma_ff ^= 1;
        return; }
    case 0x01: case 0x03: case 0x05: case 0x07: {        /* count */
        int c = p >> 1;
        if(!dma_ff) dma[c].base_count = (dma[c].base_count & 0xFF00) | v;
        else        dma[c].base_count = (uint16_t)((dma[c].base_count & 0x00FF) | (v << 8));
        dma[c].count = dma[c].base_count;
        dma_ff ^= 1;
        return; }
    case 0x0A: dma[v & 3].masked = (v & 4) ? 1 : 0; return;   /* single mask  */
    case 0x0B: dma[v & 3].mode = v; return;                   /* mode         */
    case 0x0C: dma_ff = 0; return;                            /* clear ff     */
    case 0x0D: dma_reset(); return;                           /* master clear */
    case 0x0F: { int i; for(i=0;i<4;i++) dma[i].masked = (v >> i) & 1; return; }
    case 0x87: dma[0].page = v; return;
    case 0x83: dma[1].page = v; return;
    case 0x81: dma[2].page = v; return;
    case 0x82: dma[3].page = v; return;
    }
}

uint8_t dma_read(uint16_t p){
    switch(p){
    case 0x00: case 0x02: case 0x04: case 0x06: {
        int c = p >> 1; uint8_t r;
        r = dma_ff ? (uint8_t)(dma[c].addr >> 8) : (uint8_t)dma[c].addr;
        dma_ff ^= 1; return r; }
    case 0x01: case 0x03: case 0x05: case 0x07: {
        int c = p >> 1; uint8_t r;
        r = dma_ff ? (uint8_t)(dma[c].count >> 8) : (uint8_t)dma[c].count;
        dma_ff ^= 1; return r; }
    case 0x87: return dma[0].page;
    case 0x83: return dma[1].page;
    case 0x81: return dma[2].page;
    case 0x82: return dma[3].page;
    }
    return 0xFF;
}

/* Pull one byte across the channel.  Returns -1 when the block has ended and
 * the channel is not auto-init (so the caller knows to stop). */
static int dma_fetch(int c, int *end_of_block){
    uint32_t phys;
    int b;
    *end_of_block = 0;
    if(dma[c].masked) return -1;
    phys = ((uint32_t)dma[c].page << 16) | dma[c].addr;
    b = mem_r8(phys);
    dma[c].addr++;
    if(dma[c].count == 0){
        *end_of_block = 1;
        if(dma[c].mode & 0x10){                 /* auto-init: reload and go on */
            dma[c].addr  = dma[c].base_addr;
            dma[c].count = dma[c].base_count;
        } else {
            dma[c].masked = 1;
        }
    } else {
        dma[c].count--;
    }
    return b;
}

/* ------------------------------------------------------- Sound Blaster DSP */
#define SB_BASE 0x220
int sb_irq = 7;

static struct {
    int  reset_stage;
    uint8_t outbuf[8]; int outlen, outpos;
    uint8_t cmd; int need_args; uint8_t arg[2]; int nargs;
    int  time_constant;
    int  block_size;          /* bytes in one DSP transfer */
    int  dsp_left;            /* bytes still owed on it, for -dspcount */
    int  playing, auto_init;
    int  speaker;
    int  irq_pending;
    double rate;              /* samples per second */
    double frac;              /* fractional sample carried between ticks */
    double last_t;
} sb;

static void sb_out(uint8_t v){
    if(sb.outlen < (int)sizeof(sb.outbuf)) sb.outbuf[sb.outlen++] = v;
}

void sb_reset_dev(void){
    memset(&sb, 0, sizeof(sb));
    sb.rate = 22050.0;
    sb.last_t = -1.0;
}

static void sb_start(int auto_init, int len){
    sb.playing = 1;
    sb.auto_init = auto_init;
    if(len > 0) sb.block_size = len;
    sb.dsp_left = sb.block_size;
    if(sb.time_constant > 0 && sb.time_constant < 256)
        sb.rate = 1000000.0 / (256.0 - (double)sb.time_constant);
    if(sb.rate < 4000.0) sb.rate = 4000.0;
    if(sb.rate > 48000.0) sb.rate = 48000.0;
    sb.last_t = emu_now();
    sb.frac = 0.0;
    plat_audio_init((int)(sb.rate + 0.5));
    if(sound_debug){
        /* Both lengths and the gap between re-arms, because the interesting
         * question is how the DSP's transfer length relates to the DMA
         * block the controller is actually looping - see the header. */
        static double prev_start = -1.0;
        fprintf(stderr,
                "[sb] start %s, %d bytes, tc=%d -> %.0f Hz | t=%.3f dt=%.3f | "
                "dma1 mode=%02X (%s) page=%02X addr=%04X count=%u\n",
                auto_init ? "auto-init" : "single", len, sb.time_constant, sb.rate,
                sb.last_t, prev_start < 0.0 ? 0.0 : sb.last_t - prev_start,
                dma[1].mode, (dma[1].mode & 0x10) ? "auto-init" : "single",
                dma[1].page, dma[1].base_addr, (unsigned)dma[1].base_count + 1u);
        prev_start = sb.last_t;
    }
}

static void sb_command(uint8_t c){
    switch(c){
    case 0x10: sb.cmd = c; sb.need_args = 1; sb.nargs = 0; return;   /* direct DAC */
    case 0x14: case 0x24:
    case 0x1C: case 0x2C:
        sb.cmd = c; sb.need_args = (c == 0x14 || c == 0x24) ? 2 : 0;
        sb.nargs = 0;
        if(!sb.need_args) sb_start(1, sb.block_size);
        return;
    case 0x40: sb.cmd = c; sb.need_args = 1; sb.nargs = 0; return;   /* time const */
    case 0x48: sb.cmd = c; sb.need_args = 2; sb.nargs = 0; return;   /* block size */
    case 0xD0: sb.playing = 0; return;                               /* halt DMA   */
    case 0xD4: if(sb.block_size) sb.playing = 1; sb.last_t = emu_now(); return;
    case 0xD1: sb.speaker = 1; return;
    case 0xD3: sb.speaker = 0; return;
    case 0xD8: sb_out((uint8_t)(sb.speaker ? 0xFF : 0x00)); return;
    case 0xE1: sb_out(0x01); sb_out(0x05); return;   /* DSP 1.05: an original SB */
    case 0xE0: sb.cmd = c; sb.need_args = 1; sb.nargs = 0; return;   /* identify   */
    case 0xF2: sb.irq_pending = 1; pic_raise(sb_irq); return;        /* force IRQ  */
    default:
        if(sound_debug) fprintf(stderr, "[sb] unhandled DSP command %02X\n", c);
        return;
    }
}

static void sb_command_arg(uint8_t v){
    sb.arg[sb.nargs++] = v;
    if(sb.nargs < sb.need_args) return;
    switch(sb.cmd){
    case 0x10: break;                                   /* direct DAC: ignored */
    case 0x40: sb.time_constant = v; break;
    case 0x48: sb.block_size = (sb.arg[0] | (sb.arg[1] << 8)) + 1; break;
    case 0x14: case 0x24:
        sb_start(0, (sb.arg[0] | (sb.arg[1] << 8)) + 1); break;
    case 0xE0: sb_out((uint8_t)~v); break;
    }
    sb.need_args = 0; sb.nargs = 0;
}

void sb_write(uint16_t p, uint8_t v){
    switch(p - SB_BASE){
    case 0x06:                                   /* DSP reset */
        if(v & 1) sb.reset_stage = 1;
        else if(sb.reset_stage){
            sb.reset_stage = 0;
            sb.playing = 0; sb.outlen = sb.outpos = 0;
            sb.need_args = 0; sb.nargs = 0;
            /* A reset also drops any interrupt the card was still asserting.
             * Without this, a second driver instance (each game program loads
             * its own) unmasks the line and immediately takes an interrupt
             * left over from the previous one, part-way through its own
             * initialisation. */
            sb.irq_pending = 0;
            pic_lower(sb_irq);
            sb_out(0xAA);                        /* the "I am here" reply */
            if(sound_debug) fprintf(stderr, "[sb] DSP reset\n");
        }
        return;
    case 0x0C:                                   /* command / data */
        if(sb.need_args) sb_command_arg(v);
        else sb_command(v);
        return;
    }
}

uint8_t sb_read(uint16_t p){
    switch(p - SB_BASE){
    case 0x0A:                                   /* read data */
        if(sb.outpos < sb.outlen){
            uint8_t r = sb.outbuf[sb.outpos++];
            if(sb.outpos >= sb.outlen) sb.outpos = sb.outlen = 0;
            return r;
        }
        return 0x00;
    case 0x0C: return 0x7F;                      /* write buffer always ready */
    case 0x0E:                                   /* read-buffer status + IRQ ack */
        sb.irq_pending = 0;
        pic_lower(sb_irq);
        return (uint8_t)((sb.outpos < sb.outlen) ? 0xFF : 0x7F);
    }
    return 0xFF;
}

/* Called often from the main loop: move emulated time forward, pull the bytes
 * the card would have consumed in that interval, and hand them to the host. */
#define SB_CHUNK 512
static int16_t pcm[SB_CHUNK];
static int pcm_n;

void sb_tick(void){
    double now, dt;
    int due;
    if(!sb.playing || sb.last_t < 0.0) return;
    now = emu_now();
    dt = now - sb.last_t;
    if(dt <= 0.0) return;
    if(dt > 0.25) dt = 0.25;                     /* never try to catch up far */
    sb.last_t = now;
    sb.frac += dt * sb.rate;
    due = (int)sb.frac;
    if(due <= 0) return;
    sb.frac -= due;
    if(due > 4096) due = 4096;
    while(due-- > 0){
        int eob, b = dma_fetch(1, &eob);
        if(b < 0){ sb.playing = 0; break; }
        pcm[pcm_n++] = (int16_t)((b - 128) * 192);
        if(pcm_n == SB_CHUNK){ plat_audio_push(pcm, pcm_n); pcm_n = 0; }
        /* What ends a transfer, and so interrupts: the DSP's own byte count,
         * which is the card's job on hardware.  The controller's wrap (eob)
         * is invisible to the DSP - it just reloads and keeps going.  With no
         * length ever given (a 1Ch with no preceding 48h, which none of the
         * drivers here do) there is no count to run down, so fall back to the
         * wrap rather than fire on every single sample. */
        { int fire;
          if(sb_dmairq || sb.block_size <= 0) fire = eob;
          else fire = (--sb.dsp_left <= 0);
          if(fire){
              sb.dsp_left = sb.block_size;
              sb.irq_pending = 1;
              pic_raise(sb_irq);
              if(!sb.auto_init) sb.playing = 0;
          } }
    }
}

/* ---------------------------------------------------------------- OPL stub */
static uint8_t opl_regs[512];
void opl_write(int reg, uint8_t v){ opl_regs[reg & 511] = v; }
uint8_t opl_status(void){ return 0x06; }
void spk_update(int on, uint16_t div){ (void)on; (void)div; }

/* ------------------------------------------------------------- Win32 audio */
#define NBUF 8
#define BUFSAMP 512
static HWAVEOUT hwo;
static WAVEHDR  hdrs[NBUF];
static int16_t  bufs[NBUF][BUFSAMP];
static int      hdr_i;
static int      audio_hz;
static void wav_open(int hz);

void plat_audio_init(int hz){
    WAVEFORMATEX wf;
    int i;
    if(hwo && hz == audio_hz) return;
    if(hwo){ waveOutReset(hwo); waveOutClose(hwo); hwo = NULL; }
    audio_hz = hz;
    wav_open(hz);
    memset(&wf, 0, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 1;
    wf.nSamplesPerSec = hz;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 2;
    wf.nAvgBytesPerSec = hz * 2;
    if(waveOutOpen(&hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR){
        hwo = NULL;
        fprintf(stderr, "[snd] waveOutOpen failed; running silent\n");
        return;
    }
    memset(hdrs, 0, sizeof(hdrs));
    for(i=0;i<NBUF;i++) hdrs[i].dwFlags = WHDR_DONE;
    hdr_i = 0;
    fprintf(stderr, "[snd] audio out at %d Hz\n", hz);
}

/* -wav <file>: also write everything we hand to the host, so a headless run
 * can be listened to (and measured) afterwards. */
static FILE *wav_fp;
static unsigned long wav_samples;
static uint64_t wav_hash = 1469598103934665603ULL;
static int wav_ok = 0;
const char *wav_path;

static void wav_open(int hz){
    uint8_t h[44];
    if(!wav_path || wav_fp) return;
    wav_fp = fopen(wav_path, "wb");
    if(!wav_fp) return;
    wav_ok = 1;
    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4); memcpy(h+8, "WAVEfmt ", 8);
    h[16] = 16; h[20] = 1; h[22] = 1;
    h[24] = (uint8_t)hz; h[25] = (uint8_t)(hz>>8);
    h[26] = (uint8_t)(hz>>16); h[27] = (uint8_t)(hz>>24);
    { unsigned long br = (unsigned long)hz*2;
      h[28]=(uint8_t)br; h[29]=(uint8_t)(br>>8); h[30]=(uint8_t)(br>>16); h[31]=(uint8_t)(br>>24); }
    h[32] = 2; h[34] = 16;
    memcpy(h+36, "data", 4);
    fwrite(h, 1, 44, wav_fp);
}
void wav_close(void){
    unsigned long d, r;
    if(!wav_fp) return;
    d = wav_samples * 2; r = d + 36;
    fseek(wav_fp, 4, SEEK_SET);  fputc((int)(r&0xFF),wav_fp); fputc((int)((r>>8)&0xFF),wav_fp);
    fputc((int)((r>>16)&0xFF),wav_fp); fputc((int)((r>>24)&0xFF),wav_fp);
    fseek(wav_fp, 40, SEEK_SET); fputc((int)(d&0xFF),wav_fp); fputc((int)((d>>8)&0xFF),wav_fp);
    fputc((int)((d>>16)&0xFF),wav_fp); fputc((int)((d>>24)&0xFF),wav_fp);
    fclose(wav_fp); wav_fp = NULL;
    fprintf(stderr, "[snd] wrote %s: %lu samples\n", wav_path, wav_samples);
}

void plat_audio_push(const int16_t *s, int n){
    WAVEHDR *h;
    int take, g;
    if(wav_fp){
        const uint8_t *b = (const uint8_t*)s;
        int i, m = n * 2;
        fwrite(s, 2, (size_t)n, wav_fp); wav_samples += (unsigned long)n;
        /* Running FNV-1a over the raw sample bytes (captured upstream of
         * the host gain, so volume/mute never move it).  Queryable at any
         * time, so record/replay footers don't depend on close order. */
        for(i=0;i<m;i++){ wav_hash ^= b[i]; wav_hash *= 1099511628211ULL; }
    }
    if(!hwo) return;
    g = audio_gain_q15();                          /* read once: -/+ can move it */
    while(n > 0){
        h = &hdrs[hdr_i];
        if(!(h->dwFlags & WHDR_DONE)) return;      /* queue full: drop, stay live */
        if(h->lpData) waveOutUnprepareHeader(hwo, h, sizeof(*h));
        take = n > BUFSAMP ? BUFSAMP : n;
        if(g >= 32768) memcpy(bufs[hdr_i], s, (size_t)take * 2);
        else if(g == 0) memset(bufs[hdr_i], 0, (size_t)take * 2);
        else { int k; for(k=0;k<take;k++) bufs[hdr_i][k] = (int16_t)(((int)s[k] * g) / 32768); }
        memset(h, 0, sizeof(*h));
        h->lpData = (LPSTR)bufs[hdr_i];
        h->dwBufferLength = (DWORD)(take * 2);
        waveOutPrepareHeader(hwo, h, sizeof(*h));
        waveOutWrite(hwo, h, sizeof(*h));
        hdr_i = (hdr_i + 1) % NBUF;
        s += take; n -= take;
    }
}

void plat_audio_close(void){
    if(hwo){ waveOutReset(hwo); waveOutClose(hwo); hwo = NULL; }
}

/* Current -wav capture hash for the replay footer (src/replay.c).  "none"
 * when no -wav file was requested or the open failed; otherwise FNV-1a
 * over every sample byte written so far.  Safe to call before wav_close:
 * no more pushes happen after the main loop, so record- and replay-end
 * both observe the final value. */
void wav_current_hash(char out[17], unsigned long *samples_out){
    if(samples_out) *samples_out = wav_ok ? wav_samples : 0;
    if(!wav_path || !wav_ok){ snprintf(out, 17, "none"); return; }
    snprintf(out, 17, "%016llx", (unsigned long long)wav_hash);
}

void sound_init(void){ dma_reset(); sb_reset_dev(); }

/* Mid-table savestate (src/snapshot.c): the DMA channels and the DSP's
 * command/playout state, plus the AdLib register file and the pending PCM
 * push buffer (host-side, but dropping it would click).  -wav capture and
 * waveOut queue state are output-only and not stored. */
void sound_save_state(SnapW *w){
    snap_w_bytes(w, dma, sizeof(dma));
    snap_w_u32(w, (uint32_t)dma_ff);
    snap_w_bytes(w, &sb, sizeof(sb));
    snap_w_bytes(w, opl_regs, sizeof(opl_regs));
    snap_w_u32(w, (uint32_t)pcm_n);
    snap_w_bytes(w, pcm, sizeof(pcm));
}
int sound_load_state(SnapR *r){
    snap_r_bytes(r, dma, sizeof(dma));
    dma_ff = (int)snap_r_u32(r);
    snap_r_bytes(r, &sb, sizeof(sb));
    snap_r_bytes(r, opl_regs, sizeof(opl_regs));
    pcm_n = (int)snap_r_u32(r);
    snap_r_bytes(r, pcm, sizeof(pcm));
    if(r->err || pcm_n < 0 || pcm_n > SB_CHUNK) return -1;
    return 0;
}
