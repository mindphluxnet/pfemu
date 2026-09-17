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
extern int vga_dmdlog;               /* -dmd: log DMD VRAM step cadence */
extern int vga_paldbg;               /* -paldbg: log AR14 writes w/ frame phase */
extern int vga_state_dump_on;        /* -vgastate: full pipeline dump at exit */
void cpu_dump_segment(uint16_t seg, const char *why);
extern int dump_seg_on;              /* -dumpseg SEG: dump that segment at exit */
extern int prof_on;                  /* -prof: sample CS:IP, report hot sites */
void prof_report(void);
void intstat_report(void);
void memwatch_report(void);
void memwatch_hit(uint32_t a, uint8_t v);
extern uint32_t memwatch_addr;
extern uint16_t dump_seg_which;
void vga_state_dump(void);
double vga_scanline_now(int *vtotal_out); /* frame-relative scan line, for -balldbg */
uint32_t vga_start_now(int *pitch_out);   /* raw CRTC start + row pitch */
extern double vga_last_start_write;       /* emu time of last CRTC 0C/0D write */
extern double vga_last_start_line;        /* frame line of that write */
extern int vga_latch_start, vga_ballsync; /* -nolatch / -noballsync */
void vga_note_ball_start(uint32_t start); /* PUTTHEBALL epilogue -> camera pairing */
void vga_reset_start_pairing(void);
uint32_t vga_displayed_start_now(void);   /* what vga_render would show */

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

/* ---------------------------------------------------------------- LZEXE -- */
/* Self-extracting programs, unpacked by the loader instead of by the guest so
 * that the image patches in src/fantasies.c have something to match.  Only
 * LZEXE 0.91 is handled, and only as an optimisation of the boot: every
 * failure here falls back to loading the packed file and letting its own stub
 * run.  See src/lzexe.c for why, and for why PKLITE is left alone. */
typedef struct {
    uint8_t *image;       /* the unpacked load image (malloc'd) */
    uint32_t imglen;
    uint16_t cs, ip, ss, sp;   /* the original entry point and stack */
    uint16_t minalloc, maxalloc;
    uint32_t *rel;        /* relocations, (segment << 16) | offset (malloc'd) */
    uint32_t nrel;
} LzexeImage;

int  lzexe_detect(const uint8_t *hdr32);
int  lzexe_load(FILE *f, long fsize, LzexeImage *out);
void lzexe_free(LzexeImage *im);
extern int dos_no_lzexe;        /* -nolzexe : never unpack, let the stub run */

/* ------------------------------------------------------------ releases --- */
/* Checksum-based release identity (src/release.c).  An installation is
 * identified by the SHA-256 of its program files, never by its directory name
 * or its boot filename - see docs/VERSIONS.md for why neither works. */
enum {
    RF_CODE    = 1,   /* part of the five-file identity vector */
    RF_BOOT    = 2,   /* the program pfemu executes to start this release */
    RF_REQ     = 4,   /* must be present for the game to run */
    RF_PREFIX  = 8,   /* hash covers only the first `prefix` bytes */
    RF_MUTABLE = 16,  /* the game or an installer rewrites it; never identity */
    RF_META    = 32   /* packaging metadata, not a runtime dependency */
};

typedef struct {
    const char *name;     /* DOS basename, matched case-insensitively */
    uint32_t size;        /* exact file size, a cheap prefilter */
    uint32_t prefix;      /* bytes covered by sha, 0 = the whole file */
    uint32_t flags;
    uint8_t  sha[32];
} RelFile;

/* Which programs a distribution ships, and under which names.  The full game
 * ships an intro and four tables; the 1993 five-minute demo ships an intro
 * and one table, both renamed - so the identity vector is a property of the
 * distribution's shape, not a constant.  names[0] is the anchor: the program
 * whose presence says which shape a directory holds, and whose hash decides
 * which release it is.  The rest are the tables, in table order. */
typedef struct {
    const char *id;           /* "full", "demo" - for the detection report */
    const char *names[5];
    int n;
} CodeLayout;

typedef struct {
    const char *id;       /* stable id: "floppy", "power_pack", "deluxe" */
    const char *label;    /* user-facing: "Pinball Power Pack (1996)" */
    const char *boot;     /* boot program basename for this release */
    const CodeLayout *layout;  /* the programs this release ships */
    uint16_t cfg_buf;     /* intro's six-byte options structure, DS offset;
                           * 0 = this build has no options structure at all */
    uint8_t scroll_clobber; /* intro defaults Scrolling alone after a failed load */
    uint8_t opt_validate;   /* intro re-defaults all six after a failed load */
    uint8_t cd_marker;      /* boot program checks for cd.nfo (Deluxe CD-ROM) */
    const RelFile *files;   /* the complete collected top-level manifest */
    int nfiles;
} Release;

typedef enum {
    REL_NONE = 0,
    REL_RECOGNIZED,   /* known code vector and a valid boot program */
    REL_INCOMPLETE,   /* known release, required file missing */
    REL_MODIFIED,     /* known intro, but an expected hash differs */
    REL_MIXED,        /* programs independently match different releases */
    REL_UNKNOWN,      /* the anchor program is not in the database */
    REL_AMBIGUOUS,    /* names differing only by case */
    REL_ABSENT        /* no program of any known layout here at all */
} RelState;

typedef struct {
    RelState state;
    const Release *rel;   /* non-NULL once the anchor's hash is recognised */
    char dir[512];        /* directory as given (relative stays relative) */
    char full[512];       /* ...and its absolute form, for the report */
    char boot[16];        /* boot program as actually spelled on disk */
    int  odd;             /* required data files present but not this release's */
    char summary[160];    /* one line, for the launcher */
    char detail[8192];    /* the copyable report */
    /* The code identity vector behind the verdict above: the layout's
     * programs in layout order, with the SHA-256/size release.c matched on.
     * Filled for every recognised-or-better verdict (and for unknown/mixed
     * as far as the files present allow); the replay header records this
     * vector verbatim so replay can refuse a different copy (docs/REPLAY.md
     * section 3.1 - identity is release_id + hash vector, never the
     * directory name). */
    int  ncode;
    char code_names[5][16];
    uint8_t code_sha[5][32];
    uint32_t code_size[5];
    int  code_have[5];
} RelResult;

/* Where a release keeps its program: 0 = the intro, 1..4 = that table,
 * -1 = neither.  Resolves the renames in the demo's layout, so nothing
 * outside src/release.c has to know a program filename. */
int  release_prog_slot(const Release *r, const char *base);

int  release_detect(const char *dir, RelResult *out);
int  release_scan(RelResult *out, int max);   /* GAME first, then any install */
int  release_runnable(const RelResult *r);
const char *release_state_name(RelState s);
const Release *release_by_id(const char *id);
const Release *release_at(int i);
int  release_count(void);

/* -------------------------------------------------------- game fixes ----- */
/* Per-game behaviour lives in src/fantasies.c;
 * dos.c/dev.c call in, never implement game logic themselves. */
/* rel is the detected release (src/release.c); NULL means no recognised
 * Pinball Fantasies installation, and nothing Fantasies-specific arms. */
void fantasies_begin_session(const char *dir, const char *prog, const Release *rel);
void fantasies_on_exec(const char *dospath);
void fantasies_patch_sdr(uint32_t load_base, uint32_t imglen);
void fantasies_filter_read(const char *fname, long pos, uint8_t *buf, int len);
void fantasies_patch_intro(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_patch_pause(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_pause_tick(void);
void fantasies_spring_tick(void);
void fantasies_patch_spring(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_patch_balls(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_patch_ballgap(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_ballgap_exec(uint32_t lin);   /* cpu.c hook, gated by balldbg_on */
void fantasies_ballgap_present(int fallback); /* main.c: one call per present */
void fantasies_ballgap_report(void);         /* exit summary */
int  fantasies_present_window(double *lo, double *hi); /* 1 once learned */
void fantasies_find_matrix(const char *dospath, uint32_t load_base, uint32_t imglen);
void fantasies_matrix_exec(uint32_t lin);    /* cpu.c hook, gated by mat_dbg */
void fantasies_matrix_report(void);          /* exit summary */
extern int mat_dbg;                          /* -matdbg */
extern uint32_t mat_tick_site, mat_call_site, mat_crisis_site; /* 0 = unknown */
extern int balldbg_on;                       /* -balldbg */
extern uint32_t balldbg_entry, balldbg_exit; /* PUTTHEBALL entry / RETN, 0 = unknown */
extern uint32_t balldbg_pos;                 /* the MOV [OLDPOS],SI store */
int  fantasies_intercept_cfg_open(const char *fname);
FILE *fantasies_open_cdmarker(const char *fname);
int  fantasies_fix_active(void);
int  fantasies_session_armed(void);
void fantasies_key_event(int scancode, int down);

/* ------------------------------------------------------------ platform --- */
void plat_init(const char *title);
int  plat_pump(void);                 /* returns 0 when the user closes the window */
void plat_present(const uint32_t *pix, int w, int h);
double plat_time(void);
void plat_sleep_ms(int ms);
void plat_audio_init(int hz);
void plat_audio_push(const int16_t *samples, int count);
void plat_set_fullscreen(int on);     /* runtime toggle; also Alt+Enter in-window */

/* Host-only on-screen message, drawn over the presented frame (src/main.c).
 * Used by the volume keys and by the trainer's hotkeys in src/fantasies.c. */
void osd_show(const char *text);
void osd_clear(void);

/* Host output gain, 0-100 (src/sound.c).  Set from the launcher's slider, the
 * -vol switch, or the -/+ keys in the game window.  100 is the level every
 * build before this one played at; the default is lower because a 1992 SB
 * expected a knob on the card and another on the speakers, and there is no
 * knob anywhere in here. */
#define AUDIO_VOLUME_DEFAULT 70
extern int audio_volume;
extern int audio_volume_dirty;   /* the -/+ keys moved it; save it on exit */

/* ------------------------------------------------------------ launcher --- */
/* Win32 installation picker + sound toggle (launch.c).  The dialog writes SOUND.CFG
 * into the game's PFEMU-STATE/ overlay so installed files stay pristine. */
typedef enum { LAUNCH_PLAY = 0, LAUNCH_RECORD, LAUNCH_REPLAY } LaunchMode;
typedef struct {
    char dir[512], prog[16];
    int fullscreen;
    LaunchMode mode;            /* play | record | replay (docs/REPLAY.md section 4) */
    char replay_path[512];      /* -record target / -replay source, "" when play */
} LaunchChoice;
int  show_launcher(LaunchChoice *out);   /* 1 = launch, 0 = quit */
void write_sound_cfg(const char *dir, int on, int quality);
int  read_sound_is_sb(const char *dir);
int  read_sound_quality(const char *dir);      /* SOUND.CFG byte 14h, 0-4 */
int  read_volume_cfg(const char *dir);         /* host-only file, 0-100 */
void write_volume_cfg(const char *dir, int vol); /* keeps the stored quality */

/* ------------------------------------------------- session record/replay */
/* Deterministic input recording (docs/REPLAY.md).  v1 records from the boot
 * program including table-select, replays on emulated time, and never merges
 * live keys; direct-into-table replay is refused until the section 3.4
 * validation passes.  CLI (-record/-replay) and the launcher are thin
 * frontends over the same emu-time injector (src/replay.c). */
int  replay_is_recording(void);
int  replay_is_replaying(void);
/* Parsed .pfr header, for the launcher's auto-restore (section 4.1) and for
 * main()'s install verification.  Identity fields mirror RelResult's code
 * vector above; summary/dir_hint are display/hint only, never matched on. */
typedef struct {
    char release_id[32];
    char summary[160];
    char boot[16];
    char program[16];
    char layout[16];
    int  ncode;
    char names[5][16];
    uint8_t sha[5][32];
    uint32_t size[5];
    int  have[5];
    double ips;
    double speed;
    int  nopatch, nolzexe;
    int  sound;
    int  quality;
    uint8_t options[6];
    int  fullscreen;
    int  trainer_off;
    char overlay[32];
    char dir_hint[512];
    int  nevents;
} ReplayHeader;
int  replay_read_header(const char *path, ReplayHeader *out);
const char *replay_parse_error(void);   /* last parse failure, for fail_msg */
void replay_header_detail(const ReplayHeader *h, char *dst, size_t n);
/* Record side: open with the full session context, log every kbd_key entry
 * with emu_now() (dev.c calls in), close with the footer at exit. */
int  replay_begin_record(const char *path, const RelResult *rel, const char *prog,
                         double ips, int nopatch, int nolzexe,
                         int sound, int quality, const uint8_t options[6],
                         int fullscreen);
void replay_log_key(int scancode, int down);
void replay_end_record(void);
/* Replay side: parse (header + sorted event list), verify against the
 * detected install, then force the recorded environment and inject. */
int  replay_begin_replay(const char *path);
/* 0 = the install IS the recording; else -1 with the reason in why (the
 * caller shows it - main() has no console in launcher flows). */
int  replay_verify_install(const RelResult *rel, const char *prog,
                           char *why, size_t nwhy);
void replay_apply_recorded_env(void);   /* ips/nopatch/nolzexe/time-freeze */
/* The recorded session settings (valid once a replay header parsed).
 * Quality/options change guest execution, so replay runs these, not the
 * install's current values; fullscreen is host-only but travels too. */
int  replay_recorded_fullscreen(void);
int  replay_recorded_quality(int *have);
int  replay_recorded_options(uint8_t out[6]);
/* After overlay isolation: patch the recorded quality notch into the temp
 * copy's SOUND.CFG, so the guest driver mixes exactly as recorded. */
void replay_apply_config_to_overlay(void);
double replay_forced_ips(int *have);    /* recorded ips, when replaying */
double replay_forced_speed(int *have);  /* recorded speed, when replaying */
void replay_inject_due(void);           /* kbd_key() everything <= emu_now() */
uint64_t replay_next_deadline(void);    /* cpu.cycles of next event, or ~0 */
int  replay_events_pending(void);
int  replay_should_stop(void);          /* footer emu_time reached, events out */
void replay_report(void);               /* exit mismatch stats */
/* PFEMU-STATE handling (section 3.2/3.3): hash the effective overlay for the
 * header; on replay, copy it aside and remap the DOS writedir so the user's
 * real overlay is never written. */
void replay_read_options(const char *dir, uint8_t out[6]);
void replay_overlay_hash(const char *dir, char out[32]);
void replay_isolate_overlay(const char *dir);
void replay_cleanup_overlay(void);
void replay_frozen_datetime(int *year, int *mon, int *day, int *wday,
                            int *hour, int *min, int *sec);
void replay_frozen_dos_dt(uint16_t *dosdate, uint16_t *dostime);
/* Trainer invariant (sections 3.2/3.3): recording/replay require it off. */
int  fantasies_trainer_enabled(void);
/* DOS time virtualization (section 2.3): freeze INT 21h AH=2Ah/2Ch and file
 * timestamps to the recorded epoch on replay. */
void dos_set_time_frozen(int on);
void dos_remap_writedir(const char *dir);
void dos_close_all_handles(void);

/* ------------------------------------------------------------- imaging --- */
int save_png(const char *path, const uint32_t *pix, int w, int h); /* src/png.c */

/* ------------------------------------------------------------ tracing ---- */
extern int trace_level;
void trc(const char *fmt, ...);

#endif
