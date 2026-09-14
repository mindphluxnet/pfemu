/* 8259 PIC, 8253 PIT, keyboard controller, port dispatch, display timing */
#include "pfemu.h"
#include <math.h>

double emu_time = 0.0;            /* emulated seconds since boot */
double emu_ips  = 6000000.0;      /* emulated instructions per second */
static uint64_t last_cycles = 0;

void emu_advance(void){
    uint64_t d = cpu.cycles - last_cycles;
    last_cycles = cpu.cycles;
    emu_time += (double)d / emu_ips;
}

/* emu_time is only folded forward every N instructions by the main loop, but
 * anything a program polls in a tight loop - the CRT status register, the PIT
 * counters, the port-61h refresh bit - has to be exact to the instruction, or
 * short pulses vanish between samples.  The sound driver phase-locks the timer
 * by counting horizontal blanking pulses that are only ~6 us long, so this
 * matters. */
double emu_now(void){
    return emu_time + (double)(cpu.cycles - last_cycles) / emu_ips;
}

/* ------------------------------------------------------------------ PIC */
typedef struct { uint8_t imr, irr, isr, base, icw_step, icw4, auto_eoi, read_isr; } PIC8259;
static PIC8259 pic[2];

static void pic_init(void){
    memset(pic,0,sizeof(pic));
    pic[0].base = 0x08; pic[1].base = 0x70;
    pic[0].imr = 0xB8; pic[1].imr = 0xFF;   /* as BIOS POST leaves them: IRQ0/1/2/6 on */
}
void pic_raise(int irq){
    if(irq<8) pic[0].irr |= (uint8_t)(1<<irq);
    else { pic[1].irr |= (uint8_t)(1<<(irq-8)); pic[0].irr |= 0x04; }
}
void pic_lower(int irq){
    if(irq<8) pic[0].irr &= (uint8_t)~(1<<irq);
    else pic[1].irr &= (uint8_t)~(1<<(irq-8));
}
int pic_pending(void){
    int i;
    uint8_t act = pic[0].irr & ~pic[0].imr;
    if(!act) return -1;
    for(i=0;i<8;i++){
        if(pic[0].isr & (1<<i)) return -1;          /* higher/equal in service */
        if(act & (1<<i)){
            if(i==2){
                uint8_t s = pic[1].irr & ~pic[1].imr;
                int j;
                for(j=0;j<8;j++) if(s & (1<<j)){
                    pic[1].irr &= (uint8_t)~(1<<j); pic[1].isr |= (uint8_t)(1<<j);
                    pic[0].isr |= 0x04; pic[0].irr &= (uint8_t)~0x04;
                    return pic[1].base + j;
                }
                pic[0].irr &= (uint8_t)~0x04;
                return -1;
            }
            pic[0].irr &= (uint8_t)~(1<<i);
            pic[0].isr |= (uint8_t)(1<<i);
            return pic[0].base + i;
        }
    }
    return -1;
}
static void pic_write(int n, int a0, uint8_t v){
    PIC8259 *p = &pic[n];
    if(a0==0){
        if(v & 0x10){ p->icw_step = 1; p->imr = 0; p->isr = 0; p->irr = 0; p->icw4 = v&1; return; }
        if(v & 0x08){ if(v & 0x02) p->read_isr = v & 1; return; }   /* OCW3 */
        if((v & 0xE0) == 0x20){                                     /* EOI */
            int i;
            if(v & 0x40){ p->isr &= (uint8_t)~(1 << (v&7)); }
            else for(i=0;i<8;i++) if(p->isr & (1<<i)){ p->isr &= (uint8_t)~(1<<i); break; }
        }
        return;
    }
    if(p->icw_step==1){ p->base = v & 0xF8; p->icw_step=2; return; }
    if(p->icw_step==2){ p->icw_step = p->icw4 ? 3 : 0; return; }
    if(p->icw_step==3){ p->icw_step = 0; return; }
    p->imr = v;
}
static uint8_t pic_read(int n, int a0){
    PIC8259 *p = &pic[n];
    if(a0==0) return p->read_isr ? p->isr : p->irr;
    return p->imr;
}

extern void vga_timing(double*,int*,int*,int*,int*,double*);

/* ------------------------------------------------------------------ PIT */
#define PIT_HZ 1193182.0
typedef struct {
    uint16_t reload; uint8_t mode, rw, latched_cnt_valid;
    uint16_t latch; int rd_hi, wr_hi; uint16_t wr_tmp;
    double next_irq;
    int armed;              /* mode 0: one interrupt per count written */
} PITCH;
static PITCH pit[3];
static uint8_t port61 = 0x00;
int pll_dbg = 0;                  /* -pll N : trace N timer-0 reloads */

static void pit_init(void){
    int i;
    memset(pit,0,sizeof(pit));
    for(i=0;i<3;i++){ pit[i].reload = 0; pit[i].rw = 3; pit[i].mode = 3; }
    pit[0].next_irq = 65536.0 / PIT_HZ;
}
static uint16_t pit_count(int c){
    double per = (pit[c].reload ? pit[c].reload : 65536) / PIT_HZ;
    double ph = fmod(emu_now(), per) / per;
    return (uint16_t)((1.0 - ph) * (pit[c].reload ? pit[c].reload : 65536));
}
static void pit_write(int port, uint8_t v){
    if(port==3){
        int c = v>>6;
        if(c==3) return;
        if(((v>>4)&3)==0){ pit[c].latch = pit_count(c); pit[c].latched_cnt_valid=1; pit[c].rd_hi=0; return; }
        pit[c].rw = (v>>4)&3; pit[c].mode = (v>>1)&7; pit[c].wr_hi = 0;
        return;
    }
    {
        int c = port;
        uint16_t nv = pit[c].reload;
        if(pit[c].rw==1) nv = v;
        else if(pit[c].rw==2) nv = (uint16_t)(v<<8);
        else {
            if(!pit[c].wr_hi){ pit[c].wr_tmp = v; pit[c].wr_hi = 1; return; }
            nv = (uint16_t)(pit[c].wr_tmp | (v<<8)); pit[c].wr_hi = 0;
        }
        pit[c].reload = nv;
        if(c==0){
            /* Instruction-exact, not emu_time: the sound driver's PLL starts a
             * one-shot here and counts CRT blanking pulses until it fires, so a
             * start time quantised to the 64-instruction tick would jitter the
             * count by a third of a scanline and the loop could never settle. */
            double per = (nv ? nv : 65536) / PIT_HZ;
            pit[0].next_irq = emu_now() + per;
            pit[0].armed = 1;
            if(pll_dbg && pll_dbg-- > 0){
                double p2, hf2; int vt2, vd2, vr2, ve2;
                vga_timing(&p2,&vt2,&vd2,&vr2,&ve2,&hf2);
                fprintf(stderr, "[pll] reload=%u mode=%u  di=%04X bp=%04X cx=%04X t=%.6f"
                        "  vtotal=%d vde=%d hz=%.2f\n",
                        nv, pit[0].mode, REG16(R_EDI), REG16(R_EBP), REG16(R_ECX), emu_now(),
                        vt2, vd2, p2>0?1.0/p2:0.0);
            }
        }
    }
}
static uint8_t pit_read(int c){
    uint16_t v;
    if(pit[c].latched_cnt_valid) v = pit[c].latch; else v = pit_count(c);
    if(pit[c].rw==1) { pit[c].latched_cnt_valid=0; return (uint8_t)v; }
    if(pit[c].rw==2) { pit[c].latched_cnt_valid=0; return (uint8_t)(v>>8); }
    if(!pit[c].rd_hi){ pit[c].rd_hi=1; return (uint8_t)v; }
    pit[c].rd_hi=0; pit[c].latched_cnt_valid=0; return (uint8_t)(v>>8);
}

/* -------------------------------------------------------------- keyboard */
static uint8_t kbd_buf[64];
static int kbd_head, kbd_tail;
static uint8_t kbd_last = 0;
int kbd_a20 = 1;
static uint8_t kbc_cmd = 0;

void kbd_key(int scancode, int down){
    uint8_t sc = (uint8_t)(scancode & 0x7F);
    int n = (kbd_tail+1) & 63;
    if(scancode & 0xE000) {  /* extended */
        if(n!=kbd_head){ kbd_buf[kbd_tail]=0xE0; kbd_tail=n; n=(kbd_tail+1)&63; }
    }
    if(n==kbd_head) return;
    kbd_buf[kbd_tail] = (uint8_t)(down ? sc : (sc|0x80));
    kbd_tail = n;
    pic_raise(1);
}
unsigned long kbd_port60_reads = 0;
uint8_t pic_imr(void){ return pic[0].imr; }
static uint8_t kbd_read60(void){
    kbd_port60_reads++;
    trc("[kbd] port60 read by %04X:%04X (IVT9=%08X)\n", cpu.sreg[S_CS], (unsigned)cpu.eip, mem_r32(9*4));
    if(kbd_head!=kbd_tail){
        kbd_last = kbd_buf[kbd_head];
        kbd_head = (kbd_head+1)&63;
    }
    if(kbd_head!=kbd_tail) pic_raise(1); else pic_lower(1);
    return kbd_last;
}
static uint8_t kbd_status(void){
    return (uint8_t)(0x14 | (kbd_head!=kbd_tail ? 0x01 : 0x00));
}

/* ---------------------------------------------------------- CRT timing  */

/* tiny histogram of who reads the status register, for profiling */
static uint32_t st1_site[16]; static unsigned long st1_hits[16];
static void st1_note(uint32_t lin){
    int i;
    for(i=0;i<16;i++){
        if(st1_hits[i] && st1_site[i]==lin){ st1_hits[i]++; return; }
        if(!st1_hits[i]){ st1_site[i]=lin; st1_hits[i]=1; return; }
    }
    for(i=0;i<16;i++) st1_hits[i] >>= 1;    /* age the table out */
}
void st1_report(void){
    int i;
    for(i=0;i<16;i++) if(st1_hits[i])
        printf("[pfemu]   3DA reader @ linear %05X : %lu\n",
               (unsigned)st1_site[i], st1_hits[i]);
}

int vga_old_timing = 0;
uint8_t vga_status1(void){
    double per, hde_frac;
    int vtotal, vde, vrs, vre;
    double line, frac;
    uint8_t st = 0;
    if(vga_old_timing){
        extern unsigned long st1_calls, st1_bit0, st1_bit3;
        double p = 1.0/70.086;
        double t = fmod(emu_now(), p) / p;
        double l = t * 449.0;
        uint8_t s = 0;
        if(t >= 0.92 && t < 0.99) s |= 0x08;
        if((l - floor(l)) > 0.82 || t >= 0.90) s |= 0x01;
        st1_calls++; if(s&1) st1_bit0++; if(s&8) st1_bit3++;
        st1_note((uint32_t)cpu.sbase[S_CS] + cpu.eip);
        return s;
    }
    vga_timing(&per, &vtotal, &vde, &vrs, &vre, &hde_frac);
    if(per <= 0.0) per = 1.0/70.0;
    line = fmod(emu_now(), per) / per * (double)vtotal;
    frac = line - floor(line);
    if(line >= vrs && line < vre) st |= 0x08;              /* vertical retrace */
    /* Bit 0 pulses once per scan line, for the whole frame.
     *
     * The obvious reading - "set whenever the display is not active", i.e. also
     * for the whole vertical blanking interval - is wrong for our purposes, and
     * the sound driver proves it.  Its calibration loop (NOTES.md #10) counts
     * 0->1 edges of this bit between starting a PIT one-shot and its interrupt,
     * and tunes the PIT reload until the count is exactly its target.  That only
     * terminates if the count is a *strictly monotone* function of the reload.
     * Any stretch where the bit stops pulsing - the 47 blanked lines, or even
     * just the 2-3 lines of vertical retrace - puts a flat step in that curve,
     * and the loop walks across it 1 tick at a time and gives up first.  With
     * the bit following horizontal retrace alone the curve is a clean staircase
     * of one line per step and the loop locks in a handful of rounds.  So the
     * driver is using this bit as a scan-line clock, and the hardware it was
     * written for must keep it running through vertical blanking. */
    if(frac >= hde_frac) st |= 0x01;
    (void)vde; (void)vrs; (void)vre;
    { extern unsigned long vsync_edges, st1_calls, st1_bit0, st1_bit3;
      static int prev = 0;
      int now = (st>>3)&1;
      st1_calls++;
      if(st & 1) st1_bit0++;
      if(st & 8) st1_bit3++;
      st1_note((uint32_t)cpu.sbase[S_CS] + cpu.eip);
      if(now && !prev) vsync_edges++;
      prev = now; }
    return st;
}
unsigned long vsync_edges = 0, st1_calls = 0, st1_bit0 = 0, st1_bit3 = 0;
double vga_frame_hz(void){
    double per, hde_frac; int a,b,c,d;
    vga_timing(&per,&a,&b,&c,&d,&hde_frac);
    return per > 0 ? 1.0/per : 0.0;
}

/* ---------------------------------------------------------- sound stubs  */
extern void opl_write(int reg, uint8_t v);
extern void dma_write(uint16_t p, uint8_t v);
extern uint8_t dma_read(uint16_t p);
extern void sb_tick(void);
extern uint8_t opl_status(void);
extern void sb_write(uint16_t p, uint8_t v);
extern uint8_t sb_read(uint16_t p);
extern void spk_update(int on, uint16_t div);

/* ------------------------------------------------------------ dispatch  */
static uint8_t cmos_idx = 0;
static uint8_t adlib_idx = 0;

/* -iotrace N: log the first N accesses to the sound-related hardware, so the
 * driver can be watched programming it rather than guessed at. */
int io_trace = 0;
static int io_watched(uint16_t p){
    return p < 0x10 || (p >= 0x80 && p <= 0x8F) ||
           (p >= 0x220 && p <= 0x22F) || p == 0x388 || p == 0x389;
}
static void io_note(const char *rw, uint16_t p, uint8_t v){
    if(io_trace > 0 && io_watched(p)){
        io_trace--;
        fprintf(stderr, "[io] %s %03X = %02X   from %04X:%04X\n", rw, p, v,
                cpu.sreg[S_CS], (unsigned)cpu.eip);
    }
}

uint8_t io_r8(uint16_t p){
    switch(p){
    case 0x20: case 0x21: return pic_read(0, p&1);
    case 0xA0: case 0xA1: return pic_read(1, p&1);
    case 0x40: case 0x41: case 0x42: return pit_read(p&3);
    case 0x43: return 0xFF;
    case 0x60: return kbd_read60();
    case 0x61: {
        /* bit4: RAM refresh toggle (~15.09 kHz), bit5: timer-2 output */
        double t = emu_now() * 15085.0;
        uint8_t r = (uint8_t)(port61 & 0x0F);
        if(((uint64_t)t) & 1) r |= 0x10;
        {
            double per = (pit[2].reload ? pit[2].reload : 65536) / PIT_HZ;
            if(fmod(emu_now(), per)/per < 0.5) r |= 0x20;
        }
        return r; }
    case 0x64: return kbd_status();
    case 0x70: return cmos_idx;
    case 0x71: return 0;
    case 0x92: return (uint8_t)(kbd_a20 ? 0x02 : 0x00);
    case 0x201: return 0xF0;                    /* game port: nothing attached */
    case 0x388: case 0x389: return opl_status();
    }
    if(p < 0x10 || p==0x81 || p==0x82 || p==0x83 || p==0x87){
        uint8_t v = dma_read(p); io_note("rd", p, v); return v;
    }
    if(p>=0x3B0 && p<=0x3DF) return vga_io_r(p);
    if(p>=0x220 && p<=0x22F){ uint8_t v = sb_read(p); io_note("rd", p, v); return v; }
    if(io_trace) io_note("rd", p, 0xFF);
    return 0xFF;
}

void io_w8(uint16_t p, uint8_t v){
    io_note("wr", p, v);
    switch(p){
    case 0x20: case 0x21: pic_write(0, p&1, v); return;
    case 0xA0: case 0xA1: pic_write(1, p&1, v); return;
    case 0x40: case 0x41: case 0x42: pit_write(p&3, v); return;
    case 0x43: pit_write(3, v); return;
    case 0x61:
        port61 = v;
        spk_update((v&3)==3, pit[2].reload);
        return;
    case 0x64:
        kbc_cmd = v;
        if(v==0xD1) { }
        if(v==0xFE) cpu.shutdown = 1;
        return;
    case 0x70: cmos_idx = v; return;
    case 0x71: return;
    case 0x92: kbd_a20 = (v&2)?1:0; a20_mask = kbd_a20 ? 0xFFFFFFFFu : 0xFFEFFFFFu; return;
    case 0x201: return;
    case 0x388: adlib_idx = v; return;
    case 0x389: opl_write(adlib_idx, v); return;
    }
    if(p < 0x10 || p==0x81 || p==0x82 || p==0x83 || p==0x87){ dma_write(p,v); return; }
    if(p>=0x3B0 && p<=0x3DF){ vga_io_w(p,v); return; }
    if(p>=0x220 && p<=0x22F){ sb_write(p,v); return; }
    if(p==0x60){
        if(kbc_cmd==0xD1){ kbd_a20 = (v&2)?1:0; a20_mask = kbd_a20?0xFFFFFFFFu:0xFFEFFFFFu; kbc_cmd=0; }
        return;
    }
}

uint16_t io_r16(uint16_t p){ return (uint16_t)(io_r8(p) | (io_r8((uint16_t)(p+1))<<8)); }
void io_w16(uint16_t p, uint16_t v){ io_w8(p,(uint8_t)v); io_w8((uint16_t)(p+1),(uint8_t)(v>>8)); }

/* ---------------------------------------------------------------- tick  */
void dev_tick(void){
    emu_advance();
    sb_tick();
    /* PIT channel 0 -> IRQ0.
     *
     * Mode 0 is "interrupt on terminal count": ONE interrupt per count written,
     * and then the output stays high until the count is loaded again.  Treating
     * it as periodic (as the rate-generator modes 2 and 3 are) is not a harmless
     * approximation - the sound driver calibrates with a mode-0 one-shot, and
     * the extra interrupts arrive after it has put its real handler back but
     * before it has finished initialising the event list that handler walks.
     * It then runs off the end of the list into a null callback. */
    if(pit[0].mode == 0){
        if(pit[0].armed && emu_time >= pit[0].next_irq){
            pit[0].armed = 0;
            pic_raise(0);
        }
    } else {
        double per = (pit[0].reload ? pit[0].reload : 65536) / PIT_HZ;
        if(emu_time >= pit[0].next_irq){
            if(emu_time - pit[0].next_irq > 0.25) pit[0].next_irq = emu_time;  /* resync */
            pit[0].next_irq += per;
            pic_raise(0);
        }
    }
}

/* Cycle count at which the next device event (IRQ0) is due, so the main loop
 * can stop exactly there instead of overshooting by up to a whole batch. */
uint64_t dev_next_deadline(void){
    double dt;
    if(pit[0].mode == 0 && !pit[0].armed) return cpu.cycles + 64;
    dt = pit[0].next_irq - emu_now();
    if(dt <= 0.0) return cpu.cycles;
    if(dt > 1.0) dt = 1.0;
    return cpu.cycles + (uint64_t)(dt * emu_ips);
}

void dev_state_dump(void){
    printf("[pit] ch0 reload=%u mode=%u rw=%u next_irq=%.6f now=%.6f\n",
           pit[0].reload, pit[0].mode, pit[0].rw, pit[0].next_irq, emu_now());
    printf("[pic] m: imr=%02X irr=%02X isr=%02X base=%02X   s: imr=%02X irr=%02X isr=%02X\n",
           pic[0].imr, pic[0].irr, pic[0].isr, pic[0].base,
           pic[1].imr, pic[1].irr, pic[1].isr);
    printf("[ivt] int8=%08X int9=%08X int1C=%08X\n",
           mem_r32(8*4), mem_r32(9*4), mem_r32(0x1C*4));
}

void dev_init(void){
    extern void sound_init(void);
    pic_init();
    pit_init();
    sound_init();
    kbd_head = kbd_tail = 0;
    port61 = 0;
}
