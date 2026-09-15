/* Pinball Fantasies game-specific fixes.
 *
 * Everything that applies to Fantasies (and only Fantasies) lives in this
 * file, so dos.c/dev.c stay generic:
 *
 *  - the INTRO.PRG manual-lookup image patch (memory-only, signature-checked,
 *    -nopatch disables);
 *  - the flipper fix session/exec gating (see below);
 *  - the trainer hotkeys ('1'/'2', see fantasies_key_event below).
 *
 * Flipper background: the tables drive each flipper from a single level bit
 * shared by 3 keys (left: 2A/38/1D, right: 36/E0-38/E0-1D; FANTASIE.ASM KEYINT
 * DSSK/DSSKR, live TABLE1.PRG image 0x3F34).  A lost break byte leaves the bit
 * set, so the flipper stays up until the next make+break cycle.  dev.c repairs
 * the host-side break-loss paths, but only while fantasies_fix_active().
 *
 * Session gating: the fix must only engage when the user actually booted
 * Pinball Fantasies.  Matching on the EXEC'd filename alone (TABLE1.PRG) is
 * not enough - a sibling game could ship a same-named file.  So main() arms
 * this session from the launcher choice (or its CLI equivalent: -d/-p), and
 * fantasies_on_exec() then narrows it to the table programs.  Driver/data
 * children (.SDR/.BIN/.MOD) leave the state unchanged so a table keeps the
 * fix across EXECing its own sound driver.
 */
#include "pfemu.h"

static int session_armed = 0;   /* user booted Fantasies (launcher or CLI equiv) */
static int fix_on = 0;          /* a TABLE1-4.PRG is currently running */
static char session_dir[512];   /* game directory, for the options file below */
static int table_num = 0;              /* 1-4 while a table is running, else 0 */
static uint32_t table_seg_base = 0;    /* linear address of that table's own CS */
static int cheat_balls_default = 0;    /* launcher: apply infinite balls on table load */
static int cheat_spring_default = 0;   /* launcher: apply ball-control mode on table load */
static void fantasies_apply_cheat_defaults(void);  /* defined below, needs trainer_site */

/* Upper-cased basename of a guest/host path (drives, slashes handled). */
static void base_up(const char *path, char *out, size_t n){
    const char *b = path, *p;
    size_t i = 0;
    for(p = path; *p; p++) if(*p=='\\'||*p=='/'||*p==':') b = p+1;
    while(b[i] && i+1 < n){
        char c = b[i];
        out[i] = (char)(c>='a'&&c<='z' ? c-32 : c);
        i++;
    }
    out[i] = 0;
}

static int is_table_prog(const char *base){
    return strlen(base)==10 && memcmp(base,"TABLE",5)==0 &&
           base[5]>='1' && base[5]<='4' && memcmp(base+6,".PRG",4)==0;
}

/* Host-only launcher toggle file (src/launch.c writes it, checkboxes next to
 * the F5-menu options): 2 bytes, [0]=infinite balls, [1]=ball control mode,
 * nonzero = apply automatically the moment a table loads (see
 * fantasies_on_exec below).  Independent of the live '1'/'2' hotkeys below -
 * this only sets each table's *starting* state; the hotkeys still toggle
 * from there same as always.  Kept out of the 6-byte pfemu_options.cfg blob
 * since that one specifically mirrors PINBALL.CFG's own layout. */
static void load_cheat_cfg(const char *dir){
    char path[600];
    FILE *f;
    uint8_t b[2] = {0,0};
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_cheats.cfg", dir);
    f = fopen(path, "rb");
    if(f){ if(fread(b,1,2,f) != 2){ b[0]=0; b[1]=0; } fclose(f); }
    cheat_balls_default = b[0] != 0;
    cheat_spring_default = b[1] != 0;
}

/* Called once at startup after dir/prog are final (launcher or CLI).
 * dir is the game identity (the launcher maps FANTASY<->Fantasies); prog is
 * matched too so renamed install dirs and direct table boots (-p TABLE1.PRG)
 * still arm.  prog-only matches are Fantasies-exclusive filenames. */
void fantasies_begin_session(const char *dir, const char *prog){
    char db[64], pb[64];
    base_up(dir ? dir : "", db, sizeof(db));
    base_up(prog ? prog : "", pb, sizeof(pb));
    session_armed =
        !strcmp(db, "FANTASY") ||
        !strcmp(pb, "PINBALL.EXE") || !strcmp(pb, "INTRO.PRG") ||
        is_table_prog(pb);
    fix_on = 0;
    table_num = 0; table_seg_base = 0;
    snprintf(session_dir, sizeof(session_dir), "%s", dir ? dir : "");
    if(session_armed) load_cheat_cfg(session_dir);
    if(!session_armed) kbd_clear_held();
    trc("[fantasies] session %s (dir=%s prog=%s)\n",
        session_armed ? "armed" : "not armed", db, pb);
}

/* Called from dos_exec() for every program the guest (or main()) starts.
 * cs_seg is the segment dos_exec() is about to run it at (from load_mz's
 * header-relative CS, i.e. the same "load+cs" a table's own code and data
 * are addressed from) - needed so fantasies_key_event() below can turn a
 * table-relative trainer offset into a linear address. */
void fantasies_on_exec(const char *dospath, uint16_t cs_seg){
    char b[64];
    const char *dot;
    if(!session_armed || dos_no_patch){
        if(fix_on){ fix_on = 0; kbd_clear_held(); }
        table_num = 0; table_seg_base = 0;
        return;
    }
    base_up(dospath, b, sizeof(b));
    if(is_table_prog(b)){
        if(!fix_on) trc("[fantasies] flipper fix on (%s)\n", b);
        fix_on = 1;
        table_num = b[5] - '0';
        table_seg_base = (uint32_t)cs_seg * 16;
        fantasies_apply_cheat_defaults();
        return;
    }
    dot = strrchr(b,'.');
    if(dot && (!strcmp(dot,".SDR")||!strcmp(dot,".BIN")||!strcmp(dot,".MOD")))
        return;                         /* child driver/data: keep state */
    if(dot && (!strcmp(dot,".PRG")||!strcmp(dot,".EXE")||!strcmp(dot,".COM"))){
        if(fix_on) trc("[fantasies] flipper fix off (%s)\n", b);
        fix_on = 0;
        table_num = 0; table_seg_base = 0;
        kbd_clear_held();
    }
}

int fantasies_fix_active(void){
    return session_armed && fix_on && !dos_no_patch;
}

int fantasies_session_armed(void){
    return session_armed;
}

/* Sound-driver PLL seed preset.
 *
 * DISABLED (not called from dos.c): the pass-1 skip below pokes a preset
 * result into a memory cell addressed by a segment immediate baked into the
 * .SDR file (the `push imm16 / pop ds` picked up by the `dsp`/`dsbase` search
 * a bit further down).  That segment is a fixed value from the file, not
 * derived from where this particular EXEC's driver or caller landed in
 * memory, so the poke address is only right for whichever process happens to
 * sit at that fixed spot - the intro, when it does, or a table's own memory
 * when its own .SDR load computes the same fixed address instead.  Confirmed
 * by the user as the cause of both the doc-check screen's visual corruption
 * (only with a sound driver loaded) and Table 4's instant crash: both went
 * away under -nopatch, which is what disables this function.  Kept here,
 * unused, because the reverse-engineered signature/offsets are expensive to
 * redo; re-enabling needs the poke address validated against the actual
 * owning process's MCB block first.
 *
 * Every .SDR driver calibrates the PIT tick to the monitor at load: starting
 * from DI=0x1CE8 it counts 3DAh bit-0 edges per mode-0 one-shot and walks the
 * reload up geometrically until the count matches ten times (locks ~19771
 * here, ~19921 in an earlier run - a narrow band, not a constant).  That
 * climb is ~110 rounds x ~17ms ~= 2.2s of the boot black screen, and it
 * repeats on every table switch (each table EXECs its own driver).
 *
 * Under emulation the "monitor" is computed from CRTC registers the game
 * wrote itself, so the lock band is fixed: seeding DI inside it skips the
 * climb and the loop accepts within a few rounds (~0.2s).  The seed lives in
 * the driver's init prologue, byte-identical in all 11 .SDR files (only the
 * cs:[] data ref varies - 2 wildcard bytes), so one signature covers the
 * family; anything without the signature keeps the slow path.  Patched in
 * the loaded image only (-nopatch disables), Fantasies sessions only: other
 * games reuse the .SDR/SOUND.CFG family, and their drivers must be left
 * alone. */
static uint16_t img_u16(uint32_t load_base, uint32_t o){
    return (uint16_t)(ram[load_base + o] | (ram[load_base + o + 1] << 8));
}

void fantasies_patch_sdr(uint32_t load_base, uint32_t imglen){
    /* mov ax,cs / xchg es:[22h],ax / mov cs:[????],ax / mov di,1CE8 /
     * push es / mov dx,40h / mov es,dx / mov dx,es:[63h] / add dl,6 */
    static const uint8_t sig[28] = {
        0x8C,0xC8, 0x26,0x87,0x06,0x22,0x00, 0x2E,0xA3,0,0,
        0xBF,0xE8,0x1C, 0x06, 0xBA,0x40,0x00, 0x8E,0xC2,
        0x26,0x8B,0x16,0x63,0x00, 0x80,0xC2,0x06 };
    static const uint8_t mask[28] = {
        1,1, 1,1,1,1,1, 1,1,0,0, 1,1,1, 1, 1,1,1, 1,1,
        1,1,1,1,1, 1,1,1 };
    uint32_t i, p;
    uint16_t tcell;
    int32_t p_found = -1;
    if(!session_armed || dos_no_patch || imglen < sizeof(sig)) return;
    if(load_base + imglen > RAM_SIZE) return;
    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at + k] != sig[k]) break;
        if(k == sizeof(sig)){ p_found = (int32_t)i; break; }
    }
    if(p_found < 0) return;
    p = (uint32_t)p_found;
    /* +11: imm16 of mov di,1CE8 -> 0x4D3B (19771, measured lock) */
    ram[load_base + p + 12] = 0x3B; ram[load_base + p + 13] = 0x4D;
    trc("[fantasies] sdr pll seed 1CE8 -> 4D3B at image+0x%X\n", p + 11);

    /* Skip the first (zero-target) calibration pass.  Driver init runs the
     * shared routine twice: pass 1 with target 0 (returns di~=1, the
     * measurement floor, saved for the tempo ratio) and pass 2 with the real
     * frame-line target (locks the PIT reload).  Pass 1's sweep is pure
     * overhead, and its result only needs to be what it always is (~1, the
     * natural zero-dither lock, which also varies run to run): NOP the call
     * and the di save, and preset the save cell to 1.  Everything is derived
     * from the image itself - the target cell from `cmp bp,[T]` after the
     * seed, the init pattern `mov [T],0 / call cal / mov [D],cx / call`
     * (D != T, call landing in the seed routine's entry), the poke address
     * from the `push dsbase / pop ds` just before.  Any check fails -> slow
     * path (ADLIB/INTERNAL/THING keep it). */
    tcell = 0;
    for(i = p; i + 4 <= imglen && i < p + 0xA0; i++){
        if(ram[load_base + i] == 0x3B && ram[load_base + i + 1] == 0x2E){
            tcell = img_u16(load_base, i + 2);
            break;
        }
    }
    if(!tcell) return;
    for(i = 0; i + 16 <= imglen; i++){
        uint32_t at = load_base + i;
        uint16_t dcell, dsbase, rel;
        int32_t tgt;
        uint32_t j, nds = 0, dsp = 0;
        uint32_t poke;
        if(ram[at] != 0xC7 || ram[at+1] != 0x06 ||
           ram[at+2] != (tcell & 0xFF) || ram[at+3] != (tcell >> 8) ||
           ram[at+4] != 0x00 || ram[at+5] != 0x00) continue;
        if(ram[at+6] != 0xE8 || ram[at+9] != 0x89 || ram[at+10] != 0x0E ||
           ram[at+13] != 0xE8) continue;
        dcell = img_u16(load_base, i + 11);
        if(dcell == tcell) continue;
        rel = img_u16(load_base, i + 7);
        tgt = (int32_t)(i + 9) + (rel >= 0x8000 ? (int32_t)rel - 0x10000 : (int32_t)rel);
        if(tgt < (int32_t)p - 0x60 || tgt > (int32_t)p) continue;
        for(j = (i > 40 ? i - 40 : 0); j + 3 < i + 1 && j + 4 <= imglen; j++){
            if(ram[load_base + j] == 0x68 && ram[load_base + j + 3] == 0x1F){
                nds++; dsp = j;
            }
        }
        if(nds != 1) continue;
        dsbase = img_u16(load_base, dsp + 1);
        poke = (uint32_t)dsbase * 16 + dcell;
        if(poke + 1 >= RAM_SIZE) continue;
        ram[at+6] = 0x90; ram[at+7] = 0x90; ram[at+8] = 0x90;
        ram[at+9] = 0x90; ram[at+10] = 0x90; ram[at+11] = 0x90; ram[at+12] = 0x90;
        ram[poke] = 0x01; ram[poke+1] = 0x00;
        trc("[fantasies] sdr pass-1 calibration skipped at image+0x%X"
            " ([%04X] preset 1) load_base=%05X dsbase=%04X poke_lin=%05X\n",
            i + 6, dcell, load_base, dsbase, poke);
        return;
    }
}

/* INTRO.PRG's own "have I already passed the manual check" flag, read once
 * at boot: it opens Intro.Mod, seeks to file offset 252868 (the last two
 * bytes of a 252,870-byte file - two bytes of the last music sample, per
 * WRITEUP-PHASE2.md) and reads them (traced live: open/seek(0x3DBC4)/read(2)
 * back to back, image offset 0x36643, nowhere near the sound driver's own
 * bulk reads of the same file).  If they read back as the "passed" sentinel
 * the screen never appears; if it correctly plays through and is answered,
 * INTRO.PRG rewrites those two bytes to the sentinel so future boots skip
 * it.  Confirmed by direct experiment: the shipped file's real tail is
 * 2B 3F; after passing the check once (back when a CRACK.COM-style JNC->JMP
 * edit in the loaded image forced acceptance of whatever was typed - see
 * WRITEUP-PHASE2.md §5.13/§5.13.1) the write-overlay copy's tail reads
 * 20 01; deleting that copy so INTRO.PRG sees the original 2B 3F again
 * reproduces the screen, and passing it once more rewrites the identical
 * 20 01 - so it is a fixed sentinel, not a checksum of what was typed.
 *
 * Forcing every read of exactly those two bytes to 20 01 makes INTRO.PRG
 * believe the check already passed before it ever draws the screen, on a
 * pristine INTRO.MOD or otherwise - no prompt, and nothing gets written
 * back, since the code path that writes the sentinel is inside the screen
 * that now never runs.  Replaces the old image patch entirely (removed). */
void fantasies_filter_read(const char *fname, long pos, uint8_t *buf, int len){
    char up[16];
    if(!session_armed || dos_no_patch || len != 2 || pos != 252868) return;
    base_up(fname, up, sizeof(up));
    if(strcmp(up, "INTRO.MOD")) return;
    buf[0] = 0x20; buf[1] = 0x01;
    trc("[fantasies] manual check flag forced to 'answered' (Intro.Mod+252868)\n");
}

/* Launcher options, poked straight into INTRO.PRG's memory instead of going
 * through PINBALL.CFG on disk.
 *
 * Why: INTRO.PRG reads PINBALL.CFG once at boot (DS:49A3, 6 bytes - the same
 * buffer the F5 options menu edits and the intro-to-table handoff writes
 * back, WRITEUP-PHASE2.md Sec 2.2/5.13.1).  Confirmed by direct A/B testing
 * (many repeated table loads, unmodified emulator, nothing else changed):
 * whenever that boot-time read finds a file and succeeds, the extra DOS
 * calls it costs (open+read+close vs. the single failed open of a fresh
 * install) are enough to reliably keep the table's own sound-driver PLL
 * calibration from ever converging - the reload search just never locks,
 * regardless of what's actually in the file.  A fresh install, where the
 * open always fails, never does this.  (A watchdog that detected the wedge
 * from inside vga_status1/pit_write and kicked the emulated clock forward
 * was tried; A/B testing showed it made things *worse* than doing nothing -
 * it was tripping on legitimate PIT activity and destabilising boots that
 * would have converged fine on their own - so it was removed rather than
 * tuned further.  This interception is the actual, measured fix.)  So
 * rather than writing PINBALL.CFG and hoping the resonance doesn't hit, the
 * launcher writes its choices to a host-only file DOS never sees, and this
 * makes every boot-time PINBALL.CFG open fail exactly like a fresh install
 * always has - while poking the launcher's values into the same buffer the
 * moment that open would have happened, so the menu (if opened) and the
 * table handoff both see them.
 * Zero extra guest instructions either way, so boot timing is identical to
 * the one case already proven reliable.
 *
 * Byte layout (index = display order in the F5 menu, confirmed empirically
 * by dumping this buffer against what the menu showed for each):
 *   0 Balls        0=3      1=5
 *   1 Angle        0=High   1=Low
 *   2 Scrolling    0=Hard   1=Medium  2=Soft
 *   3 Ingame Music 0=On     1=Off
 *   4 Resolution   0=Normal 1=High
 *   5 Color Mode   0=Color  1=Mono
 * This is also the game's own hardcoded default (options_cache below). */
#define PINBALL_CFG_BUF_OFFSET 0x49A3
static uint8_t options_cache[6] = {0,0,1,0,0,0};
static int options_loaded = 0;

static void load_options_cache(void){
    static const uint8_t defaults[6] = {0,0,1,0,0,0};
    char path[600];
    FILE *f;
    options_loaded = 1;
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_options.cfg", session_dir);
    f = fopen(path, "rb");
    if(!f) return;
    if(fread(options_cache, 1, 6, f) != 6)
        memcpy(options_cache, defaults, 6);
    fclose(f);
}

int fantasies_intercept_cfg_open(const char *fname){
    char up[16];
    uint32_t a;
    int i;
    if(!session_armed || dos_no_patch) return 0;
    base_up(fname, up, sizeof(up));
    if(strcmp(up, "PINBALL.CFG")) return 0;
    if(!options_loaded) load_options_cache();
    a = cpu.sbase[S_DS] + PINBALL_CFG_BUF_OFFSET;
    for(i=0;i<6;i++) mem_w8(a+(uint32_t)i, options_cache[i]);
    trc("[fantasies] options poked at %05X: %02X %02X %02X %02X %02X %02X\n", a,
        options_cache[0],options_cache[1],options_cache[2],
        options_cache[3],options_cache[4],options_cache[5]);
    return 1;
}

/* Scrolling gets clobbered by INTRO.PRG's own missing-config fallback.
 * INTRO.ASM, right after the (always-failing, per
 * fantasies_intercept_cfg_open above) boot-time load:
 *   CALL LOAD_TOGGLAREN
 *   JNC  TOGGLAREN_READY
 *   MOV  TOGGLAREN.S_SCROLLING,1     ; <- only this one field
 *   TOGGLAREN_READY:
 * OPENFILE/READFILE never touch AH/DX flags, so the real INT 21h carry from
 * our forced-failure open reaches this JNC untouched, and it always takes
 * the "no config file" branch - the same branch a genuine fresh install
 * takes.  That branch only defaults S_SCROLLING (the game trusts zeroed
 * memory for the other five fields), so it silently overwrites whatever
 * Scrolling value fantasies_intercept_cfg_open just poked, every boot,
 * regardless of what the launcher chose - and it does it before the F5 menu
 * is ever drawn, so the menu shows the wrong value too, not just the table.
 * (An earlier version of this fix re-corrected the byte only at the
 * intro-to-table handoff write, which fixed what the table saw but left the
 * menu always showing Medium - the menu reads TOGGLAREN long before that
 * write happens.  NOPing the clobber instead fixes both, since Scrolling is
 * then simply never touched again after the initial poke.)
 *
 * Signature: JNC +5 (73 05) immediately followed by MOV byte ptr
 * [imm16],1 (C6 06 lo hi 01) - the "TOGGLAREN_READY:" skip and the clobber
 * it guards, tied together by the displacement (5) exactly matching the
 * 5-byte instruction it jumps over.  Confirmed unique in the shipped
 * INTRO.PRG by static byte-scan (one match, target 0x49A5 = the already-
 * confirmed TOGGLAREN base 0x49A3 + 2 = S_SCROLLING).  NOPs just the MOV;
 * the JNC is left alone, since either branch now falls into the same
 * do-nothing bytes. */
void fantasies_patch_intro(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t sig[7] = {0x73,0x05,0xC6,0x06,0,0,0x01};
    static const uint8_t mask[7] = {1,1,1,1,0,0,1};
    char b[64];
    uint32_t i, k;
    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(strcmp(b, "INTRO.PRG")) return;
    if(load_base + imglen > RAM_SIZE) return;
    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at+k] != sig[k]) break;
        if(k == sizeof(sig)){
            uint32_t mov_at = at + 2;
            ram[mov_at]=0x90; ram[mov_at+1]=0x90; ram[mov_at+2]=0x90;
            ram[mov_at+3]=0x90; ram[mov_at+4]=0x90;
            trc("[fantasies] scrolling-default clobber NOPed at image+0x%X\n", i+2);
            return;
        }
    }
}

/* Pause/resume VBLANK<->raster-interrupt handshake race ("ball stuck after
 * pause" in the original team's own preserved TODO list, FANTASIE.ASM's
 * header COMMENT block: "?BALL STUCK AFTER PAUSE").
 *
 * Ball physics is split across two independently-firing interrupts that
 * hand off to each other once per frame via a single shared flag,
 * LAST_WAS_VB: VBLANK_INT does phase 1 and sets it TRUE ("phase 2 owed");
 * LATE_RASTER_INTERRUPT, firing later the same frame, only runs phase 2 if
 * it's TRUE, and clears it back to FALSE when done. Both handlers bail out
 * immediately if INTERRUPTS_ON is FALSE - the software gate checkpause()
 * uses to freeze gameplay while paused - but LATE_RASTER_INTERRUPT checks
 * INTERRUPTS_ON *before* it would ever reach the LAST_WAS_VB=FALSE reset.
 * Pause is requested from the foreground main loop, asynchronously to both
 * interrupts, so this interleaving is possible on real hardware (and under
 * any emulator that fires timer IRQs asynchronously to the CPU stream,
 * i.e. any correct one):
 *
 *   1. VBLANK_INT runs to completion, sets LAST_WAS_VB=TRUE.
 *   2. The main loop processes the pause keystroke before this frame's
 *      LATE_RASTER_INTERRUPT has fired; checkpause() sets INTERRUPTS_ON=
 *      FALSE and busy-waits.
 *   3. LATE_RASTER_INTERRUPT fires, sees INTERRUPTS_ON=FALSE, bails before
 *      ever reaching the LAST_WAS_VB=FALSE reset. The flag is now wedged
 *      TRUE for the whole pause (harmless while paused - both handlers keep
 *      bailing on the same check).
 *   4. On resume, VBLANK_INT's own "is phase 2 still owed?" check
 *      (CMP LAST_WAS_VB,TRUE / JE bail) makes it skip phase 1 for that
 *      frame. The next LATE_RASTER_INTERRUPT normally self-heals - TRUE is
 *      what lets *it* proceed, and it clears the flag when done - unless
 *      that one recovery tick is itself gated off (re-pausing inside the
 *      same narrow window, or the SLOWCNT frame-skip throttle landing on
 *      it), in which case the wedge persists another full cycle and can
 *      compound.
 *
 * Fix: rather than editing the game's code (which would mean inserting
 * bytes, not just flipping ones already there), poke LAST_WAS_VB back to
 * FALSE ourselves the instant pause ends, so the next VBLANK_INT always
 * finds a clean handshake. PAUSEFLAG, not INTERRUPTS_ON, is the trigger:
 * it is TRUE for exactly the pause's duration (set by the pause key, and
 * cleared only by checkpause()'s own resume paths) and touched nowhere
 * else in the engine. INTERRUPTS_ON is also saved/cleared/restored around a
 * handful of short, unrelated critical sections elsewhere (the light-
 * flashing code), which could otherwise produce a spurious FALSE->TRUE blip
 * unrelated to pause and trip this fix at the wrong moment.
 *
 * Byte offsets for INTERRUPTS_ON/PAUSEFLAG (DS-relative, in the engine's
 * shared DATA segment) and LAST_WAS_VB (CS-relative, a data byte living
 * inline in CODE next to VBLANK_INT/LATE_RASTER_INTERRUPT) are found by
 * signature, not hardcoded: PAUSEFLAG's offset is read straight out of
 * checkpause()'s own resume sequence, and LAST_WAS_VB's out of
 * LATE_RASTER_INTERRUPT's normal-completion tail. DATA's own runtime
 * segment is recovered the way fantasies_patch_sdr() already recovers a
 * driver's segment: by majority vote over every `push imm16 / pop ds`
 * (68 xx xx 1F) site in the image - DATA is by far the most common target,
 * used by nearly every routine in the engine. All three signatures were
 * verified against the shipped TABLE1-4.PRG (identical shape in all four;
 * the actual offsets differ table to table, as expected since each table's
 * own code/data size shifts everything after it). */
static uint32_t pause_addr_pauseflag[5]   = {0,0,0,0,0};  /* index = table_num */
static uint32_t pause_addr_last_was_vb[5] = {0,0,0,0,0};
static int      pause_prev_flag[5]        = {0,0,0,0,0};

void fantasies_patch_pause(const char *dospath, uint32_t load_base, uint32_t imglen){
    /* mov cx,0 / mov ax,4 / mov bx,3 / int 66h / IF_ERROR 1,OUTOFMEMORY /
     * IF_ERROR 2,INIT_ERR / call RESTORE_AFTER_PAUSE /
     * mov interrupts_on,true / mov pauseflag,false  (checkpause() resume) */
    static const uint8_t sigA[44] = {
        0xB9,0x00,0x00, 0xB8,0x04,0x00, 0xBB,0x03,0x00, 0xCD,0x66,
        0x3C,0x01,0x75,0x06,0x90,0x90,0x90,0xE9,0x00,0x00,
        0x3C,0x02,0x75,0x06,0x90,0x90,0x90,0xE9,0x00,0x00,
        0xE8,0x00,0x00,
        0xC6,0x06,0x00,0x00,0xFF,
        0xC6,0x06,0x00,0x00,0x00 };
    static const uint8_t maskA[44] = {
        1,1,1, 1,1,1, 1,1,1, 1,1,
        1,1,1,1,1,1,1,1,0,0,
        1,1,1,1,1,1,1,1,0,0,
        1,0,0,
        1,1,0,0,1,
        1,1,0,0,1 };
    /* mov cs:last_was_vb,false / mov inside_rastint,false / mov ax,12345 /
     * retf  (LATE_RASTER_INTERRUPT's normal-completion tail) */
    static const uint8_t sigB[15] = {
        0x2E,0xC6,0x06,0x00,0x00,0x00,
        0xC6,0x06,0x00,0x00,0x00,
        0xB8,0x39,0x30, 0xCB };
    static const uint8_t maskB[15] = {
        1,1,1,0,0,1,
        1,1,0,0,1,
        1,1,1, 1 };
    char b[64];
    uint32_t i, pa = (uint32_t)-1, pb = (uint32_t)-1;
    uint16_t off_pf, off_lwv;
    uint16_t cand_val[64]; uint32_t cand_cnt[64]; int ncand = 0;
    uint32_t data_seg = 0, best = 0, cs_base;
    int tn;

    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    tn = b[5] - '0';
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sigA)) return;

    for(i = 0; i + sizeof(sigA) <= imglen; i++){
        uint32_t at = load_base + i, k2;
        for(k2 = 0; k2 < sizeof(sigA); k2++)
            if(maskA[k2] && ram[at+k2] != sigA[k2]) break;
        if(k2 == sizeof(sigA)){ pa = i; break; }
    }
    for(i = 0; i + sizeof(sigB) <= imglen; i++){
        uint32_t at = load_base + i, k2;
        for(k2 = 0; k2 < sizeof(sigB); k2++)
            if(maskB[k2] && ram[at+k2] != sigB[k2]) break;
        if(k2 == sizeof(sigB)){ pb = i; break; }
    }
    if(pa == (uint32_t)-1 || pb == (uint32_t)-1){
        trc("[fantasies] pause-race fix: signature not found (table %d), leaving unpatched\n", tn);
        return;
    }
    off_pf  = img_u16(load_base, pa + 41);
    off_lwv = img_u16(load_base, pb + 3);

    for(i = 0; i + 4 <= imglen; i++){
        if(ram[load_base+i] == 0x68 && ram[load_base+i+3] == 0x1F){
            uint16_t v = img_u16(load_base, i+1);
            int j;
            for(j = 0; j < ncand; j++) if(cand_val[j] == v) break;
            if(j == ncand){ if(ncand < 64){ cand_val[ncand]=v; cand_cnt[ncand]=1; ncand++; } }
            else cand_cnt[j]++;
        }
    }
    for(i = 0; i < (uint32_t)ncand; i++)
        if(cand_cnt[i] > best){ best = cand_cnt[i]; data_seg = cand_val[i]; }
    if(!data_seg || best < 8){
        trc("[fantasies] pause-race fix: DATA segment not confidently found (table %d), leaving unpatched\n", tn);
        return;
    }

    /* CS-relative: table_seg_base isn't valid yet at this call site (it's
     * only updated by fantasies_on_exec(), which runs after load_mz()
     * returns) - derive it locally the same way dos_exec() will: the
     * shipped TABLE1-4.PRG all use header.cs=0x10 (a 0x100-byte fake-PSP
     * prefix inside the load image, per fantasies_patch_sdr()'s own comment
     * above), so cs_seg*16 == load_base + 0x100. */
    cs_base = load_base + 0x100;

    pause_addr_pauseflag[tn]   = data_seg*16 + off_pf;
    pause_addr_last_was_vb[tn] = cs_base + off_lwv;
    pause_prev_flag[tn] = 0;
    trc("[fantasies] pause-race fix armed (table %d): pauseflag=%05X last_was_vb=%05X\n",
        tn, pause_addr_pauseflag[tn], pause_addr_last_was_vb[tn]);
}

/* Called from dev_tick() - far more often than once per emulated video
 * frame. Polls PAUSEFLAG for a TRUE->FALSE edge (pause just ended) and
 * forces LAST_WAS_VB back to FALSE at that instant, so the very next
 * VBLANK_INT always finds a clean handshake instead of possibly finding it
 * wedged TRUE from before the pause. See the race writeup above
 * fantasies_patch_pause(). */
void fantasies_pause_tick(void){
    uint32_t a_pf, a_lwv;
    int cur;
    if(!fantasies_fix_active() || !table_num) return;
    a_pf  = pause_addr_pauseflag[table_num];
    a_lwv = pause_addr_last_was_vb[table_num];
    if(!a_pf || !a_lwv) return;
    cur = mem_r8(a_pf) != 0;
    if(pause_prev_flag[table_num] && !cur){
        mem_w8(a_lwv, 0);
        trc("[fantasies] pause ended: last_was_vb forced clear (table %d)\n", table_num);
    }
    pause_prev_flag[table_num] = cur;
}

/* Trainer hotkeys, ported from trainer/PINTRN.COM (RAZOR DoX, 1994).
 *
 * That trainer is a packed real-mode TSR: it decompresses itself, waits for
 * a keypress on its own banner, then hooks INT 33h (mouse) so that the very
 * first AX=0 (mouse reset) call the game makes - which happens after the
 * game has already installed its own direct, DOS-bypassing INT 9 handler,
 * per FANTASIE.ASM - lets it read that INT 9 vector's segment and learn
 * where the running table lives in memory, then splices its own INT 9
 * handler in front of the game's.  From there it watches port 60h on every
 * keystroke for two raw make codes and pokes the table directly:
 *   02 ('1') - infinite balls: the table's own "dec byte ptr [balls_left]"
 *       (opcode bytes FE 06 <lo> <hi>) NOPed out to a run of four 90s.
 *   03 ('2') - "spring mode": a flag byte toggled between 00 and FF - once
 *       set, the down-arrow (plunger) key keeps working at any point in
 *       play, not just while the ball sits on the spring.
 * Reverse-engineered by unpacking PINTRN.COM's self-decompressing stub and
 * tracing the unpacked image (its own INT 9 handler, offset 0x180 in the
 * unpacked file) under pfemu; both the balls-patch site and its "already
 * patched" NOP pattern, and the exact 4-byte original bytes (including the
 * counter address in the 2nd/3rd byte) the trainer restores on toggle-off,
 * were confirmed byte-for-byte by reading them straight out of the shipped
 * TABLE1-4.PRG.  These .PRG are COM-style images wrapped in a minimal EXE
 * (0x200-byte header, header cs=0x10 i.e. a 0x100-byte fake PSP prefix
 * inside the load image - see load_mz()), so "CS-relative offset X" here
 * means linear = table_seg_base + X, table_seg_base being (load+cs)*16 for
 * whichever TABLEn.PRG is currently running (fantasies_on_exec() records
 * it).  The spring-mode flag site wasn't independently byte-verified (it's
 * a plain data byte, not a recognisable instruction) - only inferred from
 * being listed in the same table order as the balls site in PINTRN.COM - so
 * it's marked lower-confidence than the balls patch, though both were
 * ported unchanged from the trainer's own tables. */
typedef struct {
    uint16_t balls_off;      /* site of "dec byte ptr [balls_left]" (FE 06 lo hi) */
    uint16_t balls_counter;  /* that instruction's operand: the counter's own offset */
    uint16_t spring_off;     /* ball-on-spring control-mode flag byte (00/FF) */
} TrainerSite;
static const TrainerSite trainer_site[5] = {
    {0,0,0},                     /* unused */
    {0x0AED, 0x33DE, 0x25C7},    /* TABLE1.PRG */
    {0x0A04, 0x33A4, 0x2170},    /* TABLE2.PRG */
    {0x0994, 0x2C6E, 0x10B1},    /* TABLE3.PRG */
    {0x0AD1, 0x38F4, 0x2A02},    /* TABLE4.PRG */
};

static void fantasies_toggle_balls(void){
    const TrainerSite *s = &trainer_site[table_num];
    uint32_t a = table_seg_base + s->balls_off;
    if(a + 4 > RAM_SIZE) return;
    if(ram[a]==0xFE && ram[a+1]==0x06){
        ram[a]=0x90; ram[a+1]=0x90; ram[a+2]=0x90; ram[a+3]=0x90;
        trc("[fantasies] infinite balls ON (table %d)\n", table_num);
    } else if(ram[a]==0x90 && ram[a+1]==0x90 && ram[a+2]==0x90 && ram[a+3]==0x90){
        ram[a]=0xFE; ram[a+1]=0x06;
        ram[a+2]=(uint8_t)(s->balls_counter & 0xFF);
        ram[a+3]=(uint8_t)(s->balls_counter >> 8);
        trc("[fantasies] infinite balls OFF (table %d)\n", table_num);
    }
}

static void fantasies_toggle_spring(void){
    uint32_t a = table_seg_base + trainer_site[table_num].spring_off;
    if(a >= RAM_SIZE) return;
    if(ram[a]==0x00){
        ram[a]=0xFF;
        trc("[fantasies] spring/ball-control mode ON (table %d)\n", table_num);
    } else if(ram[a]==0xFF){
        ram[a]=0x00;
        trc("[fantasies] spring/ball-control mode OFF (table %d)\n", table_num);
    }
}

/* Called from dev.c's kbd_key() on every fresh (non-autorepeat) key make,
 * host scancode already stripped of the E0 prefix bit. '1' and '2' are
 * plain, unextended scancodes, so no E0 check is needed here. */
void fantasies_key_event(int scancode, int down){
    if(!down || !fantasies_fix_active() || !table_num || dos_no_patch) return;
    if(scancode == 0x02) fantasies_toggle_balls();
    else if(scancode == 0x03) fantasies_toggle_spring();
}

/* Called from fantasies_on_exec() the moment a table finishes loading: apply
 * whichever cheats the launcher's checkboxes turned on, as that table's
 * starting state (a fresh load is always unpatched, so no need to check
 * current bytes first - just go straight to the "on" branch each toggle
 * would have taken).  The '1'/'2' hotkeys work the same as always on top of
 * this - it only sets where each table starts out. */
static void fantasies_apply_cheat_defaults(void){
    if(dos_no_patch || !table_num) return;
    if(cheat_balls_default) fantasies_toggle_balls();
    if(cheat_spring_default) fantasies_toggle_spring();
}
