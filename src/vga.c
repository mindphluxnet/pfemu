/* Memory dispatch + VGA emulation (planar / chain-4 / mode-X / text) */
#include "pfemu.h"

uint8_t vga_vram[256*1024];
int vga_dirty = 1;
int vga_force256 = 0, vga_nodbl = 0;
unsigned long vga_startaddr_changes = 0;

/* ------------------------------------------------------------- registers */
static uint8_t sq[8];        /* sequencer            3C4/3C5 */
static uint8_t gc[16];       /* graphics controller  3CE/3CF */
static uint8_t cr[64];       /* CRTC                 3D4/3D5 */
static uint8_t ar[32];       /* attribute controller 3C0     */
static uint8_t sq_idx, gc_idx, cr_idx, ar_idx;
static int ar_flipflop;
static uint8_t misc_out = 0x67;
static uint8_t dac[256][3];
static uint8_t dac_mask = 0xFF;
static int dac_widx, dac_ridx, dac_wcomp, dac_rcomp;
static uint8_t latch[4];
static int bios_mode = 3;

/* CRT timing cache, refreshed lazily by vga_timing_cached().  Set to 1
 * whenever a register it derives from changes. */
static int timing_dirty = 1;
static double timing_per, timing_hde, timing_inv_per;
static int timing_vtotal, timing_vde, timing_vrs, timing_vre;

uint32_t vga_retrace_hz = 70;

/* --------------------------------------------------------------- helpers */
static int chain4(void){ return (sq[4] & 0x08) != 0; }
static int oddeven(void){ return (sq[4] & 0x04) == 0; }   /* 0 in bit2 => odd/even on */
static int gfx_mode(void){ return (gc[6] & 0x01) != 0; }

static uint32_t vga_base(void){
    switch((gc[6]>>2)&3){
    case 0: return 0xA0000;   /* 128K */
    case 1: return 0xA0000;   /* 64K  */
    case 2: return 0xB0000;
    default: return 0xB8000;
    }
}
static uint32_t vga_size(void){
    switch((gc[6]>>2)&3){
    case 0: return 0x20000;
    case 1: return 0x10000;
    default: return 0x8000;
    }
}

/* ------------------------------------------------------ memory interface */
uint8_t vga_mem_r(uint32_t a){
    uint32_t off = a - vga_base();
    if(off >= vga_size()) return 0xFF;
    if(chain4()){
        latch[0]=vga_vram[(off&~3u)|0]; latch[1]=vga_vram[(off&~3u)|1];
        latch[2]=vga_vram[(off&~3u)|2]; latch[3]=vga_vram[(off&~3u)|3];
        return vga_vram[off & 0x3FFFF];
    }
    if(oddeven() && !(sq[4]&0x08)){
        uint32_t o = (off >> 1) & 0xFFFF;
        latch[0]=vga_vram[o*4+0]; latch[1]=vga_vram[o*4+1];
        latch[2]=vga_vram[o*4+2]; latch[3]=vga_vram[o*4+3];
        return vga_vram[o*4 + (off&1)];
    }
    {
        uint32_t o = off & 0xFFFF;
        latch[0]=vga_vram[o*4+0]; latch[1]=vga_vram[o*4+1];
        latch[2]=vga_vram[o*4+2]; latch[3]=vga_vram[o*4+3];
        if((gc[5]&0x08)==0) return latch[gc[4]&3];
        else {                              /* read mode 1: colour compare */
            uint8_t res=0; int b;
            for(b=0;b<8;b++){
                int m=1, p, ok=1;
                for(p=0;p<4;p++){
                    if(!((gc[7]>>p)&1)) continue;
                    if((((latch[p]>>b)&1)) != ((gc[2]>>p)&1)) ok=0;
                }
                if(ok) res |= (1<<b);
                (void)m;
            }
            return res;
        }
    }
}

static uint8_t alu_fn(uint8_t v, uint8_t l){
    switch((gc[3]>>3)&3){
    case 1: return v & l;
    case 2: return v | l;
    case 3: return v ^ l;
    default: return v;
    }
}

static void write_planes(uint32_t o, uint8_t data[4], uint8_t mask, uint8_t planemask){
    int p;
    for(p=0;p<4;p++){
        if(!((planemask>>p)&1)) continue;
        vga_vram[o*4+p] = (uint8_t)((vga_vram[o*4+p] & ~mask) | (data[p] & mask));
    }
}

void vga_mem_w(uint32_t a, uint8_t v){
    uint32_t off = a - vga_base();
    uint8_t data[4]; uint8_t bitmask; int p;
    if(off >= vga_size()) return;
    vga_dirty = 1;

    if(chain4()){
        uint32_t o = (off & ~3u) & 0x3FFFF;
        int plane = off & 3;
        if(!((sq[2]>>plane)&1)) return;
        vga_vram[o | plane] = v;
        return;
    }
    if(oddeven() && !(sq[4]&0x08)){
        uint32_t o = (off >> 1) & 0xFFFF;
        int plane = off & 1;
        if(!((sq[2]>>plane)&1)) return;
        vga_vram[o*4 + plane] = v;
        return;
    }
    {
        uint32_t o = off & 0xFFFF;
        int wm = gc[5] & 3;
        bitmask = gc[8];
        switch(wm){
        case 0: {
            uint8_t rot = (uint8_t)((v >> (gc[3]&7)) | (v << (8-(gc[3]&7))));
            if((gc[3]&7)==0) rot = v;
            for(p=0;p<4;p++){
                uint8_t src = ((gc[1]>>p)&1) ? (uint8_t)(((gc[0]>>p)&1)?0xFF:0x00) : rot;
                data[p] = alu_fn(src, latch[p]);
            }
            break; }
        case 1:
            for(p=0;p<4;p++) data[p] = latch[p];
            bitmask = 0xFF;
            break;
        case 2:
            for(p=0;p<4;p++) data[p] = alu_fn((uint8_t)(((v>>p)&1)?0xFF:0x00), latch[p]);
            break;
        default: {
            uint8_t rot = (uint8_t)((v >> (gc[3]&7)) | (v << (8-(gc[3]&7))));
            if((gc[3]&7)==0) rot = v;
            bitmask = (uint8_t)(gc[8] & rot);
            for(p=0;p<4;p++) data[p] = (uint8_t)(((gc[0]>>p)&1)?0xFF:0x00);
            break; }
        }
        write_planes(o, data, bitmask, sq[2]);
    }
}

/* --------------------------------------------------------- I/O interface */
uint8_t vga_io_r(uint16_t p){
    switch(p){
    case 0x3C0: return ar_idx;
    case 0x3C1: return ar[ar_idx & 0x1F];
    case 0x3C2: return 0x10;                       /* switch sense */
    case 0x3C4: return sq_idx;
    case 0x3C5: return sq[sq_idx & 7];
    case 0x3C6: return dac_mask;
    case 0x3C7: return 0;
    case 0x3C8: return (uint8_t)dac_widx;
    case 0x3C9: { uint8_t v = dac[dac_ridx & 0xFF][dac_rcomp];
                  if(++dac_rcomp==3){ dac_rcomp=0; dac_ridx=(dac_ridx+1)&0xFF; } return v; }
    case 0x3CC: return misc_out;
    case 0x3CE: return gc_idx;
    case 0x3CF: return gc[gc_idx & 15];
    case 0x3B4: case 0x3D4: return cr_idx;
    case 0x3B5: case 0x3D5: return cr[cr_idx & 63];
    case 0x3BA: case 0x3DA: {
        extern uint8_t vga_status1(void);
        ar_flipflop = 0;
        return vga_status1(); }
    }
    return 0xFF;
}

/* small ring buffer of attribute-controller traffic, for debugging */
static struct { uint16_t port; uint8_t val; uint8_t ff; } arlog[256];
static int arlog_n;

/* Present-phase timeline (-flipdbg, -vscan): WHERE in the emulated frame do
 * page flips land and VRAM mutate?  Needed because sampling the frame at the
 * wrong phase shows mid-draw pages (ball flicker).  Off by default. */
int vga_flipdbg = 0;
uint64_t vscan_step = 0;             /* sample VRAM hash every N instr (0=off) */
static uint64_t vscan_last = 0;
void vga_timing_cached(double*,double*,int*,int*,int*,int*,double*);
static uint64_t vscan_chunk[64];
static int vscan_init = 0;
static unsigned long vscan_logged = 0;

/* DMD step cadence (-dmd): the dot-matrix panel lives in VRAM chunks 46-47
 * (see dmd.log mask 0000C00000000000) and ignores the CRTC start address, so
 * viewport smoothing can't touch it.  This watches chunks 44-49 and logs
 * every transition with delta-t since the previous DMD change: the number
 * tells us whether choppy text is slow game pacing (design, off-limits) or
 * something pathological (port defect, fair game).  Sampling every 4096
 * instructions (~0.14 ms at 30 MIPS) resolves ms-scale steps. */
int vga_dmdlog = 0;
static uint64_t vscan_fnv(const uint8_t *p, size_t n);
static uint64_t dmd_last_sample = 0;
static uint64_t dmd_hash[6];
static double dmd_last_t = 0.0;
static int dmd_init = 0;
static unsigned long dmd_logged = 0;

static void dmd_poll(uint64_t c){
    int k, n = 0;
    double now, dt;
    if(c - dmd_last_sample < 4096) return;
    dmd_last_sample = c;
    for(k=0;k<6;k++){
        uint64_t h = vscan_fnv(&vga_vram[(44+k)*4096], 4096);
        if(!dmd_init || h != dmd_hash[k]){ dmd_hash[k] = h; n++; }
    }
    if(!dmd_init){ dmd_init = 1; dmd_last_t = emu_now(); return; }
    if(!n || n > 2) return;   /* bulk fills (loader, fades) are not DMD steps
                               * and don't consume the event budget either */
    if(dmd_logged++ > 20000){ vga_dmdlog = 0; fprintf(stderr, "[dmd] auto-off\n"); return; }
    now = emu_now();
    dt = now - dmd_last_t;
    dmd_last_t = now;
    fprintf(stderr, "[dmd] t=%.6f dt=%.4f n=%d\n", now, dt, n);
}

/* Current scan-line position in the frame, same basis as vga_status1(). */
static double vga_frameline(int *vtotal_out){
    double per, inv, hde, q, line;
    int vt, vd, vrs, vre;
    vga_timing_cached(&per, &inv, &vt, &vd, &vrs, &vre, &hde);
    (void)per; (void)hde; (void)vd; (void)vrs; (void)vre;
    q = emu_now() * inv;
    q -= (int64_t)q;
    line = q * (double)vt;
    if(vtotal_out) *vtotal_out = vt;
    return line;
}

static uint64_t vscan_fnv(const uint8_t *p, size_t n){
    uint64_t h = 1469598103934665603ULL;
    size_t i;
    for(i=0;i<n;i++){ h ^= p[i]; h *= 1099511628211ULL; }
    return h;
}

/* Called once per emulation batch from the main loop.  Samples a 64-chunk
 * hash of VRAM; on any change logs time, frame phase and change magnitude
 * (1-2 chunks = sprite-scale, dozens = blit/fill).  Auto-disables after
 * 100k events so a forgotten flag can't fill the disk. */
void vga_vscan_poll(void){
    uint64_t c = cpu.cycles;
    int k, changed = 0;
    uint64_t mask = 0;
    int vt;
    double line;
    if(vga_dmdlog) dmd_poll(c);
    if(!vscan_step || c - vscan_last < vscan_step) return;
    vscan_last = c;
    for(k=0;k<64;k++){
        uint64_t h = vscan_fnv(&vga_vram[k*4096], 4096);
        if(!vscan_init || h != vscan_chunk[k]){ vscan_chunk[k] = h; changed++; mask |= (1ULL << k); }
    }
    if(!vscan_init){ vscan_init = 1; return; }
    if(!changed) return;
    if(vscan_logged++ > 100000){ vscan_step = 0; fprintf(stderr, "[vscan] auto-off\n"); return; }
    line = vga_frameline(&vt);
    fprintf(stderr, "[vscan] t=%.6f line=%6.1f/%d n=%d m=%016llX\n",
            emu_now(), line, vt, changed, (unsigned long long)mask);
}

/* Smooth scrolling: the engine re-asserts the CRTC start address every
 * other frame (~30 Hz flip cadence), so sampling it raw steps visibly.
 * Interpolate between the last two published positions per present instead
 * of changing any game state.  -nosmooth disables.  hi/lo register pairs
 * arrive microseconds apart and would look like tiny extra flips, so writes
 * within 2 ms extend the current event rather than starting a new one;
 * jumps over half the address space are taken as wraps, not motion. */
int vga_smooth = 1;
static uint32_t sm_prev = 0, sm_last = 0, sm_seen = 0;
static double sm_prev_t = 0.0, sm_last_t = 0.0;
static int sm_have = 0;

static void smooth_note(uint32_t s){
    double now = emu_now();
    if(!sm_have){ sm_prev = sm_last = sm_seen = s; sm_prev_t = sm_last_t = now; sm_have = 1; return; }
    if(s == sm_seen) return;
    sm_seen = s;
    if(now - sm_last_t < 0.002){ sm_last = s; sm_last_t = now; return; }
    sm_prev = sm_last; sm_prev_t = sm_last_t;
    sm_last = s; sm_last_t = now;
}

static uint32_t smooth_start(uint32_t actual){
    double now, r, d;
    if(!vga_smooth || !sm_have) return actual;
    if(sm_last == sm_prev) return actual;
    if(sm_last_t <= sm_prev_t) return actual;
    d = (double)(int32_t)(sm_last - sm_prev);
    if(d > 32768.0 || d < -32768.0) return actual;   /* wrap, not motion */
    now = emu_now();
    r = (now - sm_prev_t) / (sm_last_t - sm_prev_t);
    if(r <= 0.0) return sm_prev;
    if(r >= 1.0) return actual;
    return (uint32_t)((double)sm_prev + d * r);
}

void vga_io_w(uint16_t p, uint8_t v){
    switch(p){
    case 0x3C0:
        arlog[arlog_n & 255].port = p;
        arlog[arlog_n & 255].val = v;
        arlog[arlog_n & 255].ff = (uint8_t)ar_flipflop;
        arlog_n++;
        if(!ar_flipflop){ ar_idx = v & 0x3F; ar_flipflop = 1; }
        else {
            if((ar_idx & 0x1F)==0x10 && ar[0x10]!=v)
                printf("[vga] AR10 %02X -> %02X  at %04X:%04X\n", ar[0x10], v,
                       cpu.sreg[S_CS], (unsigned)cpu.eip);
            ar[ar_idx & 0x1F] = v; ar_flipflop = 0; vga_dirty = 1;
        }
        break;
    case 0x3C2: misc_out = v; timing_dirty = 1; break;
    case 0x3C4: sq_idx = v & 7; break;
    case 0x3C5: sq[sq_idx & 7] = v; timing_dirty = 1; break;
    case 0x3C6: dac_mask = v; break;
    case 0x3C7: dac_ridx = v; dac_rcomp = 0; break;
    case 0x3C8: dac_widx = v; dac_wcomp = 0; break;
    case 0x3C9:
        dac[dac_widx & 0xFF][dac_wcomp] = v & 0x3F;
        if(++dac_wcomp==3){ dac_wcomp=0; dac_widx=(dac_widx+1)&0xFF; }
        vga_dirty = 1; break;
    case 0x3CE: gc_idx = v & 15; break;
    case 0x3CF: gc[gc_idx & 15] = v; break;
    case 0x3B4: case 0x3D4: cr_idx = v & 63; break;
    case 0x3B5: case 0x3D5:
        if(cr_idx==0x0C && cr[0x0C]!=v) vga_startaddr_changes++;
        cr[cr_idx & 63] = v; vga_dirty = 1; timing_dirty = 1;
        if(cr_idx==0x0C || cr_idx==0x0D)
            smooth_note((((uint32_t)cr[0x0C])<<8) | cr[0x0D]);
        if(vga_flipdbg && (cr_idx==0x0C || cr_idx==0x0D)){
            int vt; double line = vga_frameline(&vt);
            uint32_t start = ((uint32_t)cr[0x0C]<<8) | cr[0x0D];
            fprintf(stderr, "[flip] t=%.6f line=%6.1f/%d start=%04X %s from %04X:%04X\n",
                    emu_now(), line, vt, start, cr_idx==0x0C?"hi":"lo",
                    cpu.sreg[S_CS], (unsigned)cpu.eip);
        }
        break;
    }
}

/* ------------------------------------------------------------- rendering */
static uint32_t pal[256];

static void build_pal(void){
    int i;
    for(i=0;i<256;i++){
        uint8_t r = dac[i][0], g = dac[i][1], b = dac[i][2];
        pal[i] = 0xFF000000u | ((uint32_t)((r<<2)|(r>>4))<<16) |
                 ((uint32_t)((g<<2)|(g>>4))<<8) | (uint32_t)((b<<2)|(b>>4));
    }
}

static int vde(void){
    int v = cr[0x12] | (((cr[0x07]>>1)&1)<<8) | (((cr[0x07]>>6)&1)<<9);
    return v + 1;
}
static int line_compare(void){
    return cr[0x18] | (((cr[0x07]>>4)&1)<<8) | (((cr[0x09]>>6)&1)<<9);
}

/* 8x16 / 8x8 ROM font lives at F000:FA6E style; we keep our own copy */
extern const uint8_t vga_font8x16[256*16];
extern const uint8_t vga_font8x8[256*8];

void vga_render(uint32_t *out, int *wp, int *hp){
    int w, h, x, y;
    int maxscan = (cr[0x09] & 0x1F) + 1;
    int dbl = (cr[0x09] & 0x80) ? 2 : 1;
    if(vga_nodbl) dbl = 1;
    int offs = cr[0x13] ? cr[0x13] : 40;
    int is256 = (ar[0x10] & 0x40) != 0 || vga_force256;
    uint32_t start = smooth_start((((uint32_t)cr[0x0C])<<8) | cr[0x0D]);
    int lc = line_compare();
    int pel = ar[0x13] & 0x0F;

    build_pal();

    if(!gfx_mode()){
        /* ---- text mode ---- */
        int cols = cr[0x01] + 1;
        int chh  = maxscan;
        int rows  = (vde() / dbl) / chh;
        const uint8_t *font = (chh >= 14) ? vga_font8x16 : vga_font8x8;
        int fh = (chh >= 14) ? 16 : 8;
        if(cols<1||cols>200) cols=80;
        if(rows<1||rows>100) rows=25;
        w = cols*8; h = rows*chh;
        if(w > 800) w = 800;
        for(y=0;y<h;y++){
            int row = y / chh, sl = y % chh;
            for(x=0;x<w;x++){
                int col = x/8;
                uint32_t o = (start + row*offs*2 + col) & 0xFFFF;
                uint8_t ch = vga_vram[o*4+0], at = vga_vram[o*4+1];
                uint8_t bits = font[ch*fh + (sl<fh?sl:fh-1)];
                int on = (bits >> (7-(x&7))) & 1;
                uint8_t ci = on ? (at & 0x0F) : ((at>>4)&0x07);
                out[y*w+x] = pal[ci & 0xFF];
            }
        }
        *wp=w; *hp=h; return;
    }

    if(is256){
        /* a displayed row spans maxscan scanlines, doubled again when the
         * CRTC's scan-doubling bit is set (320x240 mode X does exactly that) */
        int rowh = maxscan * dbl;
        int split = (lc + 1) / rowh;                 /* first row after the split */
        w = (cr[0x01] + 1) * 4;
        h = vde() / rowh;
        if(w<16||w>1024) w=320;
        if(h<16||h>1024) h=200;
        for(y=0;y<h;y++){
            uint32_t ctr;
            if(split < h && y >= split) ctr = (uint32_t)(y - split) * (uint32_t)(offs*2);
            else ctr = start + (uint32_t)y*(uint32_t)(offs*2);
            for(x=0;x<w;x++){
                uint32_t px = (uint32_t)x + (uint32_t)(pel>>1);
                uint32_t o = (ctr + (px>>2)) & 0xFFFF;
                uint8_t c = vga_vram[o*4 + (px&3)];
                out[y*w+x] = pal[c & dac_mask];
            }
        }
        *wp=w; *hp=h; return;
    }

    /* ---- 16-colour planar ---- */
    {
    /* Scan-doubled planar modes (the table-select menu is 640x240 timing
     * doubled to 480 scanlines) have tall 1:2 pixels: single-width dots
     * horizontally, two scanlines per row vertically.  The 256-colour
     * Mode-X playfield stays square without help because its pixels are
     * already double-width, but planar needs each row emitted twice so the
     * square-pixel framebuffer fills the window instead of letterboxing to
     * half height.  On period hardware both fill the screen (480 scanlines),
     * so without this the sidebar scroll (256-wide, full height) visibly
     * shrinks to the letterboxed menu. */
    int rowh = maxscan * dbl;
    int h_log, split_log, dup = (dbl == 2) ? 2 : 1;
    w = (cr[0x01] + 1) * 8;
    h_log = vde() / rowh;
    split_log = (lc + 1) / rowh;
    h = h_log * dup;
    if(w<16||w>1024) w=640;
    if(h<16||h>1024) h=480;
    for(y=0;y<h;y++){
        int yl = dup == 2 ? y >> 1 : y;
        uint32_t ctr;
        if(split_log < h_log && yl >= split_log) ctr = (uint32_t)(yl - split_log) * (uint32_t)(offs*2);
        else ctr = start + (uint32_t)yl*(uint32_t)(offs*2);
        for(x=0;x<w;x++){
            uint32_t px = (uint32_t)x + (uint32_t)pel;
            uint32_t o = (ctr + (px>>3)) & 0xFFFF;
            int bit = 7 - (px & 7);
            int ci = (((vga_vram[o*4+0]>>bit)&1)) | (((vga_vram[o*4+1]>>bit)&1)<<1) |
                     (((vga_vram[o*4+2]>>bit)&1)<<2) | (((vga_vram[o*4+3]>>bit)&1)<<3);
            {
                uint8_t a = ar[ci & 0x0F];
                uint8_t di;
                if(ar[0x10] & 0x80) di = (uint8_t)((a & 0x0F) | ((ar[0x14]&0x0F)<<4));
                else di = (uint8_t)((a & 0x3F) | ((ar[0x14]&0x0C)<<4));
                out[y*w+x] = pal[di];
            }
        }
    }
    }
    *wp=w; *hp=h;
}

/* --------------------------------------------------------- BIOS mode set */
struct modedef { uint8_t cr[25]; uint8_t sq[5]; uint8_t gc[9]; uint8_t ar[21]; uint8_t misc; };

static void apply_regs(const uint8_t *c, const uint8_t *s, const uint8_t *g, const uint8_t *a, uint8_t mo){
    int i;
    misc_out = mo;
    for(i=0;i<5;i++) sq[i]=s[i];
    for(i=0;i<25;i++) cr[i]=c[i];
    for(i=0;i<9;i++) gc[i]=g[i];
    for(i=0;i<21;i++) ar[i]=a[i];
    ar_flipflop = 0;
    timing_dirty = 1;
}

static const uint8_t c_text80[25] = {
 0x5F,0x4F,0x50,0x82,0x55,0x81,0xBF,0x1F,0x00,0x4F,0x0D,0x0E,0x00,0x00,0x00,0x00,
 0x9C,0x8E,0x8F,0x28,0x1F,0x96,0xB9,0xA3,0xFF };
static const uint8_t s_text80[5] = { 0x03,0x00,0x03,0x00,0x02 };
static const uint8_t g_text80[9] = { 0x00,0x00,0x00,0x00,0x00,0x10,0x0E,0x00,0xFF };
static const uint8_t a_text80[21] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
 0x0C,0x00,0x0F,0x08,0x00 };

static const uint8_t c_13h[25] = {
 0x5F,0x4F,0x50,0x82,0x54,0x80,0xBF,0x1F,0x00,0x41,0x00,0x00,0x00,0x00,0x00,0x00,
 0x9C,0x8E,0x8F,0x28,0x40,0x96,0xB9,0xA3,0xFF };
static const uint8_t s_13h[5] = { 0x03,0x01,0x0F,0x00,0x0E };
static const uint8_t g_13h[9] = { 0x00,0x00,0x00,0x00,0x00,0x40,0x05,0x0F,0xFF };
static const uint8_t a_13h[21] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0A,0x0B,0x0C,0x0D,0x0E,0x0F,
 0x41,0x00,0x0F,0x00,0x00 };

static const uint8_t c_12h[25] = {
 0x5F,0x4F,0x50,0x82,0x54,0x80,0x0B,0x3E,0x00,0x40,0x00,0x00,0x00,0x00,0x00,0x00,
 0xEA,0x8C,0xDF,0x28,0x00,0xE7,0x04,0xE3,0xFF };
static const uint8_t s_12h[5] = { 0x03,0x01,0x0F,0x00,0x06 };
static const uint8_t g_12h[9] = { 0x00,0x00,0x00,0x00,0x00,0x00,0x05,0x0F,0xFF };
static const uint8_t a_12h[21] = { 0x00,0x01,0x02,0x03,0x04,0x05,0x14,0x07,0x38,0x39,0x3A,0x3B,0x3C,0x3D,0x3E,0x3F,
 0x01,0x00,0x0F,0x00,0x00 };

/* default 6-bit DAC palette (EGA 16 + grey ramp + colour cube), enough for
 * text and 16-colour modes; games in mode 13h upload their own. */
static void default_dac(void){
    static const uint8_t base[16][3] = {
        {0,0,0},{0,0,42},{0,42,0},{0,42,42},{42,0,0},{42,0,42},{42,21,0},{42,42,42},
        {21,21,21},{21,21,63},{21,63,21},{21,63,63},{63,21,21},{63,21,63},{63,63,21},{63,63,63}};
    int i;
    for(i=0;i<16;i++){ dac[i][0]=base[i][0]; dac[i][1]=base[i][1]; dac[i][2]=base[i][2]; }
    for(i=16;i<256;i++){ uint8_t v=(uint8_t)((i-16)&0x3F); dac[i][0]=v; dac[i][1]=v; dac[i][2]=v; }
}

void vga_set_mode_bios(int mode){
    bios_mode = mode;
    memset(vga_vram,0,sizeof(vga_vram));
    default_dac();
    sm_have = 0;   /* drop scroll history: old positions are meaningless now */
    switch(mode & 0x7F){
    case 0x13: apply_regs(c_13h,s_13h,g_13h,a_13h,0x63); break;
    case 0x12: apply_regs(c_12h,s_12h,g_12h,a_12h,0xE3); break;
    case 0x10: apply_regs(c_12h,s_12h,g_12h,a_12h,0xA3); break;
    default:   apply_regs(c_text80,s_text80,g_text80,a_text80,0x67);
               { int i; for(i=0;i<2000;i++){ vga_vram[i*4+0]=' '; vga_vram[i*4+1]=0x07; } }
               break;
    }
    dac_mask = 0xFF;
    vga_dirty = 1;
}

int vga_get_mode(void){ return bios_mode; }

/* ---------------------------------------------------------- CRT timing  */
/* Everything the game's frame pacing depends on comes out of these
 * registers, so compute it rather than assuming 70 Hz: mode 13h and text
 * are 70 Hz (449-line total), the 480-line mode-X timing is 60 Hz. */
void vga_timing(double *frame_period, int *vtotal_out, int *vde_out,
                int *vrs_out, int *vre_out, double *hde_frac){
    double dotclk = ((misc_out >> 2) & 3) == 1 ? 28322000.0 : 25175000.0;
    int dots_per_char = (sq[1] & 0x01) ? 8 : 9;
    int htotal = cr[0x00] + 5;
    int hde    = cr[0x01] + 1;
    int vtotal = (cr[0x06] | (((cr[0x07]>>0)&1)<<8) | (((cr[0x07]>>5)&1)<<9)) + 2;
    int vrs    =  cr[0x10] | (((cr[0x07]>>2)&1)<<8) | (((cr[0x07]>>7)&1)<<9);
    int vre    = (vrs & ~0x0F) | (cr[0x11] & 0x0F);
    double dots;
    if(sq[1] & 0x08) dotclk /= 2.0;                 /* dot clock / 2 */
    if(htotal < 10) htotal = 100;
    if(vtotal < 10) vtotal = 449;
    if(vre <= vrs) vre = vrs + 2;
    dots = (double)htotal * dots_per_char;
    *frame_period = dots * (double)vtotal / dotclk;
    *vtotal_out = vtotal;
    *vde_out = vde();
    *vrs_out = vrs; *vre_out = vre;
    *hde_frac = (double)hde / (double)htotal;
}

/* Cached copy of the above.  vga_status1() runs once per guest `in al,3DAh`
 * - tens of millions of times per second in the game's sync loops - and the
 * registers it derives from only change on mode set / CRTC reprogramming.
 * Recomputing the doubles every read dominated the profile, so refresh the
 * cache only when those registers are written (see vga_io_w/apply_regs). */
void vga_timing_cached(double *frame_period, double *inv_period,
                int *vtotal_out, int *vde_out,
                int *vrs_out, int *vre_out, double *hde_frac){
    if(timing_dirty){
        vga_timing(&timing_per, &timing_vtotal, &timing_vde,
                   &timing_vrs, &timing_vre, &timing_hde);
        if(timing_per <= 0.0) timing_per = 1.0/70.0;
        timing_inv_per = 1.0 / timing_per;
        timing_dirty = 0;
    }
    *frame_period = timing_per;
    *inv_period = timing_inv_per;
    *vtotal_out = timing_vtotal;
    *vde_out = timing_vde;
    *vrs_out = timing_vrs; *vre_out = timing_vre;
    *hde_frac = timing_hde;
}

void vga_dump(void){
    int i;
    printf("[vga] misc=%02X  seq:", misc_out);
    for(i=0;i<5;i++) printf(" %d=%02X", i, sq[i]);
    printf("\n[vga] crtc:");
    for(i=0;i<0x19;i++) printf(" %02X=%02X", i, cr[i]);
    printf("\n[vga] gc:");
    for(i=0;i<9;i++) printf(" %d=%02X", i, gc[i]);
    { long vnz=0; int i2; for(i2=0;i2<256*1024;i2++) if(vga_vram[i2]) vnz++;
      printf("\n[vga] vram: %ld non-zero bytes of 262144", vnz); }
    { int nz=0, mx=0;
      for(i=0;i<256;i++){ int s=dac[i][0]+dac[i][1]+dac[i][2]; if(s) nz++; if(s>mx) mx=s; }
      printf("\n[vga] dac: %d non-black entries, brightest sum=%d", nz, mx); }
    printf("\n[vga] ar:");
    for(i=0;i<0x15;i++) printf(" %02X=%02X", i, ar[i]);
    printf("\n[vga] last 3C0 writes (ff=0 means index):");
    for(i = (arlog_n>40?arlog_n-40:0); i<arlog_n; i++)
        printf(" %s%02X", arlog[i&255].ff?"=":"", arlog[i&255].val);
    printf("\n[vga] attr: mode=%02X pel=%02X  vde=%d maxscan=%d offs=%d start=%04X lc=%d\n",
           ar[0x10], ar[0x13], vde(), (cr[0x09]&0x1F)+1, cr[0x13],
           (cr[0x0C]<<8)|cr[0x0D], line_compare());
}

void vga_init(void){
    memset(sq,0,sizeof(sq)); memset(gc,0,sizeof(gc));
    memset(cr,0,sizeof(cr)); memset(ar,0,sizeof(ar));
    vga_set_mode_bios(3);
}

/* --------------------------------------------------- plain RAM interface */
#define VGA_LO 0xA0000u
#define VGA_HI 0xC0000u

uint8_t mem_r8(uint32_t a){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a >= VGA_LO && a < VGA_HI) return vga_mem_r(a);
    return ram[a];
}
uint16_t mem_r16(uint32_t a){
    /* One range check + one direct read.  The old version paid for two full
     * mem_r8 calls (mask, branch, call) per 16-bit access.  ROM reads come
     * straight from ram[], so only the VGA window needs the slow path. */
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 1u >= VGA_LO && a < VGA_HI) return (uint16_t)(mem_r8(a) | (mem_r8(a+1)<<8));
    return (uint16_t)(ram[a] | (ram[a+1]<<8));
}
uint32_t mem_r32(uint32_t a){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 3u >= VGA_LO && a < VGA_HI)
        return (uint32_t)mem_r8(a) | ((uint32_t)mem_r8(a+1)<<8) | ((uint32_t)mem_r8(a+2)<<16) | ((uint32_t)mem_r8(a+3)<<24);
    return (uint32_t)ram[a] | ((uint32_t)ram[a+1]<<8) | ((uint32_t)ram[a+2]<<16) | ((uint32_t)ram[a+3]<<24);
}
void mem_w8(uint32_t a, uint8_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a >= VGA_LO && a < VGA_HI){ vga_mem_w(a,v); return; }
    if(a >= 0xC0000 && a < 0x100000) return;       /* ROM */
    ram[a] = v;
}
void mem_w16(uint32_t a, uint16_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 1u >= VGA_LO && a < 0x100000){ mem_w8(a,(uint8_t)v); mem_w8(a+1,(uint8_t)(v>>8)); return; }
    ram[a]=(uint8_t)v; ram[a+1]=(uint8_t)(v>>8);
}
void mem_w32(uint32_t a, uint32_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 3u >= VGA_LO && a < 0x100000){
        mem_w8(a,(uint8_t)v); mem_w8(a+1,(uint8_t)(v>>8));
        mem_w8(a+2,(uint8_t)(v>>16)); mem_w8(a+3,(uint8_t)(v>>24)); return; }
    ram[a]=(uint8_t)v; ram[a+1]=(uint8_t)(v>>8); ram[a+2]=(uint8_t)(v>>16); ram[a+3]=(uint8_t)(v>>24);
}
