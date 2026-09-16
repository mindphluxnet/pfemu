/* pfemu - a small 386 real-mode PC emulator, written to run Pinball Fantasies (1993)
 * natively on 64-bit Windows.  No external emulator code is used.
 */
#ifndef PFEMU_H
#define PFEMU_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#define RAM_SIZE 0x1000000u          /* 16 MB linear space (we only use <1MB) */

/* ---------------------------------------------------------------- CPU ---- */
enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
enum { S_ES, S_CS, S_SS, S_DS, S_FS, S_GS };

typedef struct {
    uint32_t regs[8];
    uint16_t sreg[6];
    uint32_t sbase[6];
    uint32_t eip;
    /* flags kept unpacked for speed; assembled on demand */
    uint32_t cf, pf, af, zf, sf, tf, iflag, df, of, nt, iopl, ac;
    int halted;
    uint64_t cycles;
    int shutdown;
} CPU;

extern CPU cpu;
extern uint8_t *ram;

#define REG32(i)  (cpu.regs[i])
#define REG16(i)  (*(uint16_t*)&cpu.regs[i])
#define REG8(i)   (*((uint8_t*)&cpu.regs[(i)&3] + (((i)>>2)&1)))

void cpu_reset(void);
void cpu_step(void);
void cpu_run(int cycles);
void cpu_interrupt(int n, int soft);   /* push flags/cs/ip, vector through IVT */
uint32_t cpu_getflags(void);
void cpu_setflags(uint32_t f);

/* ------------------------------------------------------------- memory ---- */
uint8_t  mem_r8 (uint32_t a);
uint16_t mem_r16(uint32_t a);
uint32_t mem_r32(uint32_t a);
void     mem_w8 (uint32_t a, uint8_t v);
void     mem_w16(uint32_t a, uint16_t v);
void     mem_w32(uint32_t a, uint32_t v);
extern uint32_t a20_mask;

/* ---------------------------------------------------------------- I/O ---- */
uint8_t  io_r8 (uint16_t p);
uint16_t io_r16(uint16_t p);
void     io_w8 (uint16_t p, uint8_t v);
void     io_w16(uint16_t p, uint16_t v);

/* ------------------------------------------------------------- devices --- */
void dev_init(void);
void dev_tick(void);                  /* called from the main loop */
uint64_t dev_next_deadline(void);     /* cpu.cycles at which IRQ0 is next due */
double emu_now(void);                 /* instruction-exact emulated time */
void pic_raise(int irq);
void pic_lower(int irq);
int  pic_pending(void);               /* returns vector or -1 */
void pic_ack(int vec);
void kbd_key(int scancode, int down);
void kbd_release_all(void);     /* synthesize breaks for held keys (focus loss) */
void kbd_clear_held(void);      /* drop held-key state (game switch / fix off) */
int  kbd_held_get(unsigned idx);/* idx = scancode | (ext ? 0x80 : 0) */
extern int  kbd_a20;

/* ---------------------------------------------------------------- VGA ---- */
void vga_init(void);
uint8_t vga_mem_r(uint32_t a);
void    vga_mem_w(uint32_t a, uint8_t v);
uint8_t vga_io_r(uint16_t p);
void    vga_io_w(uint16_t p, uint8_t v);
void    vga_render(uint32_t *out, int *w, int *h);
void    vga_set_mode_bios(int mode);
extern uint8_t vga_vram[256*1024];
extern int vga_dirty;
extern int vga_flipdbg;              /* -flipdbg: log page flips w/ phase */
extern uint64_t vscan_step;          /* -vscan N: VRAM mutation timeline */
void vga_vscan_poll(void);           /* per-batch sampler, no-op unless set */
void vga_timing_cached(double*,double*,int*,int*,int*,int*,double*);
extern int vga_smooth;               /* -nosmooth disables scroll interp */
extern int vga_dmdlog;               /* -dmd: log DMD VRAM step cadence */

/* ---------------------------------------------------------------- BIOS --- */
void bios_init(void);
void bios_call(int n);                /* dispatch for INT n from a callback stub */
int  bios_kbuf_get(uint16_t *out);    /* type-ahead queue for DOS input */
int  bios_kbuf_peek(uint16_t *out);

/* ---------------------------------------------------------------- DOS ---- */
void dos_init(const char *hostdir);
void dos_int21(void);
int  dos_exec(const char *path, uint16_t psp_env, uint32_t cmdtail_ptr, uint32_t fcb1, uint32_t fcb2);
extern int dos_done;
extern int dos_no_patch;        /* -nopatch : leave manual checks in place */

/* -------------------------------------------------------- game fixes ----- */
/* Per-game behaviour lives in its own TU (src/fantasies.c, src/dreams.c);
 * dos.c/dev.c call in, never implement game logic themselves. */
void fantasies_begin_session(const char *dir, const char *prog);
void fantasies_on_exec(const char *dospath, uint16_t cs_seg);
void fantasies_patch_sdr(uint32_t load_base, uint32_t imglen);
void fantasies_filter_read(const char *fname, long pos, uint8_t *buf, int len);
void fantasies_patch_intro(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_patch_pause(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_pause_tick(void);
void fantasies_spring_tick(void);
void fantasies_patch_spring(const char *dospath, uint32_t load_base, uint32_t imglen);
int  fantasies_intercept_cfg_open(const char *fname);
int  fantasies_fix_active(void);
int  fantasies_session_armed(void);
void fantasies_key_event(int scancode, int down);
void fantasies_draw_osd(uint32_t *fb, int w, int h);
void dreams_patch_image(uint32_t load_base, uint32_t imglen);

/* ------------------------------------------------------------ platform --- */
void plat_init(const char *title);
int  plat_pump(void);                 /* returns 0 when the user closes the window */
void plat_present(const uint32_t *pix, int w, int h);
double plat_time(void);
void plat_sleep_ms(int ms);
void plat_audio_init(int hz);
void plat_audio_push(const int16_t *samples, int count);
void plat_set_fullscreen(int on);     /* runtime toggle; also Alt+Enter in-window */

/* ------------------------------------------------------------ launcher --- */
/* Win32 game picker + sound toggle (launch.c).  The dialog writes SOUND.CFG
 * into the game's PFEMU-STATE/ overlay so installed files stay pristine. */
typedef struct { const char *dir, *prog; int fullscreen; } LaunchChoice;
int  show_launcher(LaunchChoice *out);   /* 1 = launch, 0 = quit */
void write_sound_cfg(const char *dir, int on);
int  read_sound_is_sb(const char *dir);

/* ------------------------------------------------------------ tracing ---- */
extern int trace_level;
void trc(const char *fmt, ...);

#endif
