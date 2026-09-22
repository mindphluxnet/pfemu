/* Session driver: argument parsing, machine setup, and the emulation loop.
 *
 * Split out of src/main.c so that one loop can run under two hosts - the
 * Win32 one next door, and the null host (src/host_null.c) that the headless
 * Linux build uses.  Everything in this file is portable C; every call that
 * reaches the world outside the emulated machine goes through a plat_ entry
 * point declared in pfemu.h.
 *
 * The split is what makes docs/VERIFY.md Spike B mean anything.  "Replay the
 * same .pfr on two platforms and compare the footers" is only a determinism
 * measurement if both platforms are running the same loop; if each host had
 * its own transcription of it, a mismatch would not say which of the two was
 * responsible.  Splitting rather than reimplementing keeps the emulated
 * machine, its clock and its stop conditions identical by construction, and
 * leaves the hosts differing only in what they draw and where they get the
 * time of day.
 */
#include <stdarg.h>
#include "compat.h"
#include "pfemu.h"

extern void  emu_advance(void);
extern double emu_time;
extern double emu_ips;
extern double emu_inv_ips;
extern void  dev_tick(void);
extern void  set_sreg(int s, uint16_t v);
extern int   vga_get_mode(void);
extern int   vga_force256, vga_nodbl;
extern void  kbd_key(int sc, int down);

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

/* The emulated framebuffer.  vga_render() fills it; the host only ever
 * receives a pointer to it. */
static uint32_t fb[1024*768];
static int fbw = 320, fbh = 200;

/* Raised by the host (a keypress, a window event), serviced by the loop
 * below at an instruction boundary between batches - never inside one, so
 * a snapshot taken here is exact. */
int screenshot_pending = 0;
/* Snapshot slot (src/snapshot.c): pending flags are raised in wndproc (F6
 * save / F8 load) and serviced in the main loop at an instruction
 * boundary, like screenshots.  snap_dir/snap_rel capture the detected
 * install for the slot path and the identity check. */
int snap_save_pending = 0, snap_load_pending = 0;
static char snap_dir[512] = "";
static RelResult snap_rel;
static int snap_have_rel = 0;
/* Mute state for the keypad-* toggle, kept out here because the exit path
 * needs it: see the volume keys in wndproc() and the save in main(). */
int vol_premute = -1, vol_muted = 0;

/* 1 once the launcher dialog returned Launch: refusals after that point get
 * a MessageBox as well as stderr.  A windowed process has no console, so a
 * stderr-only message looks exactly like "Launch does nothing, the emulator
 * just quits".  CLI runs (from_launcher == 0) never pop up. */
int from_launcher = 0;
static void fail_msg(const char *fmt, ...){
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(stderr, "%s\n", buf);
    plat_fail_msg(buf);
}

/* -untilemu SEC as a cycle deadline.  emu_now() and cpu.cycles are both
 * deterministic functions of the run, so this target is too - which is
 * what lets two runs stop on the same instruction.  Never returns
 * cpu.cycles itself while the target is still ahead: a zero-length
 * batch would stall the loop without ever reaching the stop. */
static uint64_t until_cycles(double t){
    double dt = t - emu_now();
    uint64_t n;
    if(dt <= 0.0) return cpu.cycles;
    n = (uint64_t)(dt * emu_ips);
    return cpu.cycles + (n ? n : 1);
}

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

/* "t:scancode:updown,..." - drive the keyboard from a script for testing */
/* -keys "t:sc:state,...": scripted input, injected on the emulated clock.
 *
 * This used to fire from the outer loop against wall-derived time, once per
 * frame-paced iteration, so a key landed on whatever instruction the host
 * happened to have reached: two runs of the same script diverged and nothing
 * downstream could be compared.  It now works exactly like replay injection
 * (REPLAY.md 2.1) - the script is parsed once into a sorted list, due events
 * fire on emu_now() before each batch, and the batch is clamped to the next
 * event so the key lands on the instruction it is due on.  Same script, same
 * cycle, every run.
 *
 * -keys is nulled during replay (above), so the two injectors never both
 * drive the guest.
 */
typedef struct { double t; int sc, dn; } KeyEv;
#define KEYEV_MAX 256
static KeyEv keyev[KEYEV_MAX];
static int keyev_n = 0, keyev_i = 0;

static void keys_parse(const char *s){
    const char *p = s;
    int i, j;
    keyev_n = keyev_i = 0;
    while(*p && keyev_n < KEYEV_MAX){
        const char *c1 = strchr(p, ':');
        double t = atof(p);
        if(!c1) break;
        {
            const char *c2 = strchr(c1+1, ':');
            keyev[keyev_n].t  = t;
            keyev[keyev_n].sc = (int)strtol(c1+1, NULL, 16);
            keyev[keyev_n].dn = c2 ? atoi(c2+1) : 1;
            keyev_n++;
        }
        p = strchr(p, ',');
        if(!p) break;
        p++;
    }
    /* Insertion sort, not qsort: the order of events sharing a timestamp
     * has to survive, or a make/break pair written as "t:sc:1,t:sc:0"
     * could come back inverted.  qsort gives no such guarantee; this does,
     * and 256 entries make the cost irrelevant. */
    for(i = 1; i < keyev_n; i++){
        KeyEv k = keyev[i];
        for(j = i - 1; j >= 0 && keyev[j].t > k.t; j--)
            keyev[j+1] = keyev[j];
        keyev[j+1] = k;
    }
    /* -load resumes mid-stream, so events already behind the restored
     * clock must not all fire at once on the first batch.  At a normal
     * boot emu_now() is 0 and nothing is skipped. */
    while(keyev_i < keyev_n && keyev[keyev_i].t < emu_now()) keyev_i++;
    fprintf(stderr, "[keys] %d scripted event%s", keyev_n, keyev_n == 1 ? "" : "s");
    if(keyev_i) fprintf(stderr, ", %d already past (skipped)", keyev_i);
    fprintf(stderr, "\n");
}
static void keys_inject_due(void){
    double now = emu_now();
    while(keyev_i < keyev_n && keyev[keyev_i].t <= now){
        kbd_key(keyev[keyev_i].sc, keyev[keyev_i].dn);
        keyev_i++;
    }
}
static uint64_t keys_next_deadline(void){
    if(keyev_i >= keyev_n) return ~(uint64_t)0;
    return until_cycles(keyev[keyev_i].t);
}

static unsigned long irq_count[32];

/* Seed present window, as a fraction of vtotal, used only until the redraw
 * bands have been learned from the running game (fantasies_present_window(),
 * src/fantasies.c - see docs #28).  These bounds come from the original
 * -balldbg measurement: the earliest mid-frame band seen across the four
 * shipped tables starts at line 222.7/527 = 0.422, and the vblank band ends
 * by 522/527. */
#define PRESENT_PHASE_LO 0.114   /* line  60/527 */
#define PRESENT_PHASE_HI 0.379   /* line 200/527 */
int present_phaselock = 1;       /* -nophaselock reverts to the wall timer */

/* ------------------------------------------------------------ main loop */
int emu_main(int argc, char **argv){
    const char *dir = NULL;   /* -d, the launcher, or release_scan() */
    const char *prog = NULL;  /* -p/-setup, or the detected release's boot file */
    const char *force_release = NULL; /* -release ID: skip detection's verdict */
    int list_releases = 0;    /* -releases: print the detection report and exit */
    RelResult rel;
    LaunchChoice lc;
    int no_launcher = 0;      /* -nolauncher: skip the picker dialog */
    int explicit_prog = 0;    /* -p / -setup names the program directly */
    int prog_opt = 0;         /* -p alone (not -setup) */
    int setup_opt = 0;        /* -setup */
    int start_fullscreen = 0; /* -fullscreen, or the launcher's checkbox */
    const char *record_path = NULL; /* -record FILE / launcher record mode */
    const char *replay_path = NULL; /* -replay FILE / launcher replay mode */
    const char *snap_load_path = NULL; /* -load FILE: boot from a snapshot */
    const char *snap_save_path = NULL; /* -snapsave FILE: snapshot at exit */
    int exit_code = 0;        /* nonzero when an exit-time step failed */
    /* -freezetime: pin the guest-visible DOS clock to the same constants
     * replay uses.  INT 21h AH=2Ah/2Ch are the only host-clock reads the
     * guest can see (INT 1Ah runs off the emulated BDA tick), so this is
     * what makes two play-mode runs byte-comparable - without it the game
     * mixes the wall clock into its own state and every run differs. */
    int freeze_time = 0;
    int start_table = 0;      /* -table N / launcher: boot straight to a table */
    int ips_given = 0, speed_given = 0;
    double t0, last_present = 0, wall_t0 = 0;
    double max_secs = 0, until_emu = -1.0;
    const char *shotfile = NULL;
    int keep_overlay = 0;     /* -keepoverlay: keep the replay overlay */
    const char *keyscript = NULL;
    /* -shotevery: both are EMULATED seconds now, not wall seconds.  See
     * the capture site in the batch loop below. */
    double shot_every = 0, next_shot = 0;
    double speed = 1.0;
    /* -unthrottle: drop the wall-clock pacer and run as fast as the host
     * allows.  Host pacing only - it overrides nothing the .pfr carries,
     * so a replay's recorded speed still travels with the file exactly as
     * REPLAY.md says it does; this bypasses the pacer rather than arguing
     * with it.  Verification wants it: a paced replay burns one wall
     * second per emulated second, which is ~5x the hardware for nothing.
     *
     * The reason it should be guest-invisible is that the pacer only
     * decides HOW MANY batches run per outer iteration.  Where a batch
     * begins and ends is set below by dev_next_deadline(),
     * replay_next_deadline(), until_cycles() and the 256-instruction cap -
     * all emulated-clock quantities, none of which can see plat_time().
     * That is an argument, not a measurement: docs/VERIFY.md lists
     * "is speed truly guest-invisible during replay?" as open, and the A/B
     * this flag makes possible is what closes it. */
    int unthrottle = 0;
    int vol_override = -1;    /* -vol N overrides the saved slider position */
    unsigned long mem_lo = 0;
    int shot_n = 0;
    int i;

    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-d") && i+1<argc) dir = argv[++i];
        else if(!strcmp(argv[i],"-p") && i+1<argc){ prog = argv[++i]; explicit_prog = 1; prog_opt = 1; }
        /* -setup: boot the sound-configuration utility instead of the game.
         * Equivalent to -p SETSOUND.EXE, but discoverable.  Pick SoundBlaster
         * (base 220h, IRQ 7), answer its questions, and it writes SOUND.CFG;
         * the write goes to PFEMU-STATE/ via the DOS overlay, so the
         * installed files stay pristine.  The game then uses it on next boot. */
        else if(!strcmp(argv[i],"-setup")){ prog = "SETSOUND.EXE"; explicit_prog = 1; setup_opt = 1; }
        else if(!strcmp(argv[i],"-t")){ trace_level = 1; trace_fp = fopen("pfemu.log","w"); }
        else if(!strcmp(argv[i],"-record") && i+1<argc) record_path = argv[++i];
        else if(!strcmp(argv[i],"-replay") && i+1<argc) replay_path = argv[++i];
        else if(!strcmp(argv[i],"-load") && i+1<argc) snap_load_path = argv[++i];
        else if(!strcmp(argv[i],"-snapsave") && i+1<argc) snap_save_path = argv[++i];
        else if(!strcmp(argv[i],"-ips") && i+1<argc){ emu_ips = atof(argv[++i]); ips_given = 1; }
        else if(!strcmp(argv[i],"-secs") && i+1<argc) max_secs = atof(argv[++i]);
        /* -untilemu SEC: stop when emulated time reaches SEC (validation:
         * snapshot round-trips and replay seeks need cycle-exact stops,
         * which wall-clock -secs cannot give). */
        else if(!strcmp(argv[i],"-untilemu") && i+1<argc) until_emu = atof(argv[++i]);
        /* -table N: start at table N (1-4) instead of the intro.  The
         * boot program still runs and stays resident, so quitting the
         * table returns to the menu (src/fantasies.c). */
        else if(!strcmp(argv[i],"-table") && i+1<argc) start_table = atoi(argv[++i]);
        else if(!strcmp(argv[i],"-freezetime")) freeze_time = 1;
        /* -cfgscan: report the six-byte PINBALL.CFG transfer in every
         * program that loads, not just the intro (src/fantasies.c). */
        else if(!strcmp(argv[i],"-cfgscan")){ extern int fantasies_cfgscan;
            fantasies_cfgscan = 1; }
        else if(!strcmp(argv[i],"-shot") && i+1<argc) shotfile = argv[++i];
        else if(!strcmp(argv[i],"-keepoverlay")) keep_overlay = 1;
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
        /* -res normal|high: one-run resolution override without saving
         * (like -vol for volume). The guest really boots in that mode;
         * refused with -replay, which must run the recorded one. */
        else if(!strcmp(argv[i],"-res") && i+1<argc){
            const char *r = argv[++i];
            if(!strcmp(r,"normal")) fantasies_res_override = 0;
            else if(!strcmp(r,"high")) fantasies_res_override = 1;
            else { fail_msg("-res takes normal|high"); return 1; } }
        else if(!strcmp(argv[i],"-dmairq")){ extern int sb_dmairq; sb_dmairq = 1; }
        else if(!strcmp(argv[i],"-nopatch")){ extern int dos_no_patch; dos_no_patch = 1; }
        else if(!strcmp(argv[i],"-nolzexe")){ dos_no_lzexe = 1; }
        /* -undefdump: on the first instruction the CPU cannot decode, write
         * that whole code segment to pfemu_cs_<SEG>.bin.  For programs that
         * arrive compressed (PKLITE .SDR, LZEXE .PRG) the guest's memory is
         * the only disassemblable copy of what is running. */
        else if(!strcmp(argv[i],"-undefdump")){ extern int undef_dump; undef_dump = 1; }
        /* -vgastate: at exit, print the mode, the registers that select the
         * picture, the DAC entries it can reach, and what is in the memory the
         * CRTC points at.  "Black screen" has three unrelated causes that look
         * identical from outside; this separates them. */
        else if(!strcmp(argv[i],"-vgastate")){ vga_state_dump_on = 1; }
        /* -prof: sample CS:IP every 4096 instructions and print the hottest
         * sites at exit.  "Which code is spending the emulated budget" is not
         * reliably answerable by counting instructions per loop iteration. */
        else if(!strcmp(argv[i],"-prof")){ prof_on = 1; }
        /* -dumpseg SEG: write that 64K segment out at exit.  The same need as
         * -undefdump, for a program that misbehaves without ever executing a
         * bad opcode - a compressed one has no file that matches what runs. */
        else if(!strcmp(argv[i],"-dumpseg") && i+1<argc){
            dump_seg_on = 1;
            dump_seg_which = (uint16_t)strtoul(argv[++i], NULL, 16); }
        else if(!strcmp(argv[i],"-mem") && i+1<argc){ mem_lo = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-balldbg")){ balldbg_on = 1; }
        else if(!strcmp(argv[i],"-nophaselock")){ present_phaselock = 0; }
        else if(!strcmp(argv[i],"-nolatch")){ vga_latch_start = 0; }
        else if(!strcmp(argv[i],"-noballsync")){ vga_ballsync = 0; }
        else if(!strcmp(argv[i],"-trapexit")){ extern int dos_trap_exit; extern int x_on; dos_trap_exit = 1; x_on = 1; }
        else if(!strcmp(argv[i],"-intwatch") && i+1<argc){ extern int int_watch; int_watch = (int)strtol(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-intstat") && i+1<argc){ extern int int_stat; int_stat = (int)strtol(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-memwatch") && i+1<argc){ memwatch_addr = (uint32_t)strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-trap") && i+2<argc){ extern uint32_t x_trap_lo, x_trap_hi; extern int x_on;
            x_on = 1; x_trap_lo = strtoul(argv[++i],NULL,16); x_trap_hi = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-speed") && i+1<argc){ speed = atof(argv[++i]); speed_given = 1; }
        else if(!strcmp(argv[i],"-unthrottle")) unthrottle = 1;
        else if(!strcmp(argv[i],"-strict")) replay_set_strict(1);
        else if(!strcmp(argv[i],"-nolauncher")) no_launcher = 1;
        else if(!strcmp(argv[i],"-fullscreen")) start_fullscreen = 1;
        else if(!strcmp(argv[i],"-flipdbg")) vga_flipdbg = 1;
        else if(!strcmp(argv[i],"-dmd")) vga_dmdlog = 1;
        else if(!strcmp(argv[i],"-matdbg")){ extern int pit0_m0_log;
            mat_dbg = 1; pit0_m0_log = 60; }
        /* -scoredbg: per-attempt score/segmentation trace, Spike A of
         * docs/VERIFY.md.  Read-only - a run with it on must produce the
         * same footer as one without. */
        else if(!strcmp(argv[i],"-scoredbg")){ scoredbg_on = 1; }
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
    /* Session record / replay (docs/REPLAY.md): refuse nonsense combos
     * first, then let the replay header override the clock. */
    if(record_path && replay_path){
        fail_msg("cannot -record and -replay in one run");
        return 1;
    }
    if(setup_opt && (record_path || replay_path)){
        fail_msg("cannot combine -setup with -record/-replay");
        return 1;
    }
    if(prog_opt && replay_path){
        fail_msg("cannot combine -p with -replay: replay boots the recorded program");
        return 1;
    }
    if(replay_path && fantasies_res_override != -1){
        fail_msg("cannot combine -res with -replay: replay boots the recorded resolution");
        return 1;
    }
    /* Snapshots hold the whole machine, so there is nothing to record or
     * replay, no program to boot, and no setup to run. */
    if(snap_load_path && (record_path || replay_path || prog_opt || setup_opt)){
        fail_msg("cannot combine -load with -record/-replay/-p/-setup");
        return 1;
    }
    if(start_table && (start_table < 1 || start_table > 4)){
        fail_msg("-table takes 1-4");
        return 1;
    }
    /* -p/-setup name the program directly, so there is no boot program left
     * to stay resident and redirect; -load already holds a whole machine. */
    if(start_table && (prog_opt || setup_opt || snap_load_path)){
        fail_msg("cannot combine -table with -p/-setup/-load");
        return 1;
    }
    /* -load with -snapsave is allowed on purpose: resume a snapshot and
     * re-freeze at a later point is the round-trip that validates the
     * whole mechanism (EMULATOR.md, Savestates).  Only the modes that own
     * the clock are refused. */
    if(snap_save_path && (record_path || replay_path)){
        fail_msg("cannot combine -snapsave with -record/-replay");
        return 1;
    }
    if(replay_path && replay_begin_replay(replay_path) != 0){
        const char *e = replay_parse_error();
        if(e) fail_msg("%s", e);
        else fail_msg("cannot replay '%s'", replay_path);
        return 1;
    }
    if(replay_path){
        int h;
        double f = replay_forced_ips(&h);
        if(h){
            if(ips_given) fprintf(stderr, "[replay] ignoring -ips %.0f, using recorded %.0f\n",
                                  emu_ips, f);
            emu_ips = f;
        }
        f = replay_forced_speed(&h);
        if(h){
            if(speed_given) fprintf(stderr, "[replay] ignoring -speed %g, using recorded %g\n",
                                    speed, f);
            speed = f;
        }
        /* The wall-clock keyscript path is replaced by the emu-time
         * injector; a script beside a replay would double-drive the guest. */
        if(keyscript){
            fprintf(stderr, "[replay] ignoring -keys during replay\n");
            keyscript = NULL;
        }
    }
    /* Recording forces real-time speed: the event timestamps only mean what
     * they say when one wall second is one emulated second. */
    if(record_path && speed != 1.0){
        fprintf(stderr, "[record] forcing speed=1 (was %g)\n", speed);
        speed = 1.0;
    }
    if(emu_ips <= 0.0) emu_ips = 6000000.0;
    emu_inv_ips = 1.0 / emu_ips;
    if(unthrottle)
        fprintf(stderr, "[pfemu] -unthrottle: wall-clock pacing off,"
                        " batch deadlines unchanged\n");

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

    /* The command line's own values, kept because the launcher merges its
     * choices into these same variables and a refused attempt leaves them
     * half-merged.  A second trip through the picker has to start from the
     * command line again, not from the attempt that just failed. */
    {
    const char *cli_dir = dir, *cli_prog = prog, *cli_keys = keyscript;
    int cli_fullscreen = start_fullscreen, cli_table = start_table;
    int cli_nopatch = dos_no_patch, cli_nolzexe = dos_no_lzexe;
    double cli_ips = emu_ips, cli_speed = speed;
    int attempt = 0;

    /* One pass from here to the first executed instruction is one attempt to
     * start a session.  A refusal along the way - a replay that does not
     * match the installation, a record target that will not open, a boot
     * program that will not load - used to show its message box and then
     * exit, which from the picker looks exactly like "Launch quits the app".
     * It now refuses only the thing that was asked for and brings the picker
     * back, so the user can pick something else.  A command-line run has no
     * picker to return to and still exits with a status; that path comes
     * through here exactly once. */
relaunch:
    if(attempt++){
        /* Undo everything the refused attempt merged in or armed. */
        replay_abort();
        record_path = replay_path = NULL;
        dir = cli_dir; prog = cli_prog; keyscript = cli_keys;
        start_fullscreen = cli_fullscreen; start_table = cli_table;
        dos_no_patch = cli_nopatch; dos_no_lzexe = cli_nolzexe;
        emu_ips = cli_ips; emu_inv_ips = 1.0 / emu_ips; speed = cli_speed;
        dos_set_time_frozen(freeze_time);
        from_launcher = 0;
    }

    /* Game picker, unless the run is explicit or automated: -p/-setup name
     * the program, -secs means a headless benchmark, -nolauncher forces the
     * old behaviour (boot dir/prog straight away).  In picker mode -d is
     * ignored: the directory comes from the selected installation. */
    if(!no_launcher && !explicit_prog && max_secs <= 0.0 && !record_path && !replay_path){
        if(!show_launcher(&lc)) return 0;
        from_launcher = 1;
        dir = lc.dir; prog = lc.prog;
        if(lc.fullscreen) start_fullscreen = 1;
        if(lc.start_table) start_table = lc.start_table;
        /* The launcher's record/replay mode is the same frontend as the
         * CLI flags above (docs/REPLAY.md section 4). */
        if(lc.mode == LAUNCH_RECORD) record_path = lc.replay_path;
        else if(lc.mode == LAUNCH_REPLAY) replay_path = lc.replay_path;
        if(replay_path && replay_begin_replay(replay_path) != 0){
            const char *e = replay_parse_error();
            if(e) fail_msg("%s", e);
            else fail_msg("cannot replay '%s'", replay_path);
            goto relaunch;
        }
        if(replay_path){
            int h;
            double f = replay_forced_ips(&h);
            if(h){ emu_ips = f; emu_inv_ips = 1.0 / emu_ips; }
            f = replay_forced_speed(&h);
            if(h) speed = f;
            if(keyscript){
                fprintf(stderr, "[replay] ignoring -keys during replay\n");
                keyscript = NULL;
            }
        }
        if(record_path) speed = 1.0;
    }
    /* No -d and no launcher: take the first installation the detector finds
     * (GAME, then the rest alphabetically), so a headless run needs no more
     * arguments than an interactive one.
     *
     * A replay is the one case where that default has something better to go
     * on: the file names the release it was recorded from, and an install of
     * some other release will be refused a few lines below whatever else is
     * true of it.  So prefer an install that IS that release.  This only ever
     * decides between installs nobody chose - the launcher does not run for a
     * replay, so reaching here means no -d was given - and it never overrides
     * an explicit choice, because an explicit wrong one is worth a refusal
     * rather than a silent substitution. */
    if(!dir){
        static RelResult found[8];
        const char *want = replay_path ? replay_wanted_release() : NULL;
        int n = release_scan(found, 8), k;
        if(want)
            for(k=0;k<n;k++)
                if(release_runnable(&found[k]) && found[k].rel &&
                   !_stricmp(found[k].rel->id, want)){
                    dir = found[k].dir;
                    fprintf(stderr, "[replay] no -d given; '%s' is the '%s'"
                            " install this recording needs\n", dir, want);
                    break;
                }
        if(!dir)
            for(k=0;k<n;k++) if(release_runnable(&found[k])){ dir = found[k].dir; break; }
        if(!dir) dir = n ? found[0].dir : "GAME";
    }
    {   /* Install dirs are stored relative ("FANTASY", ...): resolve once
         * against the launch-time CWD so nothing that moves it afterwards
         * (a file dialog, a shortcut's "Start in" dir) changes which
         * install boots or where its state lives. */
        static char absdir[1024];
        if(dir && plat_abspath(dir, absdir, sizeof(absdir)) && absdir[0])
            dir = absdir;
    }

    /* Identify the release by content.  The launcher already did this for its
     * own choice; -d and -nolauncher runs land here having done nothing, and
     * every path needs the descriptor, so it simply runs again - hashing 2.4 MB
     * is not worth the bookkeeping to avoid. */
    release_detect(dir, &rel);
    if(force_release){
        const Release *fr = release_by_id(force_release);
        if(!fr){
            /* A typo in a command-line flag, so the picker has nothing to
             * offer instead - but say so in a box as well, or a run that
             * happened to show the picker just disappears. */
            char ids[512] = "";
            int k;
            for(k=0;k<release_count();k++){
                if(k) strncat(ids, " ", sizeof(ids)-strlen(ids)-1);
                strncat(ids, release_at(k)->id, sizeof(ids)-strlen(ids)-1);
            }
            fail_msg("unknown release id '%s'; known ids are: %s",
                     force_release, ids);
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
    /* Host DSP enhancement (src/sound.c): launcher-only settings, read like
     * the saved volume above so headless runs sound like interactive ones.
     * Never recorded and never forced by replay - same promise as volume. */
    { PfCfg ecfg;
      cfg_read(dir, &ecfg);
      audio_bass = ecfg.bass;
      audio_treble = ecfg.treble;
      audio_oomph = ecfg.oomph;
      audio_headphone = ecfg.headphone ? 1 : 0;
      if(audio_bass < -12) audio_bass = -12;
      if(audio_bass > 12) audio_bass = 12;
      if(audio_treble < -12) audio_treble = -12;
      if(audio_treble > 12) audio_treble = 12;
      if(audio_oomph < 0) audio_oomph = 0;
      if(audio_oomph > 12) audio_oomph = 12;
      if(audio_bass || audio_treble || audio_oomph || audio_headphone)
          fprintf(stderr, "[snd] enhancement: bass %+d dB, treble %+d dB,"
                          " oomph +%d dB, headphones %s\n",
                  audio_bass, audio_treble, audio_oomph,
                  audio_headphone ? "on" : "off");
    }

    /* Arm the Fantasies session.  Nothing Fantasies-specific runs unless the
     * directory's five program hashes identified an actual release, so no
     * Fantasies-only behaviour can leak into a sibling game however its files
     * happen to be named - and, just as importantly, no intro's memory layout
     * can be poked into a different intro's code. */
    fantasies_begin_session(dir, prog, release_runnable(&rel) ? rel.rel : NULL);

    /* High resolution is a different machine load: 360x350 at ~71 Hz
     * against 320x240 at 60 Hz (see FANTASIE.ASM: SH_HI/SH_LO,
     * sync_per_sec 71 vs 60, and the TECHSCROLL mode line).  That is
     * ~1.64x the pixels per frame at ~1.18x the frames, so ~2x the
     * fill plus unscaled physics (init_ballspeed skips its 5/6 downscale
     * in hi-res).  At the default 6 MIPS the guest cannot meet its own
     * frame deadlines, so emu_time runs behind wall time: the game
     * (scrolling) and the MOD player (tempo) both run slow, and the
     * waveOut queue starves between emu-paced pushes, which reads as
     * crackle.  Lowering the sound notch does not help because the
     * mixer is not the bottleneck - the pixels are.  Model a faster CPU
     * automatically (record stores the bumped value, replay keeps its
     * own, -ips always wins). */
    if(!ips_given && !replay_is_replaying()){
        int eff_hi;
        if(fantasies_res_override == 1) eff_hi = 1;
        else if(fantasies_res_override == 0) eff_hi = 0;
        else { PfCfg cc; cfg_read(dir, &cc); eff_hi = (cc.options[4] == 1); }
        if(eff_hi && emu_ips < 12000000.0){
            emu_ips = 12000000.0;
            emu_inv_ips = 1.0 / emu_ips;
            fprintf(stderr, "[pfemu] High resolution: using 12000000 ips"
                            " (default 6000000 is not enough for 360x350);"
                            " -ips overrides\n");
        }
    }

    /* Replay preconditions (docs/REPLAY.md sections 3.2/3.3): the trainer is
     * incompatible with both modes, recording needs a recognised install to
     * have any identity to store, and replay boots the recorded program in
     * the recorded environment - verified before anything runs. */
    if((record_path || replay_path) && fantasies_trainer_enabled()){
        fail_msg("[replay] refused: the trainer is enabled for '%s'."
                 " Recording and replay need it off.", dir);
        if(from_launcher) goto relaunch;
        return 1;
    }
    if(record_path && !release_runnable(&rel)){
        fail_msg("[record] refused: '%s' is not a recognised release"
                 " (%s: %s).",
                 dir, release_state_name(rel.state), rel.summary);
        if(from_launcher) goto relaunch;
        return 1;
    }
    if(replay_path){
        char why[512], hint[256] = "";
        if(replay_verify_install(&rel, prog, why, sizeof(why)) != 0){
            /* A release mismatch here means -d named an install of some other
             * release (nothing else can reach this - an unchosen install is
             * matched to the file above).  The fix is one flag away, so name
             * the install that would work instead of leaving it to be
             * guessed. */
            const char *want = replay_wanted_release();
            if(want && rel.rel && _stricmp(rel.rel->id, want)){
                static RelResult found[8];
                int n = release_scan(found, 8), k;
                for(k=0;k<n;k++)
                    if(release_runnable(&found[k]) && found[k].rel &&
                       !_stricmp(found[k].rel->id, want)){
                        snprintf(hint, sizeof(hint),
                                 "\n'%s' is the '%s' install here: add -d %s",
                                 found[k].dir, want, found[k].dir);
                        break;
                    }
            }
            fail_msg("%s%s", why, hint);
            if(from_launcher) goto relaunch;
            return 1;
        }
        replay_apply_recorded_env();
        /* The session runs the recorded view, not the install's current
         * one: fullscreen is host-only, but it is part of the session. */
        start_fullscreen = replay_recorded_fullscreen();
    }
    if(record_path){
        int sound = read_sound_is_sb(dir);
        int quality = read_sound_quality(dir);
        uint8_t opts[6];
        replay_read_options(dir, opts);
        /* A -res override changes what the guest executes, so the header
         * must store it - otherwise replay (which runs the header's blob)
         * would diverge from what was recorded. */
        if(fantasies_res_override == 0 || fantasies_res_override == 1)
            opts[4] = (uint8_t)fantasies_res_override;
        if(replay_begin_record(record_path, &rel, prog, emu_ips,
                               dos_no_patch, dos_no_lzexe, sound, quality, opts,
                               start_fullscreen, start_table) != 0){
            fail_msg("[record] cannot write '%s'", record_path);
            if(from_launcher) goto relaunch;
            return 1;
        }
    }

    /* Allocated once, wiped per attempt: a retry re-runs every init below,
     * so the machine starts as fresh as on the first pass, but calloc()ing
     * again would simply leak the previous attempt's RAM.  This is the one
     * failure here that really is fatal, so it does end the process - but
     * through fail_msg, so a launcher run sees why rather than vanishing. */
    if(!ram) ram = (uint8_t*)calloc(RAM_SIZE,1);
    else memset(ram, 0, RAM_SIZE);
    if(!ram){ fail_msg("out of memory"); return 1; }

    cpu_reset();
    vga_init();
    dev_init();
    bios_init();
    dos_init(dir);
    /* The snapshot slot + identity for F6/F8, captured once the install
     * is known.  A snapshot always belongs to the install it was taken
     * on: release id + code vector are verified on load. */
    snprintf(snap_dir, sizeof(snap_dir), "%s", dir);
    snap_rel = rel; snap_have_rel = 1;
    /* Only ever set, never cleared: replay turns the same gate on for its
     * own reasons and must keep it. */
    if(freeze_time) dos_set_time_frozen(1);
    /* A replay carries its own start_table and it wins: the recorded
     * event stream assumes whichever way that session began, so
     * replaying it the other way desyncs on the first key. */
    if(replay_is_replaying()){
        int th, t = replay_forced_start_table(&th);
        if(th && t != start_table){
            if(start_table) fprintf(stderr,
                "[replay] ignoring -table %d, using recorded %d\n", start_table, t);
            start_table = t;
        }
    }
    if(start_table) fantasies_set_start_table(start_table);
    /* After dos_init (which points the overlay at the install) and before
     * dos_exec (which first touches it): replay works on a temp copy, so
     * the user's real PFEMU-STATE/ is never written (REPLAY.md 3.3). */
    if(replay_path){
        replay_isolate_overlay(dir);
        /* ...and the recorded sound notch into the temp copy, so the
         * driver mixes as recorded even when the install moved on. */
        replay_apply_config_to_overlay();
    }

    /* Hand-built boot: park the CPU on a HLT in ROM, then EXEC the program.
     * -load skips the EXEC: the snapshot holds the whole machine already. */
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
    if(snap_load_path){
        char why[512] = "";
        if(snapshot_load(snap_load_path, &rel, why, sizeof(why)) != 0){
            fail_msg("%s", why[0] ? why : "cannot load snapshot");
            if(from_launcher) goto relaunch;
            return 1;
        }
    } else if(dos_exec(prog, 0, 0, 0, 0) != 0){
        fail_msg("could not load %s from %s", prog, dir);
        if(from_launcher) goto relaunch;
        return 1;
    }

    /* The window opens here, after the last thing that can refuse the
     * session.  Creating it with the machine (where this used to sit) meant
     * a refused boot flashed an empty game window up and tore it down again
     * behind the message box - and now that the picker comes back, it would
     * have left that window standing behind it.  Nothing between dos_init()
     * and this point draws or reads input, so there is nothing to miss. */
    plat_init("Pinball Fantasies - pfemu");
    if(start_fullscreen) plat_set_fullscreen(1);
    }   /* end of the retry scope: the session is committed from here */

    /* Parse -keys into the sorted event list the injector reads.
     *
     * This call went missing when the wall-clock keyscript path was replaced
     * by the emu-time one: run_keyscript() was removed and keys_parse() was
     * written, but nothing ever called it.  -keys has therefore been accepted
     * and silently ignored ever since - keyev_n stayed 0, so keys_inject_due()
     * had nothing to inject and keys_next_deadline() never clamped a batch.
     *
     * Here rather than at the flag, because replay nulls keyscript in two
     * places above (a script beside a replay would double-drive the guest)
     * and both have had their say by now. */
    if(keyscript) keys_parse(keyscript);

    t0 = plat_time();
    /* A snapshot resumes mid-stream: anchor the wall clock behind the
     * restored emulated time, or `emu_time < real` never runs again.
     * -secs still counts wall seconds from process start, on its own
     * marker. */
    wall_t0 = plat_time();
    if(snap_load_path) t0 -= emu_now() / speed;
    /* Fell-behind re-anchors (below): how many times the guest failed to
     * keep up with wall time.  Reported at exit ([pfemu] pace) - the
     * number that says whether a slowdown is emulation lagging the wall
     * (this climbs) or game logic running slow on a healthy clock. */
    unsigned long fell_n = 0;
    /* Wall-time split for the same report: emulation (catch-up batches)
     * vs presentation (render + StretchDIBits + blit).  Says whether a
     * host ceiling is interpreter throughput or the present path. */
    double t_emu_w = 0.0, t_pres_w = 0.0;
    { unsigned long long pres_last_frame = 0; int pending_present = 0;
    while(plat_pump() && !cpu.shutdown){
        double wall = plat_time() - t0;
        double real = wall * speed;
        if(max_secs > 0 && plat_time() - wall_t0 > max_secs) break;
        if(until_emu >= 0.0 && emu_now() >= until_emu) break;
        /* Reconcile reads live host key state: a leak on replay, where the
         * guest must see only the recorded list (REPLAY.md 2.4/3.3). */
        if(!replay_is_replaying()) plat_kbd_reconcile();
        int guard = 0;
        { double t0e = plat_time();
        while((unthrottle || emu_time < real) && !cpu.shutdown && guard < 10000){
            int n;
            /* Emu-time injection (REPLAY.md 2.1): due events fire on the
             * emulated clock before the next batch runs. */
            if(replay_is_replaying()) replay_inject_due();
            if(keyscript) keys_inject_due();
            if(cpu.halted){
                /* idle: jump the clock forward to the next scheduled event */
                uint64_t step = (uint64_t)(emu_ips / 10000.0);
                if(replay_is_replaying()){
                    /* Never jump past a recorded keypress: an HLT wait for
                     * input would otherwise land the event a jump late. */
                    uint64_t rdl = replay_next_deadline();
                    if(rdl != ~(uint64_t)0 && rdl < cpu.cycles + step)
                        step = (rdl > cpu.cycles) ? (rdl - cpu.cycles) : 0;
                }
                /* Never jump an idle clock past a scripted key either -
                 * the guest HLTs waiting for input, which is exactly when
                 * the script is supposed to supply some. */
                if(keyscript){
                    uint64_t kdl = keys_next_deadline();
                    if(kdl != ~(uint64_t)0 && kdl < cpu.cycles + step)
                        step = (kdl > cpu.cycles) ? (kdl - cpu.cycles) : 0;
                }
                /* Same for the -untilemu stop: an idle jump must not carry
                 * the clock an arbitrary distance past the target. */
                if(until_emu >= 0.0){
                    uint64_t udl = until_cycles(until_emu);
                    if(udl < cpu.cycles + step)
                        step = (udl > cpu.cycles) ? (udl - cpu.cycles) : 0;
                }
                cpu.cycles += step;
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
                /* Clamp the batch to the next recorded event, the same way
                 * the IRQ0 deadline keeps timer precision: injection stays
                 * within a few instructions of the recorded time. */
                if(replay_is_replaying()){
                    uint64_t rdl = replay_next_deadline();
                    if(rdl < dl) dl = rdl;
                }
                if(keyscript){
                    uint64_t kdl = keys_next_deadline();
                    if(kdl < dl) dl = kdl;
                }
                if(until_emu >= 0.0){
                    uint64_t udl = until_cycles(until_emu);
                    if(udl < dl) dl = udl;
                }
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
                            pit0_wait_sum += wait;
                            if(wait > pit0_wait_max){
                                /* Latch what was blocking the worst one, so
                                 * the report can name it instead of just
                                 * sizing it. */
                                extern uint16_t pit0_raise_cs, pit0_raise_ip;
                                extern int pit0_raise_if;
                                extern uint16_t pit0_blk_cs, pit0_blk_ip, pit0_got_cs, pit0_got_ip;
                                extern int pit0_blk_if;
                                pit0_wait_max = wait;
                                pit0_blk_cs = pit0_raise_cs; pit0_blk_ip = pit0_raise_ip;
                                pit0_blk_if = pit0_raise_if;
                                pit0_got_cs = cpu.sreg[S_CS]; pit0_got_ip = (uint16_t)cpu.eip;
                            }
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
            /* -shotevery capture, on the emulated clock.
             *
             * This used to ride the present path, which is paced by the WALL
             * clock (`real = wall * speed`, and the present gate itself tests
             * plat_time()).  So the frames landed wherever the host happened
             * to be: two runs of the same replay on the same machine sampled
             * different emulated moments and produced different files.  That
             * makes them useless as the comparison REPLAY.md's validation
             * section names them for ("replay twice -> ... -shotevery frames
             * ... must all match"), which is the criterion Spike B has to
             * carry across two platforms.
             *
             * Sampled here instead, at the first batch boundary at or after
             * each due time.  Deterministic, because where the batches fall is
             * itself a deterministic function of the run - and it no longer
             * needs a present at all, so a headless host captures too.
             *
             * It deliberately does NOT clamp the batch to the due cycle, the
             * way the replay, -keys and -untilemu deadlines do.  That was the
             * first attempt and it was wrong: a capture has to observe the run
             * without altering it, and shortening a batch alters it.  Measured,
             * not reasoned - the same replay on the same binary produced wav
             * hash dd938f6bd1540842 with -shotevery and dfb1427eef9a2239
             * without, so taking a picture was changing the sound.  The clamp
             * is right for an injection (the key must land on the instruction
             * it is due on) and wrong for an observation.
             *
             * The cost is that a frame lands up to one batch late rather than
             * exactly on the grid.  That is the correct trade: the capture is
             * still identical between two runs, and now the run is identical
             * to one that was never capturing at all.
             *
             * vga_render() only reads guest state (registers, VRAM, DAC), so
             * sampling here costs the guest nothing it can observe. */
            if(shot_every > 0 && emu_now() >= next_shot){
                char nm[64];
                do { next_shot += shot_every; } while(emu_now() >= next_shot);
                vga_render(fb, &fbw, &fbh);
                sprintf(nm, "seq%03d.ppm", shot_n++);
                save_ppm(nm, fb, fbw, fbh);
            }
            /* Batch-precision -untilemu stop, for the same reason as the
             * replay footer below: the outer check is wall-clock paced, so
             * it fires a variable distance past the target and two runs
             * stop on different instructions - which is exactly what the
             * flag exists to prevent.  The clamp above already ended this
             * batch on the target cycle. */
            if(until_emu >= 0.0 && emu_now() >= until_emu) break;
            /* Batch-precision replay stop: the outer check below only runs
             * once per (frame-paced) outer iteration, which lands the
             * footer up to a frame late.  Catch it here instead, on the
             * batch that crosses it - the footer deadline above already
             * clamped this batch to end right on it. */
            if(replay_is_replaying() && replay_should_stop()) break;
            guard++;
        }
        t_emu_w += plat_time() - t0e; }
        if(!unthrottle && emu_time < real - 0.25*speed) { t0 = plat_time() - emu_time/speed; fell_n++; }  /* fell behind */
        /* Replay end condition (REPLAY.md 3.3): the footer's emu_time, with
         * the event list exhausted.  ScrollLock / window close still end it
         * early through plat_pump(), exactly as in normal play.  Paused
         * stays paused (resume to finish). */
        if(replay_is_replaying() && replay_should_stop()) break;
        /* Snapshot slot (F6 save / F8 load): serviced here, at an
         * instruction boundary between batches - cpu.c's decode residue
         * only ever lives mid-cpu_step, so the saved state is exact. */
        if(snap_save_pending || snap_load_pending){
            char slot[512];
            snapshot_slot_path(snap_dir, slot, sizeof(slot));
            if(snap_save_pending){
                snap_save_pending = 0;
                if(snap_have_rel && snapshot_save(slot, &snap_rel) == 0)
                    osd_show("Snapshot saved");
                else osd_show(snapshot_error() ? snapshot_error()
                                               : "Snapshot save failed");
            } else {
                char why[512] = "";
                snap_load_pending = 0;
                if(snap_have_rel && snapshot_load(slot, &snap_rel, why, sizeof(why)) == 0){
                    /* Re-anchor the wall clock to the restored emulated one.
                     * The loop only runs batches while emu_time < real, and a
                     * load moves emu_time to either side of it: a snapshot
                     * from a longer session than this one lands emu_time
                     * AHEAD, so no batch ever runs again and the guest sits
                     * there while the window keeps painting the restored
                     * frame - indistinguishable from a freeze, and it would
                     * last until wall time caught up (minutes).  The
                     * fell-behind re-anchor below only covers the other
                     * direction, which is why loading a state from earlier
                     * in the same session always looked fine.  -load does
                     * this already; F8 did not. */
                    t0 = plat_time() - emu_now() / speed;
                    osd_show("Snapshot loaded");
                }
                else osd_show(why[0] ? why : "Snapshot load failed");
            }
        }

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
            double t0p = plat_time();
            pres_last_frame = idx;
            last_present = plat_time();
            fantasies_ballgap_present(fell_behind);
            vga_render(fb, &fbw, &fbh);
            /* Live hi-res latch: the F5 menu can switch resolution without
             * rebooting, which the boot-time cfg check above never sees.
             * A 256-colour table frame 340-360 rows high IS the hi-res
             * table mode (Normal renders 240, the menu 480, text 400), so
             * latch the same faster-CPU model on first sight.  Play mode
             * only: recording must keep the ips its header already stored
             * and replay must keep the recorded one, or the two diverge. */
            if(!ips_given && !replay_is_recording() && !replay_is_replaying()
               && emu_ips < 12000000.0 && fbh >= 340 && fbh <= 360){
                emu_ips = 12000000.0;
                emu_inv_ips = 1.0 / emu_ips;
                fprintf(stderr, "[pfemu] High-resolution frame (%dx%d):"
                                " using 12000000 ips; -ips overrides\n",
                        fbw, fbh);
            }
            /* Captures first, badges after: -shot frames and F11 screenshots
             * are validation artifacts and stay pixel-clean (this also lifts
             * the old OSD text out of them).  The window still shows
             * everything below.  -shotevery is no longer taken here - it is
             * sampled on the emulated clock in the batch loop above. */
            if(screenshot_pending){
                screenshot_pending = 0;
                plat_screenshot(fb, fbw, fbh);
            }
            plat_present(fb, fbw, fbh);
            t_pres_w += plat_time() - t0p;
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
     * -vol on its own never writes; only an in-window change does.
     *
     * Never on replay: the volume keys stay live (host gain only), but
     * persisting them would write the user's real overlay, which replay
     * promises never to touch (REPLAY.md 3.3). */
    if(audio_volume_dirty && !replay_is_replaying())
        write_volume_cfg(dir, vol_muted && vol_premute > 0 ? vol_premute : audio_volume);

    /* Remember where the window was for the next launch.  First thing
     * after the loop with the volume save, for the same reason: the exit
     * report below must not be able to lose it.  Global (pfemu-winpos.cfg),
     * so replay's promise never to touch the install's PFEMU-STATE/ is
     * unaffected - the window lives on the desk, not in the install. */
    plat_save_window_pos();

    vga_render(fb,&fbw,&fbh);
    plat_present(fb,fbw,fbh);
    if(shotfile) save_ppm(shotfile, fb, fbw, fbh);
    { extern void vga_dump(void); vga_dump(); }
    { extern unsigned long st1_calls, st1_bit0, st1_bit3;
      printf("[pfemu] 3DA reads=%lu  bit0(blank)=%lu  bit3(vsync)=%lu\n",
             st1_calls, st1_bit0, st1_bit3); }
    { extern void st1_report(void); st1_report(); }
    /* -snapsave: freeze the session at loop exit (a -secs point), for
     * scripted save/load validation.  Interactive F6 serves the same call
     * mid-session. */
    /* A failed -snapsave has to reach the exit code: scripted validation
     * reads that before it reads the logs, and a silent 0 here made a run
     * that saved nothing look like a clean pass. */
    if(snap_save_path && snapshot_save(snap_save_path, &rel) != 0){
        fail_msg("%s", snapshot_error() ? snapshot_error() : "cannot save snapshot");
        exit_code = 1;
    }
    /* Close the recording with its footer before the exit reports below,
     * so the file is complete even though the session is over. */
    if(replay_is_recording()) replay_end_record();
    replay_report();
    fantasies_ballgap_report();
    fantasies_matrix_report();
    fantasies_score_report();
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
    vga_state_dump();
    prof_report();
    intstat_report();
    memwatch_report();
    if(dump_seg_on) cpu_dump_segment(dump_seg_which, "-dumpseg");
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
    { extern unsigned long audio_drop_n;
      double wall_used = plat_time() - wall_t0;
      /* Pace: did emulation keep up with the wall?  fell_behind climbing
       * with emu_time < wall means the guest (or the host) could not hold
       * the modelled rate - game slow-motion plus starved audio.  audio
       * drops climbing instead means pushes outran consumption. */
      printf("[pfemu] pace: wall=%.3fs emu=%.3fs ips=%.0f fell_behind=%lu"
             " audio_drops=%lu host_mips=%.2f\n",
             wall_used, emu_time, emu_ips, fell_n, audio_drop_n,
             wall_used > 0.0 ? (double)(unsigned long long)cpu.cycles / wall_used / 1e6 : 0.0);
      /* Where the wall time went: emulation vs presentation, the rest
       * (other) is sleep + message pump + input. */
      printf("[pfemu] wsplit: emu=%.2fs present=%.2fs other=%.2fs of wall=%.2fs\n",
             t_emu_w, t_pres_w,
             wall_used - t_emu_w - t_pres_w > 0.0 ? wall_used - t_emu_w - t_pres_w : 0.0,
             wall_used); }
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
      wav_close(); plat_audio_close(); plat_shutdown(); }
    dos_close_all_handles();
    /* -keepoverlay: leave the isolated copy behind instead of deleting it.
     * Everything the guest wrote during the replay is in there, and the one
     * that matters for scoring is TABLEn.HI - the game's own high-score file,
     * 4 entries of 12 unpacked BCD digits and three initials, written by
     * SAVE_HIGHS when a run earns a place.  That makes it an independent
     * check on -scoredbg: the number pfemu read out of guest memory and the
     * number the game itself wrote to disk have nothing in common but the
     * game.  The directory is the caller's to delete.  Only here, never on
     * the abort path in replay.c, where nothing ran and there is nothing to
     * look at. */
    if(keep_overlay && replay_overlay_path())
        fprintf(stderr, "[replay] -keepoverlay: session overlay left at '%s'\n",
                replay_overlay_path());
    else
        replay_cleanup_overlay();
    if(trace_fp) fclose(trace_fp);
    return exit_code;
}
