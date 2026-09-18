/* Pinball Fantasies game-specific fixes.
 *
 * Everything that applies to Fantasies (and only Fantasies) lives in this
 * file, so dos.c/dev.c stay generic:
 *
 *  - the intro's manual-lookup image patch (memory-only, signature-checked,
 *    -nopatch disables);
 *  - the flipper fix session/exec gating (see below);
 *  - the trainer hotkeys ('1'-'3', Z and '/' - see fantasies_key_event
 *    below);
 *  - Pinball Fantasies Deluxe's (1995 CD-ROM release) boot-time CD-present
 *    check, faked so a from-disk-only install reads as "CD found" (see
 *    fantasies_open_cdmarker below).
 *
 * Flipper background: the tables drive each flipper from a single level bit
 * shared by 3 keys (left: 2A/38/1D, right: 36/E0-38/E0-1D; FANTASIE.ASM KEYINT
 * DSSK/DSSKR, live TABLE1.PRG image 0x3F34).  A lost break byte leaves the bit
 * set, so the flipper stays up until the next make+break cycle.  dev.c repairs
 * the host-side break-loss paths, but only while fantasies_fix_active().
 *
 * Session gating: the fix must only engage when the user actually booted
 * Pinball Fantasies.  Matching on the EXEC'd filename alone (TABLE1.PRG) is
 * not enough - a sibling game could ship a same-named file, and the 1993
 * demo ships this game's own table under a different name (PLAND.PRG).
 * The detected release's program layout resolves both.  So main() arms
 * this session from the launcher choice (or its CLI equivalent: -d/-p), and
 * fantasies_on_exec() then narrows it to the table programs.  Driver/data
 * children (.SDR/.BIN/.MOD) leave the state unchanged so a table keeps the
 * fix across EXECing its own sound driver.
 */
#include "pfemu.h"

extern double emu_time;

static int session_armed = 0;   /* a recognised Fantasies release is booting */
static const Release *rel = NULL;  /* which one - see src/release.c */
static int fix_on = 0;          /* one of the release's tables is running */
static uint32_t cfg_buf = 0;    /* intro options struct, DS offset; 0 = none */
static char session_dir[512];   /* game directory, for the options file below */
static int table_num = 0;              /* 1-4 while a table is running, else 0 */
static int trainer_enabled = 0;        /* launcher: arm the '1'-'3'/'Z' hotkeys below */
static int spring_cheat_on = 0;        /* our own state for the '2' toggle - see below */

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

/* Which of the release's programs this is: 0 = the intro, 1..4 = that table,
 * -1 = something else.  The names are not a constant - the 1993 demo ships
 * DEMO.PRG and PLAND.PRG in place of INTRO.PRG and TABLE1.PRG - so the
 * detected release's own layout answers it (src/release.c).  Every caller
 * below has already checked session_armed, which is what guarantees `rel`;
 * asking without one is answered "not a program of this release" rather than
 * by falling back to a guess. */
static int prog_slot(const char *base){
    return release_prog_slot(rel, base);
}

static int is_table_prog(const char *base){
    return prog_slot(base) >= 1;
}

/* The launcher's "Enable trainer" checkbox, from the install's settings file
 * (PFEMU-STATE/pfemu.cfg, src/cfg.c).  On just ARMS the '1'/'2' hotkeys in
 * fantasies_key_event() below; it does not itself turn either cheat on.
 * Every fresh table load is the game's own unpatched image (see
 * fantasies_on_exec below - it no longer pre-applies anything), so with the
 * trainer enabled, the cheats still start OFF and stay off until the
 * player actually presses one of the hotkeys.  Its own key rather than one
 * of the six option bytes, since those specifically mirror PINBALL.CFG's
 * own layout. */
static void load_cheat_cfg(const char *dir){
    PfCfg c;
    cfg_read(dir, &c);
    trainer_enabled = c.trainer;
}

/* Called once at startup after dir/prog are final (launcher or CLI).
 *
 * Arming is now a consequence of release detection and nothing else: main()
 * hashes the directory's five program files and hands over the matching
 * release descriptor (src/release.c), or NULL.  The old rule - arm on the
 * directory basename FANTASY, or on a Fantasies-exclusive program name like
 * PINBALL.EXE/INTRO.PRG/TABLEn.PRG - could not survive three releases: Power
 * Pack calls its launcher PF.EXE and so armed nothing at all, while the
 * options poke below needs to know *which* intro build it is writing into.
 * A name cannot answer that; a hash can.  Direct boots of INTRO.PRG or a
 * table still arm, because the directory they live in is what gets detected,
 * not the program that was asked for. */
void fantasies_begin_session(const char *dir, const char *prog, const Release *r){
    char db[64], pb[64];
    base_up(dir ? dir : "", db, sizeof(db));
    base_up(prog ? prog : "", pb, sizeof(pb));
    rel = r;
    session_armed = (r != NULL);
    fix_on = 0;
    table_num = 0;
    cfg_buf = 0;
    snprintf(session_dir, sizeof(session_dir), "%s", dir ? dir : "");
    if(session_armed) load_cheat_cfg(session_dir);
    if(!session_armed) kbd_clear_held();
    trc("[fantasies] session %s (release=%s dir=%s prog=%s)\n",
        session_armed ? "armed" : "not armed",
        r ? r->id : "none", db, pb);
}

/* Called from dos_exec() for every program the guest (or main()) starts. */
void fantasies_on_exec(const char *dospath){
    char b[64];
    const char *dot;
    if(!session_armed || dos_no_patch){
        if(fix_on){ fix_on = 0; kbd_clear_held(); }
        table_num = 0;
        spring_cheat_on = 0;
        osd_clear();
        return;
    }
    base_up(dospath, b, sizeof(b));
    if(is_table_prog(b)){
        if(!fix_on) trc("[fantasies] flipper fix on (%s)\n", b);
        fix_on = 1;
        table_num = prog_slot(b);
        spring_cheat_on = 0;
        osd_clear();
        return;
    }
    dot = strrchr(b,'.');
    if(dot && (!strcmp(dot,".SDR")||!strcmp(dot,".BIN")||!strcmp(dot,".MOD")))
        return;                         /* child driver/data: keep state */
    if(dot && (!strcmp(dot,".PRG")||!strcmp(dot,".EXE")||!strcmp(dot,".COM"))){
        if(fix_on) trc("[fantasies] flipper fix off (%s)\n", b);
        fix_on = 0;
        table_num = 0;
        spring_cheat_on = 0;
        osd_clear();
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
 * EMULATOR.md) and reads them (traced live: open/seek(0x3DBC4)/read(2)
 * back to back, image offset 0x36643, nowhere near the sound driver's own
 * bulk reads of the same file).  If they read back as the "passed" sentinel
 * the screen never appears; if it correctly plays through and is answered,
 * INTRO.PRG rewrites those two bytes to the sentinel so future boots skip
 * it.  Confirmed by direct experiment: the shipped file's real tail is
 * 2B 3F; after passing the check once (back when a CRACK.COM-style JNC->JMP
 * edit in the loaded image forced acceptance of whatever was typed - see
 * docs/EMULATOR.md (copy protection)) the write-overlay copy's tail reads
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
 * Why: INTRO.PRG reads PINBALL.CFG once at boot (6 bytes into the same
 * buffer the F5 options menu edits and the intro-to-table handoff writes
 * back, docs/EMULATOR.md (launch chain, copy protection)).  Confirmed by direct A/B testing
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
 * This is also the game's own hardcoded default (options_cache below).
 *
 * WHERE that buffer is, is per release, and not by a little: DS:49A3 on the
 * floppy build, DS:4846 on Power Pack, DS:48D7 on Deluxe.  Writing the floppy
 * offset into either of the others lands in the middle of the in-memory
 * credits strings (it overwrote the tail of DESIGN and the start of
 * PROGRAMMING) while the game went on defaulting the real structure - so the
 * launcher's choices were silently lost *and* memory was corrupted.
 *
 * So the offset is derived from the loaded image instead of assumed, by
 * fantasies_patch_intro() below, and cross-checked against the detected
 * release's recorded value.  cfg_buf is 0 until that succeeds, and a 0 here
 * means no poke happens at all: an intro whose layout is not known is left
 * strictly alone.  (cfg_buf itself is declared with the other session state
 * at the top of this file, since fantasies_begin_session clears it.) */
static uint8_t options_cache[6] = {0,0,1,0,0,0};
static int options_loaded = 0;

static void load_options_cache(void){
    PfCfg c;
    options_loaded = 1;
    /* On replay the session runs the recorded blob, not the install's
     * current file (docs/REPLAY.md section 3.1): the six bytes are game
     * behavior, so a launcher change after recording must not leak in. */
    if(replay_is_replaying() && replay_recorded_options(options_cache)){
        trc("[fantasies] replay options: %02X %02X %02X %02X %02X %02X\n",
            options_cache[0],options_cache[1],options_cache[2],
            options_cache[3],options_cache[4],options_cache[5]);
        return;
    }
    cfg_read(session_dir, &c);
    memcpy(options_cache, c.options, 6);
}

int fantasies_intercept_cfg_open(const char *fname){
    char up[16];
    uint32_t a;
    int i;
    if(!session_armed || dos_no_patch) return 0;
    base_up(fname, up, sizeof(up));
    if(strcmp(up, "PINBALL.CFG")) return 0;
    /* Still fail the open even when the layout is unknown - that is the half
     * that keeps the sound driver's PLL calibration converging, and it is
     * release-independent.  Only the poke needs a known buffer. */
    if(!cfg_buf){
        trc("[fantasies] PINBALL.CFG open failed as usual, but the launcher's"
            " options were not poked (intro options buffer unknown)\n");
        return 1;
    }
    if(!options_loaded) load_options_cache();
    a = cpu.sbase[S_DS] + cfg_buf;
    for(i=0;i<6;i++) mem_w8(a+(uint32_t)i, options_cache[i]);
    trc("[fantasies] options poked at %05X: %02X %02X %02X %02X %02X %02X\n", a,
        options_cache[0],options_cache[1],options_cache[2],
        options_cache[3],options_cache[4],options_cache[5]);
    return 1;
}

/* Pinball Fantasies DELUXE (the 1995 CD-ROM release) only: PINBALL.EXE's
 * boot-time CD-present check.  That release's INSTALL.COM only copies
 * PINBALL.EXE and the sound drivers to the hard drive; INTRO.PRG/TABLE*.PRG/
 * the .MOD music stay on the CD-ROM, referenced by hardcoded absolute paths
 * (\21stcent\pfd\..., \21stcent\soundsys\... - see dos_path()'s comment).
 * INSTALL.COM's own last step runs CD_DRIVE.EXE, which writes a "cd.nfo"
 * marker into \21stcent\ on the hard drive recording which drive letter the
 * CD-ROM is on; PINBALL.EXE's very first act (CHDIR \21stcent, then a plain
 * relative OPEN "cd.nfo") is just checking that marker exists - confirmed by
 * disassembling PINBALL.EXE's boot sequence and CD_DRIVE.EXE's own strings
 * (both name "cd.nfo" and "\21stcent"). No CD drive is emulated here, so
 * that marker never gets written for real; fake its presence instead,
 * exactly like fantasies_intercept_cfg_open() above fakes PINBALL.CFG's
 * absence, so a from-disk install (this emulator's only kind) reads as
 * "CD found". The byte value itself is never a validity check - on real
 * hardware it only ever fed a "select this drive" call, and dos_path()
 * ignores drive letters entirely - so any placeholder value works. */
FILE *fantasies_open_cdmarker(const char *fname){
    char up[16];
    FILE *f;
    if(!session_armed || dos_no_patch) return NULL;
    /* Deluxe's boot program is the only one that asks.  The other two are
     * flat floppy-family layouts with no CD half, so faking the marker for
     * them would be inventing a file the release never had. */
    if(!rel || !rel->cd_marker) return NULL;
    base_up(fname, up, sizeof(up));
    if(strcmp(up, "CD.NFO")) return NULL;
    f = tmpfile();
    if(!f) return NULL;
    fputc('C', f);
    rewind(f);
    trc("[fantasies] cd.nfo presence faked (Deluxe CD-ROM check)\n");
    return f;
}

/* Everything this emulator has to know about the loaded INTRO.PRG, found in
 * the image itself rather than assumed from a table of magic numbers:
 *
 *   1. where the six-byte options structure lives (cfg_buf above);
 *   2. which of the two known "there is no PINBALL.CFG" fallbacks this build
 *      uses, and how to stop it undoing the launcher's choices.
 *
 * The detected release records both, but as a cross-check: a signature that
 * finds nothing, or finds something the release did not predict, is reported
 * and then trusted less, never papered over.  That is what keeps a fourth
 * release from quietly getting a third release's memory layout.
 *
 * --- 1. The options buffer -------------------------------------------------
 *
 * LOAD_TOGGLAREN and SAVE_TOGGLAREN both set up the same six-byte transfer:
 *
 *   MOV CX,6 ; MOV DX,<buf> ; MOV AX,3F00h ; INT 21h     (read)
 *   MOV CX,6 ; MOV DX,<buf> ; MOV AH,40h   ; INT 21h     (write)
 *
 * Scanning for those two shapes finds the buffer in every collected release
 * (floppy 49A3, Power Pack 4846, Deluxe 48D7) with exactly two matches each
 * that agree on the address.  Disagreement, or no match at all, leaves
 * cfg_buf at 0 and the poke simply does not happen.
 *
 * --- 2. The missing-config fallback ---------------------------------------
 *
 * Our forced-failure open (fantasies_intercept_cfg_open) makes LOAD_TOGGLAREN
 * return carry, exactly as a genuine fresh install does.  What the caller
 * then does differs by build, and both variants overwrite the values we just
 * poked:
 *
 * (a) The floppy build defaults one field:
 *
 *       CALL LOAD_TOGGLAREN
 *       JNC  TOGGLAREN_READY
 *       MOV  TOGGLAREN.S_SCROLLING,1     ; <- only this one
 *     TOGGLAREN_READY:
 *
 *     It trusts zeroed memory for the other five, so only Scrolling is lost -
 *     but lost before the F5 menu is ever drawn, so the menu showed the wrong
 *     value too, not just the table.  (An earlier fix re-corrected the byte
 *     at the intro-to-table handoff instead; that fixed what the table saw
 *     and left the menu permanently showing Medium.)  NOPing the MOV fixes
 *     both, since Scrolling is then never touched again after the poke.  The
 *     JNC is left alone: either branch now falls into the same bytes.
 *     Signature: JNC +5 (73 05) followed by MOV byte ptr [imm16],1
 *     (C6 06 lo hi 01), tied together by the displacement 5 exactly matching
 *     the 5-byte instruction it jumps over.  One match in the shipped file.
 *
 * (b) Power Pack and Deluxe replaced that with a range check over all six:
 *
 *       CALL LOAD_TOGGLAREN
 *       JC   defaults                    ; <- load failed
 *       CMP  S_SCROLLING,3 ; JA defaults
 *       CMP  S_BALLS,1     ; JA defaults
 *       ... four more ...
 *       JMP  ready
 *     defaults:
 *       MOV  S_SCROLLING,1 ; MOV S_BALLS,0 ; ... all six ...
 *     ready:
 *
 *     Here the forced failure costs all six values, not one.  NOPing the
 *     leading JC is what fixes it: execution falls into the range checks,
 *     which the launcher's values pass (every option it writes is in range),
 *     and the JMP at the end skips the defaults.  Values that are somehow out
 *     of range still get defaulted, which is the behaviour we want anyway.
 *     Signature: JC rel8, then CMP byte[cfg_buf+2],3, then JA/JAE rel8, then
 *     CMP byte[cfg_buf+0],1 - anchored to the buffer derived in step 1, and
 *     unique in both builds that have it.
 *
 * Which variant a release uses is recorded in its descriptor, so "no match"
 * is a warning where one was expected and an expected silence where it was
 * not.  Both scans run regardless: a release not yet in the database still
 * gets whichever of the two it actually contains. */
static int scan_sig(uint32_t base, uint32_t len, const uint8_t *sig,
                    const uint8_t *mask, uint32_t n, uint32_t *at_out){
    uint32_t i, k, hits = 0;
    for(i = 0; i + n <= len; i++){
        for(k = 0; k < n; k++)
            if(mask[k] && ram[base+i+k] != sig[k]) break;
        if(k == n){ if(!hits++) *at_out = i; }
    }
    return (int)hits;
}

/* -cfgscan: run the options-buffer scan over every program that loads, not
 * just the intro, and report what it finds.  The question it answers is
 * whether TABLE*.PRG carries the same six-byte PINBALL.CFG transfer the
 * intro does - if it does, a direct-to-table boot can have the launcher's
 * options poked into the table itself; if it does not, direct-to-table runs
 * on the game's defaults and that has to be said out loud rather than
 * discovered. */
int fantasies_cfgscan = 0;
static uint32_t cfg_sites(uint32_t base, uint32_t len, uint32_t *first, uint32_t *conflict);
void fantasies_report_cfgscan(const char *dospath, uint32_t base, uint32_t len){
    uint32_t buf = 0, n;
    if(!fantasies_cfgscan) return;
    if(base + len > RAM_SIZE) return;
    n = cfg_sites(base, len, &buf, NULL);
    if(n) fprintf(stderr, "[cfgscan] %-12s %u site(s), buffer DS:%04X\n",
                  dospath, (unsigned)n, (unsigned)buf);
    else   fprintf(stderr, "[cfgscan] %-12s no six-byte config transfer\n", dospath);
}

/* The raw scan, shared by derive_cfg_buf() and the -cfgscan report: counts
 * matching sites and hands back the first buffer address.  Returns 0 when
 * the sites disagree, which is the ambiguous case the caller must refuse. */
static uint32_t cfg_sites(uint32_t base, uint32_t len, uint32_t *first, uint32_t *conflict){
    static const uint8_t rd[11] = {0xB9,0x06,0x00,0xBA,0,0,0xB8,0x00,0x3F,0xCD,0x21};
    static const uint8_t rdm[11] = {1,1,1,1,0,0,1,1,1,1,1};
    static const uint8_t wr[10] = {0xB9,0x06,0x00,0xBA,0,0,0xB4,0x40,0xCD,0x21};
    static const uint8_t wrm[10] = {1,1,1,1,0,0,1,1,1,1};
    uint32_t i, k, found, buf = 0, n = 0;
    for(i = 0; i + sizeof(wr) <= len; i++){
        int hit = 0;
        if(i + sizeof(rd) <= len){
            for(k = 0; k < sizeof(rd); k++)
                if(rdm[k] && ram[base+i+k] != rd[k]) break;
            hit = (k == sizeof(rd));
        }
        if(!hit){
            for(k = 0; k < sizeof(wr); k++)
                if(wrm[k] && ram[base+i+k] != wr[k]) break;
            if(k < sizeof(wr)) continue;
        }
        found = (uint32_t)ram[base+i+4] | ((uint32_t)ram[base+i+5] << 8);
        if(n++ == 0) buf = found;
        else if(found != buf){
            if(first) *first = buf;
            if(conflict) *conflict = found;
            return 0;                     /* sites disagree: caller refuses */
        }
    }
    if(first) *first = buf;
    return n;
}

static void derive_cfg_buf(uint32_t base, uint32_t len){
    uint32_t buf = 0, conflict = 0, n;
    n = cfg_sites(base, len, &buf, &conflict);
    if(!n && conflict){
        trc("[fantasies] intro options buffer ambiguous "
            "(DS:%04X and DS:%04X); not poking\n", buf, conflict);
        return;
    }
    if(!n){
        /* Expected in a build that has no options menu at all - the demo's
         * intro carries neither the F5 menu nor the PINBALL.CFG behind it. */
        if(rel && !rel->cfg_buf)
            trc("[fantasies] release '%s' has no intro options buffer, as "
                "expected; nothing to poke\n", rel->id);
        else
            trc("[fantasies] intro options buffer not found; not poking\n");
        return;
    }
    /* A recorded 0 means "this build has none", so a match there is a
     * contradiction just as much as a match at the wrong address is. */
    if(rel && !rel->cfg_buf){
        trc("[fantasies] intro options buffer DS:%04X found, but release '%s' "
            "is recorded as having none; not poking\n", buf, rel->id);
        return;
    }
    if(rel && rel->cfg_buf != buf){
        trc("[fantasies] intro options buffer DS:%04X contradicts release "
            "'%s' (expects DS:%04X); not poking\n", buf, rel->id, rel->cfg_buf);
        return;
    }
    cfg_buf = buf;
    trc("[fantasies] intro options buffer DS:%04X (%u sites)\n", buf, n);
}

void fantasies_patch_intro(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t scroll[7]  = {0x73,0x05,0xC6,0x06,0,0,0x01};
    static const uint8_t scrollm[7] = {1,1,1,1,0,0,1};
    uint8_t valid[13], validm[13];
    char b[64];
    uint32_t at;
    int hits;
    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    fantasies_report_cfgscan(b, load_base, imglen);
    if(prog_slot(b) != 0) return;       /* the release's intro, whatever it is called */
    if(load_base + imglen > RAM_SIZE) return;

    derive_cfg_buf(load_base, imglen);

    hits = scan_sig(load_base, imglen, scroll, scrollm, sizeof(scroll), &at);
    if(hits == 1){
        uint32_t mov_at = load_base + at + 2;
        memset(&ram[mov_at], 0x90, 5);
        trc("[fantasies] scrolling-default clobber NOPed at image+0x%X\n", at+2);
    } else if(hits > 1){
        trc("[fantasies] scrolling-default clobber signature not unique "
            "(%d matches); left alone\n", hits);
    } else if(rel && rel->scroll_clobber){
        trc("[fantasies] WARNING: release '%s' should have the scrolling-default "
            "clobber, but its signature is absent\n", rel->id);
    }

    if(cfg_buf){
        /* JC rel8; CMP byte[cfg_buf+2],3; J(A|AE) rel8; CMP byte[cfg_buf],1 */
        memcpy(valid, "\x72\x00\x80\x3E\x00\x00\x03\x00\x00\x80\x3E\x00\x00", 13);
        memcpy(validm, "\x01\x00\x01\x01\x01\x01\x01\x00\x00\x01\x01\x01\x01", 13);
        valid[4]  = (uint8_t)(cfg_buf + 2);
        valid[5]  = (uint8_t)((cfg_buf + 2) >> 8);
        valid[11] = (uint8_t)cfg_buf;
        valid[12] = (uint8_t)(cfg_buf >> 8);
        hits = scan_sig(load_base, imglen, valid, validm, sizeof(valid), &at);
        /* The J(A|AE) byte is the one thing the two builds spell differently
         * (77 vs 73), so it is checked here rather than in the mask. */
        if(hits == 1 && ram[load_base+at+7] != 0x77 && ram[load_base+at+7] != 0x73)
            hits = 0;
        if(hits == 1){
            ram[load_base+at] = 0x90; ram[load_base+at+1] = 0x90;
            trc("[fantasies] options-default fallback bypassed at image+0x%X\n", at);
        } else if(hits > 1){
            trc("[fantasies] options-default fallback signature not unique "
                "(%d matches); left alone\n", hits);
        } else if(rel && rel->opt_validate){
            trc("[fantasies] WARNING: release '%s' should have the six-field "
                "options-default fallback, but its signature is absent\n", rel->id);
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
    tn = prog_slot(b);
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

    /* CS-relative: the shipped TABLE1-4.PRG all use header.cs=0x10 (a
     * 0x100-byte fake-PSP prefix inside the load image, per
     * fantasies_patch_sdr()'s own comment above), so the table's own CS is
     * always load_base + 0x100 regardless of the segment dos_exec() ends up
     * running it at. */
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

/* Trainer hotkeys: '1'/'2' ported from trainer/PINTRN.COM (RAZOR DoX,
 * 1994); '3'/'Z' from trainer/TRAINER.EXE (MAT's "Tripper-Trainer",
 * Jan 1994 - PKLite-packed, unpacked and traced under pfemu itself).
 * Only the cheat mechanisms travelled across - both trainers' TSR
 * machinery (INT 33h / INT 10h hooks, option menus) stays behind; pfemu
 * locates every target by signature scan instead of their fixed offsets,
 * so the cheats work across releases, not just the floppy build the two
 * trainers were written against.
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
 *   03 ('2') - forces SPRING_VALID TRUE (see fantasies_patch_spring below):
 *       the down-arrow (plunger) key keeps "launching" the ball at any point
 *       in play, not just while it's actually sitting on the spring.
 * Reverse-engineered by unpacking PINTRN.COM's self-decompressing stub and
 * tracing the unpacked image (its own INT 9 handler, offset 0x180 in the
 * unpacked file) under pfemu; the balls-patch site, its "already patched"
 * NOP pattern, and the exact 4-byte original bytes (including the counter
 * address in the 2nd/3rd byte) the trainer restores on toggle-off were all
 * confirmed byte-for-byte by reading them straight out of the shipped
 * TABLE1-4.PRG - originally as a fixed per-table offset table, since only the
 * floppy release existed yet. Located by signature instead now (see
 * fantasies_patch_balls() below), the same way fantasies_patch_pause() and
 * fantasies_patch_spring() already locate their own targets: Pinball
 * Fantasies Deluxe's TABLE1-4.PRG carry the identical instruction sequence,
 * just relocated by a couple hundred bytes (the Deluxe binaries are a
 * separate build, not merely the floppy files with a different loader glued
 * on), so a fixed offset silently missed on Deluxe - harmlessly, since
 * fantasies_toggle_balls() checks the opcode bytes before writing, but it
 * never worked there either. */
/* balls_addr[table_num] is the linear address of the "dec byte ptr
 * [balls_left]" instruction's own opcode byte (FE 06 lo hi), located by
 * fantasies_patch_balls() below; 0 = not found for that table (the '1'
 * hotkey becomes a no-op, same as an unrecognized-image fallback everywhere
 * else in this file). balls_counter_val[table_num] is that instruction's
 * operand - the counter's own linear-address low/high bytes - restored
 * verbatim when toggling the cheat back off. */
static uint32_t balls_addr[5] = {0,0,0,0,0};
static uint16_t balls_counter_val[5] = {0,0,0,0,0};

/* Locates the balls-left decrement.  FANTASIE.ASM's ball-lost handler reads
 * some other flag, jumps over the increment path (74 0A ...), then runs two
 * back-to-back "dec byte ptr [x]" stores on two different counters: the first
 * (a different flag entirely) is immediately followed by a short jump (EB),
 * the second - the one that actually counts balls left - is immediately
 * followed by a re-read of it (A0 lo hi = mov al,[same address+1]) and then a
 * compare against the just-decremented counter itself (38 06 lo hi = cmp
 * [that counter],al) guarding a "did it just hit zero" branch (77 = ja).
 * That second dec/compare pair is what PINTRN.COM's own balls-patch site
 * targets. The two flanking single-byte jump displacements and the two
 * addresses read from immediates are wildcarded; everything else (13 opcode
 * bytes) is fixed and confirmed unique by static byte-scan against all four
 * shipped TABLE1-4.PRG, floppy and Deluxe alike. */
void fantasies_patch_balls(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t sig[22] = {
        0x90,0x90,0x90,                 /* NOP padding before the ball-lost handler */
        0xFE,0x06,0,0,                  /* dec byte ptr [some other flag]  <- not this one */
        0xEB,0,                         /* jmp short +disp */
        0x90,
        0xFE,0x06,0,0,                  /* dec byte ptr [balls_left]  <- offset wanted */
        0xA0,0,0,                       /* mov al,[balls_left+1] (same counter, next byte) */
        0x38,0x06,0,0,                  /* cmp [balls_left],al */
        0x77 };                         /* ja +disp */
    static const uint8_t mask[22] = {
        1,1,1,
        1,1,0,0,
        1,0,
        1,
        1,1,0,0,
        1,0,0,
        1,1,0,0,
        1 };
    char b[64];
    uint32_t i;
    int tn;

    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    tn = prog_slot(b);
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sig)) return;

    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at+k] != sig[k]) break;
        if(k == sizeof(sig)){
            balls_addr[tn] = load_base + i + 10;
            balls_counter_val[tn] = img_u16(load_base, i + 12);
            trc("[fantasies] infinite-balls site located (table %d): %05X counter=%04X\n",
                tn, balls_addr[tn], balls_counter_val[tn]);
            return;
        }
    }
    balls_addr[tn] = 0;
    trc("[fantasies] infinite-balls fix: signature not found (table %d), leaving unpatched\n", tn);
}

/* tilt_addr[table_num] is the linear address of the nudge-accumulator
 * update's own opcode byte (83 06 lo hi 3C = add word ptr [tilt_meter],3Ch),
 * located by fantasies_patch_tilt() below; 0 = not found for that table
 * (the '3' hotkey becomes a no-op). tilt_orig[table_num] is that
 * instruction's five genuine bytes, restored verbatim on toggle-off. */
static uint32_t tilt_addr[5] = {0,0,0,0,0};
static uint8_t tilt_orig[5][5] = {
    {0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0} };

/* Locates the tilt-meter increment.  Nudging (Space) runs a guarded
 * sequence: two "already tilting/latched, skip" checks (each a
 * CMP-then-JE padded with the assembler's usual three NOPs), then
 * ADD 3Ch onto a word tilt meter, a CS-relative latch set, and a
 * CMP/JA against 78h - the third nudge in quick succession trips TILT.
 * NOPing just the 5-byte ADD leaves the meter permanently at rest, which
 * is exactly the megatrainer's (trainer/TRAINER.EXE, MAT Jan 1994)
 * "INFINITE TILTS": its per-table patch writes five 90s at this same site
 * (verified against the unpacked image - same DI in all four tables).
 * The full 37-byte shape is confirmed unique by static byte-scan against
 * every shipped TABLE1-4.PRG of the floppy, Deluxe and Power Pack
 * releases alike (one match each); the meter/latch addresses read from
 * the immediates differ per table and build, so nothing is hardcoded. */
void fantasies_patch_tilt(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t sig[37] = {
        0x80,0x3E,0,0,0xFF, 0x74,0x3E, 0x90,0x90,0x90,
        0x2E,0x80,0x3E,0,0,0xFF, 0x74,0x33, 0x90,0x90,0x90,
        0x83,0x06,0,0,0x3C, 0x2E,0xC6,0x06,0,0,0xFF,
        0x83,0x3E,0,0,0x78 };
    static const uint8_t mask[37] = {
        1,1,0,0,1, 1,1, 1,1,1,
        1,1,1,0,0,1, 1,1, 1,1,1,
        1,1,0,0,1, 1,1,1,0,0,1,
        1,1,0,0,1 };
    char b[64];
    uint32_t i;
    int tn;

    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    tn = prog_slot(b);
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sig)) return;

    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at+k] != sig[k]) break;
        if(k == sizeof(sig)){
            int j;
            tilt_addr[tn] = load_base + i + 21;
            for(j = 0; j < 5; j++) tilt_orig[tn][j] = ram[tilt_addr[tn]+(uint32_t)j];
            trc("[fantasies] infinite-tilts site located (table %d): %05X\n",
                tn, tilt_addr[tn]);
            return;
        }
    }
    tilt_addr[tn] = 0;
    trc("[fantasies] infinite-tilts fix: signature not found (table %d), leaving unpatched\n", tn);
}

/* SPRING_VALID's linear address per table, located by fantasies_patch_spring()
 * below; 0 = not found for that table (toggle becomes a no-op). */
static uint32_t spring_valid_addr[5] = {0,0,0,0,0};

/* Locates SPRING_VALID - historicalsource/pinballfantasies FANTASIE.ASM: "the
 * ball on the spring may be launched" flag, checked by SPRINGUP right before
 * it applies the down-arrow's launch speed (SETBALLSPEED) and skipped
 * straight past that when FALSE.  WHEN_NEW_BALL_RESET sets it TRUE when a
 * fresh ball is readied on the spring, but it's not a one-shot latch: every
 * table's own switch handlers keep it live for real - each of PLAND.ASM,
 * SDEV.ASM, SHOW.ASM and STONES.ASM has a "SPRING INVALID" bygel (a
 * playfield-switch callback, Swedish "bygel" = sensor/relay) that sets it
 * back FALSE the instant the ball rolls past the sensor at the spring
 * lane's exit, and a "SPRING VALID" bygel elsewhere that sets it TRUE again
 * when a ball re-enters the lane.  So it's continuously driven by gameplay,
 * not just set once at ball start.
 *
 * Forcing it permanently TRUE is exactly PINTRN.COM's "spring mode": with
 * it pinned, every down-arrow press launches the ball, wherever it actually
 * is on the table - but "pinned" has to mean *continuously* re-asserted,
 * because the exit-lane bygel above will clear it again the moment the ball
 * leaves the spring lane, same as it does in an uncheated game.  A one-shot
 * poke here undoes itself after exactly one launch, which is what "ball
 * control mode seems to be disabled automatically after launching a ball"
 * turned out to be - not a wrong address, just a plain write racing a
 * switch handler that fires during play.  fantasies_spring_tick() below is
 * the fix: while spring_cheat_on is set, it re-forces the byte TRUE every
 * dev_tick(), the same "poll and correct" approach fantasies_pause_tick()
 * already uses for LAST_WAS_VB.  fantasies_toggle_spring() also no longer
 * infers on/off from the byte's current value - TRUE is SPRING_VALID's own
 * everyday "ball's on the spring" state (true again within moments of
 * booting a table, ball 1 already readied), indistinguishable from our
 * forced-on state by value alone, which was the second bug: pressing '2'
 * for the first time was reading that ordinary TRUE and toggling it "off".
 * spring_cheat_on is our own state instead, set by the hotkey and cleared
 * whenever a table (re)loads (fantasies_on_exec above).
 *
 * DATA's own runtime segment is recovered the same way
 * fantasies_patch_pause() above already recovers it - majority vote over
 * every `push imm16 / pop ds` (68 xx xx 1F) site in the image, DATA being by
 * far the most common target - and the flag's DATA-relative offset is read
 * straight out of WHEN_NEW_BALL_RESET's own signature: a long, distinctive
 * run of six consecutive immediate-value stores that starts every fresh
 * ball (LASTCHECK=0, SPRING_VALID=TRUE, SHIFTPRESSED=FALSE, KEYTASK=offset
 * DUMRET, JINGLE_READY_ANIM=TRUE, JINGLE_READY_LOGIC=TRUE). Confirmed unique
 * (exactly one match) by static byte-scan against all four shipped
 * TABLE1-4.PRG.  (The very first version of this fix treated the flag as
 * CS-relative like the balls-counter site above - also wrong, since
 * SPRING_VALID actually lives in DATA, not CODE; fixed before this one.) */
void fantasies_patch_spring(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t sig[32] = {
        0xC7,0x06,0,0,0x00,0x00,       /* mov word ptr [LASTCHECK],0 */
        0xC6,0x06,0,0,0xFF,            /* mov byte ptr [SPRING_VALID],true  <- offset wanted */
        0xC6,0x06,0,0,0x00,            /* mov byte ptr [SHIFTPRESSED],false */
        0xC7,0x06,0,0,0,0,             /* mov word ptr [KEYTASK],offset DUMRET */
        0xC6,0x06,0,0,0xFF,            /* mov byte ptr [JINGLE_READY_ANIM],true */
        0xC6,0x06,0,0,0xFF };          /* mov byte ptr [JINGLE_READY_LOGIC],true */
    static const uint8_t mask[32] = {
        1,1,0,0,1,1,
        1,1,0,0,1,
        1,1,0,0,1,
        1,1,0,0,0,0,
        1,1,0,0,1,
        1,1,0,0,1 };
    char b[64];
    uint32_t i;
    uint16_t cand_val[64]; uint32_t cand_cnt[64]; int ncand = 0;
    uint32_t data_seg = 0, best = 0;
    int tn;

    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    tn = prog_slot(b);
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sig)) return;

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
        trc("[fantasies] spring-valid fix: DATA segment not confidently found (table %d), leaving unpatched\n", tn);
        return;
    }

    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at+k] != sig[k]) break;
        if(k == sizeof(sig)){
            uint16_t off = img_u16(load_base, i + 8);
            spring_valid_addr[tn] = data_seg*16 + off;
            trc("[fantasies] spring-valid located (table %d): %05X (data_seg=%04X off=%04X)\n",
                tn, spring_valid_addr[tn], data_seg, off);
            return;
        }
    }
    trc("[fantasies] spring-valid fix: signature not found (table %d), leaving unpatched\n", tn);
}

/* jump_vel_addr[table_num] is the linear address of the ball-velocity
 * word the Z//' kick subtracts from, located by fantasies_patch_jump()
 * below; 0 = not found for that table (kicks become a no-op). */
static uint32_t jump_vel_addr[5] = {0,0,0,0,0};

/* Locates the ball-velocity word the Z//' kick subtracts from, inside
 * the vertical ball-motion integrator (trainer/TRAINER.EXE, MAT Jan 1994
 * - its "BALL JUMP KEY").
 *
 * Per physics tick the engine folds a velocity word into a 32-bit
 * fixed-point accumulator and scales it down with a fixed divisor:
 *
 *   MOV AX,[vel] / CWD / ADD [acc_lo],AX / ADC [acc_hi],DX /
 *   MOV AX,[acc_lo] / MOV DX,[acc_hi] / MOV BX,0400h / IDIV BX
 *
 * The megatrainer's jump handler subtracts 0400h from [vel] on a Z (2Ch)
 * or '/' (35h) keystroke - an instant upward kick wherever the ball is -
 * which is what fantasies_jump_kick() below replays.
 *
 * The sequence occurs twice per table (the X and Y integrators share the
 * shape); the trainer's own fixed patch offsets hook the first of the
 * two, which is the vertical channel the kick needs, so the first match
 * is taken here too.  Confirmed by static byte-scan (both matches, same
 * spacing) against every shipped TABLE1-4.PRG of the floppy, Deluxe and
 * Power Pack releases, plus the unpacked demo's PLAND.PRG.
 * DATA's runtime segment is recovered the same majority-vote way
 * fantasies_patch_spring() recovers it; the velocity offset comes
 * straight out of the first matched immediate.
 *
 * Deliberately not ported: the trainer's companion cheat at this same
 * site, clamping the accumulator high word 9->7 every tick
 * ("CONTINUOUS BALL").  Implemented that way here and then removed again:
 * side-drain ball loss does not go through the clamped state, so balls
 * kept draining exactly as without it. */
void fantasies_patch_jump(const char *dospath, uint32_t load_base, uint32_t imglen){
    static const uint8_t sig[24] = {
        0xA1,0,0,                       /* mov ax,[vel]  <- velocity offset wanted */
        0x99,                           /* cwd */
        0x01,0x06,0,0,                  /* add [acc_lo],ax */
        0x11,0x16,0,0,                  /* adc [acc_hi],dx  <- accumulator offset wanted */
        0xA1,0,0,                       /* mov ax,[acc_lo] */
        0x8B,0x16,0,0,                  /* mov dx,[acc_hi] */
        0xBB,0x00,0x04,                 /* mov bx,0400h */
        0xF7,0xFB };                    /* idiv bx */
    static const uint8_t mask[24] = {
        1,0,0,
        1,
        1,1,0,0,
        1,1,0,0,
        1,0,0,
        1,1,0,0,
        1,1,1,
        1,1 };
    char b[64];
    uint32_t i;
    uint16_t cand_val[64]; uint32_t cand_cnt[64]; int ncand = 0;
    uint32_t data_seg = 0, best = 0;
    uint16_t vel_off = 0;
    int tn;

    if(!session_armed || dos_no_patch) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    tn = prog_slot(b);
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sig)) return;

    for(i = 0; i + sizeof(sig) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sig); k++)
            if(mask[k] && ram[at+k] != sig[k]) break;
        if(k == sizeof(sig)){
            vel_off = img_u16(load_base, i + 1);
            break;
        }
    }
    if(!vel_off){
        jump_vel_addr[tn] = 0;
        trc("[fantasies] ball-jump fix: signature not found (table %d), leaving unpatched\n", tn);
        return;
    }

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
        jump_vel_addr[tn] = 0;
        trc("[fantasies] ball-jump fix: DATA segment not confidently found (table %d), leaving unpatched\n", tn);
        return;
    }

    jump_vel_addr[tn] = data_seg*16 + vel_off;
    trc("[fantasies] ball-jump located (table %d): vel=%05X (data_seg=%04X)\n",
        tn, jump_vel_addr[tn], data_seg);
}

/* Returns 1 if now ON, 0 if now OFF, -1 if the image didn't match either
 * known state (nothing touched) - callers use this to decide whether/what
 * to show on the OSD. */
static int fantasies_toggle_balls(void){
    uint32_t a = balls_addr[table_num];
    if(!a || a + 4 > RAM_SIZE) return -1;
    if(ram[a]==0xFE && ram[a+1]==0x06){
        ram[a]=0x90; ram[a+1]=0x90; ram[a+2]=0x90; ram[a+3]=0x90;
        trc("[fantasies] infinite balls ON (table %d)\n", table_num);
        return 1;
    } else if(ram[a]==0x90 && ram[a+1]==0x90 && ram[a+2]==0x90 && ram[a+3]==0x90){
        uint16_t counter = balls_counter_val[table_num];
        ram[a]=0xFE; ram[a+1]=0x06;
        ram[a+2]=(uint8_t)(counter & 0xFF);
        ram[a+3]=(uint8_t)(counter >> 8);
        trc("[fantasies] infinite balls OFF (table %d)\n", table_num);
        return 0;
    }
    return -1;
}

/* Same contract as fantasies_toggle_balls() above, for the 5-byte tilt
 * increment.  Unlike the megatrainer - whose in-game '2' toggle NOPs only
 * four of the five bytes (leaving a dangling 3Ch that accidentally pairs
 * with the next opcode byte into a benign CMP) - both directions here
 * cover the whole instruction: five 90s on, the five saved bytes off. */
static int fantasies_toggle_tilt(void){
    uint32_t a = tilt_addr[table_num];
    int i;
    if(!a || a + 5 > RAM_SIZE) return -1;
    if(ram[a]==0x83 && ram[a+1]==0x06){
        for(i = 0; i < 5; i++) ram[a+(uint32_t)i]=0x90;
        trc("[fantasies] infinite tilts ON (table %d)\n", table_num);
        return 1;
    }
    for(i = 0; i < 5; i++)
        if(ram[a+(uint32_t)i] != 0x90) return -1;
    for(i = 0; i < 5; i++) ram[a+(uint32_t)i]=tilt_orig[table_num][i];
    trc("[fantasies] infinite tilts OFF (table %d)\n", table_num);
    return 0;
}

/* Own-state toggle, not a byte-value inference - see the comment above
 * fantasies_patch_spring() for why reading SPRING_VALID itself can't tell
 * "ball's legitimately on the spring" from "we forced this on".  Writes the
 * byte both ways: forcing TRUE on is what fantasies_spring_tick() then
 * holds, but turning off has to force FALSE too, not just stop holding it -
 * otherwise the last-forced TRUE simply sits there (nothing else was due to
 * clear it right then) and the cheat's actual effect - down-arrow launching
 * the ball anywhere - keeps working even though the toggle says OFF. */
static int fantasies_toggle_spring(void){
    uint32_t a = spring_valid_addr[table_num];
    if(!a || a >= RAM_SIZE) return -1;
    spring_cheat_on = !spring_cheat_on;
    ram[a] = spring_cheat_on ? 0xFF : 0x00;
    trc("[fantasies] ball control mode %s (table %d)\n",
        spring_cheat_on ? "ON" : "OFF", table_num);
    return spring_cheat_on;
}

/* Called from dev_tick() - far more often than once per emulated video
 * frame, same as fantasies_pause_tick() above.  While spring_cheat_on is
 * set, re-forces SPRING_VALID back to TRUE every tick, so the table's own
 * "ball left the spring lane" switch handler (which clears it FALSE as part
 * of ordinary gameplay - see fantasies_patch_spring()'s comment) can never
 * make the cheat's effect disappear mid-play. Logs only the correction
 * itself, not every tick that finds nothing to do. */
void fantasies_spring_tick(void){
    uint32_t a;
    if(!fantasies_fix_active() || !table_num || !spring_cheat_on) return;
    a = spring_valid_addr[table_num];
    if(!a || a >= RAM_SIZE) return;
    if(ram[a] != 0xFF){
        ram[a] = 0xFF;
        trc("[fantasies] ball control mode re-asserted (table %d)\n", table_num);
    }
}

/* One upward kick: subtracts 0400h from the ball-velocity word, the
 * megatrainer's "BALL JUMP KEY" (Z / '/' in that trainer).  Fire-and-forget
 * like a keystroke - no sticky state, so nothing to clear on table reload
 * and nothing for the tick to hold. */
static void fantasies_jump_kick(void){
    uint32_t a = jump_vel_addr[table_num];
    uint16_t v;
    if(!a || a + 2 > RAM_SIZE) return;
    v = (uint16_t)(ram[a] | (ram[a+1] << 8));
    v = (uint16_t)(v - 0x400);
    ram[a] = (uint8_t)(v & 0xFF); ram[a+1] = (uint8_t)(v >> 8);
    trc("[fantasies] ball jump kick (table %d)\n", table_num);
}

/* Called from dev.c's kbd_key() on every fresh (non-autorepeat) key make,
 * host scancode already stripped of the E0 prefix bit.  '1'-'3' and 'Z'
 * are plain, unextended scancodes, so no E0 check is needed here.  Gated on
 * trainer_enabled (the launcher's "Enable trainer" checkbox, see
 * load_cheat_cfg above): unchecked, the hotkeys are completely inert - not
 * just "cheats start off", but no keypress here ever touches memory at
 * all - same as before this feature existed.
 *
 * '1' and '3' NOP a counter update out of the image (restoring it on
 * toggle-off); '2' flips holding state the tick re-asserts.  'Z' (2Ch)
 * is an action, not a toggle: a one-shot upward kick, live only while
 * ball-control mode ('2') is on - the megatrainer's jump key folded into
 * pfemu's existing ball-control mode.  The game itself ignores all four
 * scancodes, so every hotkey doubles as its own no-op downstream. */
/* The trainer invariant (docs/REPLAY.md sections 3.2/3.3): recording and
 * replay require the trainer off, and the hotkeys are dead in both
 * modes even if a config says otherwise.  main() refuses to start either
 * mode with it enabled; this is the backstop for the hotkeys themselves. */
int fantasies_trainer_enabled(void){ return trainer_enabled; }

void fantasies_key_event(int scancode, int down){
    int r;
    if(replay_is_recording() || replay_is_replaying()) return;
    if(!down || !trainer_enabled || !fantasies_fix_active() || !table_num || dos_no_patch)
        return;
    if(scancode == 0x02){
        r = fantasies_toggle_balls();
        if(r >= 0) osd_show(r ? "INFINITE BALLS: ON" : "INFINITE BALLS: OFF");
    } else if(scancode == 0x03){
        r = fantasies_toggle_spring();
        if(r >= 0) osd_show(r ? "BALL CONTROL: ON" : "BALL CONTROL: OFF");
    } else if(scancode == 0x04){
        r = fantasies_toggle_tilt();
        if(r >= 0) osd_show(r ? "INFINITE TILTS: ON" : "INFINITE TILTS: OFF");
    } else if(scancode == 0x2C){
        if(spring_cheat_on) fantasies_jump_kick();
    }
}

/* ------------------------------------------------- ball-gap logger ------
 * -balldbg: measures the window in which the ball does not exist on screen.
 *
 * FANTASIE.ASM's PUTTHEBALL ("FN: DELETES AND PUTS THE BALL") restores the
 * saved background at OLDPOS via DELBALL, then re-saves and redraws at the
 * new position via PUTBALL.  There is no sprite double-buffer: between those
 * two calls the ball is simply absent from the visible page, and because
 * PUTITBETWEEN/PUTF3BETWEEN are set by the caller, a full DOFLIPPER and
 * DOFLIPPER3 blit run *inside* that window - so it is far wider than the
 * 16-line ball itself, and any interrupt (notably the MOD mixer) taken
 * mid-routine widens it further.
 *
 * The engine hides the gap by racing the beam rather than by buffering: the
 * VBLANK handler compares SC_Y against START_RASTER + MIDDLE_RASTER_LO/HI
 * and redraws a lower-half ball immediately (beam still above it), or sets
 * LATEGFX=TRUE to defer an upper-half ball to LATE_RASTER_INTERRUPT, which
 * fires mid-screen once the beam has already passed it.  Done on time, the
 * erase/redraw is always in the half the beam is not painting, so a CRT
 * never shows it.
 *
 * vga_render() has no beam: it snapshots all of VRAM at one instant, so a
 * present landing inside the gap drops the ball from the *whole* frame.
 * That is what this logger quantifies, and what a present-phase fix has to
 * avoid - docs/EMULATOR.md notes that locking presents to vsync is
 * exactly when a lower-half ball is being redrawn, and flickered constantly
 * as a result.  Numbers wanted before choosing a phase: how wide the gap
 * actually is, and where in the frame it sits for each of the two cases.
 *
 * PUTTHEBALL is located by signature, not by a fixed offset, the way
 * fantasies_patch_pause()/_balls() locate theirs.  The prologue below
 * (PUSHA / PUSH 0A000h / POP ES / CMP VERYFIRSTPUT,TRUE / JE) is unique in
 * all four shipped TABLE1-4.PRG of both the floppy and Deluxe releases, as
 * is the epilogue (CALL PUTBALL / SET_DS DATA / MOV OLDPOS,SI /
 * MOV OLDSHIFT,DX / POPA / RETN); the body is 134 bytes in all eight, with
 * only the DS-relative operands differing.  The two are cross-checked
 * against each other rather than either being trusted alone. */
int balldbg_on = 0;
uint32_t balldbg_entry = 0, balldbg_exit = 0, balldbg_pos = 0;

/* ---- present window, derived at runtime ---------------------------------
 * The quiet span between the two redraw bands is where a present has to land
 * (#26), but the bands sit at each table's own raster line - measured at
 * lines 222.7, 225.5, 231.4 and ~296 across the four shipped tables - so a
 * hardcoded window is luck rather than design, and the several releases carry
 * slightly different table binaries.  Derive it instead.
 *
 * Every redraw marks the frame buckets it covered, from the PUTTHEBALL entry
 * hook to the epilogue hook.  The window is then placed in the quiet span
 * that FOLLOWS the vblank band - deliberately that one and not merely the
 * largest, because sampling before the mid-frame redraw is what matches the
 * CRT: an upper-half ball is painted by the beam before that redraw, so the
 * pre-redraw state is what hardware showed.  The later span would show it one
 * tick early and make it step backwards as it crosses mid-screen (#26).
 *
 * Until enough redraws have been seen the seed constants in main.c stand.
 * The histogram resets when the table changes or the CRTC timing does, so a
 * hi-res toggle (which moves MIDDLE_RASTER, and with it the bands) re-learns
 * rather than pinning the window to stale geometry. */
#define PW_B      64        /* frame buckets, ~8.2 scan lines each */
#define PW_WARMUP 90        /* redraws before the derived window is trusted (~3 s) */
#define PW_MARGIN 4         /* buckets kept clear of either band (~33 lines) */
#define PW_MINW   4         /* refuse to adopt a window narrower than this */
static unsigned pw_occ[PW_B];
static unsigned pw_n = 0;
static int pw_vt = 0, pw_valid = 0;
static double pw_lo = 0.0, pw_hi = 0.0;

static void pw_reset(void){
    memset(pw_occ, 0, sizeof(pw_occ));
    pw_n = 0; pw_valid = 0; pw_vt = 0;
}

static void pw_mark(double line0, double line1, int vt){
    int a, b, n = 0;
    if(vt <= 0) return;
    if(vt != pw_vt){ pw_reset(); pw_vt = vt; }   /* timing changed - relearn */
    a = (int)(line0 * PW_B / vt);
    b = (int)(line1 * PW_B / vt);
    if(a < 0) a = 0; if(a >= PW_B) a = PW_B - 1;
    if(b < 0) b = 0; if(b >= PW_B) b = PW_B - 1;
    while(n < PW_B){                              /* walks forward, wraps */
        pw_occ[a]++;
        if(a == b) break;
        a = (a + 1) % PW_B; n++;
    }
    pw_n++;
}

/* Place the window in the quiet span following the vblank band. */
static void pw_derive(void){
    double per, inv, hde;
    int vt, vd, vrs, vre, r, i, s = -1, e, len = 0;
    if(pw_n < PW_WARMUP || pw_vt <= 0) return;
    vga_timing_cached(&per, &inv, &vt, &vd, &vrs, &vre, &hde);
    (void)per; (void)inv; (void)vd; (void)vre; (void)hde;
    if(vt <= 0 || vt != pw_vt) return;
    r = (int)((double)vrs * PW_B / vt);
    if(r < 0 || r >= PW_B) return;
    /* from vertical retrace, walk past the vblank band to the first gap */
    for(i = 0; i < PW_B; i++){
        int k = (r + i) % PW_B;
        if(pw_occ[k] == 0){ s = k; break; }
    }
    if(s < 0) return;                       /* no quiet bucket at all */
    for(i = 0; i < PW_B; i++){              /* how far the gap runs */
        if(pw_occ[(s + i) % PW_B]) break;
        len++;
    }
    if(len < 2 * PW_MARGIN + PW_MINW) return;   /* too tight to use safely */
    e = (s + len - 1) % PW_B;
    (void)e;
    pw_lo = (double)((s + PW_MARGIN) % PW_B) / (double)PW_B;
    pw_hi = (double)(((s + len - 1 - PW_MARGIN) % PW_B) + 1) / (double)PW_B;
    pw_valid = 1;
}

/* main.c: overrides its seed constants once the bands have been learned. */
int fantasies_present_window(double *lo, double *hi){
    if(!pw_valid) return 0;
    *lo = pw_lo; *hi = pw_hi;
    return 1;
}

#define BG_BUDGET 20000          /* auto-off cap: ~11 min of play at 30 Hz */
#define BG_BUCKETS 16
static int    bg_inside = 0;
static double bg_t0 = 0.0, bg_line0 = 0.0;
static int    bg_vtotal = 0;
static unsigned long bg_events = 0, bg_orphan = 0;
static unsigned long bg_presents = 0, bg_presents_inside = 0, bg_fallback = 0;
#define BG_PROF 64
static unsigned long bg_occ[BG_PROF];    /* frame lines covered by a redraw */
/* Camera/ball coherence: SETSCREENSTART runs before PUTTHEBALL inside
 * LATE_RASTER_INTERRUPT, so between them the CRTC start is one camera step
 * newer than the ball drawn under it.  A present sampled in that window puts
 * the ball a few pixels off its true screen position.  bg_last_start is the
 * start address in force when the ball was last drawn; comparing it against
 * the start at present time measures the displacement directly. */
static uint32_t bg_last_start = 0; static int bg_have_start = 0;
static double bg_last_ball_t = 0.0;
static unsigned long bg_skew_n = 0; static double bg_skew_sum = 0.0, bg_skew_max = 0.0;
static unsigned long bg_pres[BG_PROF];   /* frame lines presents landed on */
static double bg_sum = 0.0, bg_max = 0.0, bg_min = 1e9;
static unsigned long bg_hist_entry[BG_BUCKETS], bg_hist_exit[BG_BUCKETS];

void fantasies_patch_ballgap(const char *dospath, uint32_t load_base, uint32_t imglen){
    /* PUSHA / PUSH 0A000h / POP ES / CMP byte[VERYFIRSTPUT],TRUE / JE short */
    static const uint8_t sigA[12] = {
        0x60, 0x68,0x00,0xA0, 0x07, 0x80,0x3E,0,0,0xFF, 0x74,0 };
    static const uint8_t maskA[12] = {
        1, 1,1,1, 1, 1,1,0,0,1, 1,0 };
    /* CALL PUTBALL / PUSH DATA / POP DS / MOV [OLDPOS],SI / MOV [OLDSHIFT],DX
     * / POPA / RETN */
    static const uint8_t sigB[17] = {
        0xE8,0,0, 0x68,0,0, 0x1F,
        0x89,0x36,0,0, 0x89,0x16,0,0, 0x61, 0xC3 };
    static const uint8_t maskB[17] = {
        1,0,0, 1,0,0, 1,
        1,1,0,0, 1,1,0,0, 1, 1 };
    char b[64];
    uint32_t i, ea = (uint32_t)-1, eb = (uint32_t)-1;

    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)) return;
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sigA)) return;

    for(i = 0; i + sizeof(sigA) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sigA); k++)
            if(maskA[k] && ram[at+k] != sigA[k]) break;
        if(k == sizeof(sigA)){ ea = i; break; }
    }
    if(ea != (uint32_t)-1){
        /* epilogue must follow the prologue inside one routine body */
        for(i = ea; i + sizeof(sigB) <= imglen && i < ea + 400; i++){
            uint32_t at = load_base + i, k;
            for(k = 0; k < sizeof(sigB); k++)
                if(maskB[k] && ram[at+k] != sigB[k]) break;
            if(k == sizeof(sigB)){ eb = i; break; }
        }
    }
    if(ea == (uint32_t)-1 || eb == (uint32_t)-1){
        trc("[fantasies] PUTTHEBALL signature not found; camera pairing and"
            " -balldbg are both idle for this image\n");
        balldbg_entry = balldbg_exit = balldbg_pos = 0;
        return;
    }
    balldbg_entry = load_base + ea;
    balldbg_exit  = load_base + eb + 16;      /* the RETN itself */
    balldbg_pos   = load_base + eb + 7;       /* MOV [OLDPOS],SI - SI = ball VRAM offset */
    vga_reset_start_pairing();
    pw_reset();
    if(balldbg_on)
        fprintf(stderr, "[ball] PUTTHEBALL entry=%05X retn=%05X body=%u bytes\n",
                (unsigned)balldbg_entry, (unsigned)balldbg_exit,
                (unsigned)(eb + 17 - ea));
}

/* Called from cpu_step() for every instruction while balldbg_on, but only
 * after the two-address compare there, so this is off the hot path. */
void fantasies_ballgap_exec(uint32_t lin){
    double now = emu_now(), line;
    int vt = 0;

    /* The entry hook is live in normal play, not just under -balldbg: the
     * present-window derivation needs the start of each redraw to know how
     * much of the frame the band covers. */
    if(lin == balldbg_entry){
        line = vga_scanline_now(&vt);
        if(bg_inside) bg_orphan++;            /* entry without a matching exit */
        bg_inside = 1; bg_t0 = now; bg_line0 = line; bg_vtotal = vt;
        return;
    }

    if(lin == balldbg_pos){
        /* about to store OLDPOS: SI is the ball VRAM offset, DX its shift.
         * Hand the start address in force right now to the renderer so the
         * displayed camera stays paired with this ball (see vga.c). */
        int pitch = 0;
        uint32_t st = vga_start_now(&pitch);
        vga_note_ball_start(st);
        line = vga_scanline_now(&vt);
        if(bg_inside){                        /* paired with an entry: a real band */
            pw_mark(bg_line0, line, vt);
            if((pw_n % 30) == 0) pw_derive();
        }
        if(balldbg_on && bg_events < BG_BUDGET){
            uint32_t si = REG16(R_ESI);
            double row = pitch ? ((double)si - (double)st) / (double)pitch : 0.0;
            fprintf(stderr, "[ballpos] t=%.6f si=%u dx=%u start=%u row=%.2f\n",
                    now, (unsigned)si, (unsigned)REG16(R_EDX), (unsigned)st, row);
            bg_last_start = st; bg_have_start = 1; bg_last_ball_t = now;
        }
        return;
    }
    if(bg_events >= BG_BUDGET) return;
    line = vga_scanline_now(&vt);

    /* lin == balldbg_exit */
    if(!bg_inside){ bg_orphan++; return; }
    bg_inside = 0;
    {
        double dt = (now - bg_t0) * 1e6;       /* microseconds */
        double per, inv, hde; int vd, vrs, vre, vtx;
        vga_timing_cached(&per, &inv, &vtx, &vd, &vrs, &vre, &hde);
        (void)inv; (void)vd; (void)vrs; (void)vre; (void)hde;
        bg_events++;
        bg_sum += dt;
        if(dt > bg_max) bg_max = dt;
        if(dt < bg_min) bg_min = dt;
        if(bg_vtotal > 0){
            int k = (int)(bg_line0 * BG_BUCKETS / bg_vtotal);
            if(k >= 0 && k < BG_BUCKETS) bg_hist_entry[k]++;
        }
        if(vt > 0){
            int k = (int)(line * BG_BUCKETS / vt);
            if(k >= 0 && k < BG_BUCKETS) bg_hist_exit[k]++;
        }
        /* mark every frame line this redraw covered, so the occupancy
         * profile shows the real band edges rather than 16ths */
        if(vt > 0 && bg_vtotal > 0){
            int a = (int)(bg_line0 * BG_PROF / bg_vtotal);
            int b2 = (int)(line * BG_PROF / vt);
            int n = 0;
            if(a < 0) a = 0; if(a >= BG_PROF) a = BG_PROF-1;
            if(b2 < 0) b2 = 0; if(b2 >= BG_PROF) b2 = BG_PROF-1;
            while(n < BG_PROF){                  /* walks forward, wraps */
                bg_occ[a]++;
                if(a == b2) break;
                a = (a + 1) % BG_PROF; n++;
            }
        }
        fprintf(stderr, "[ball] t=%.6f in=%.1f out=%.1f /%d gap=%.1fus (%.2f%% of frame)\n",
                now, bg_line0, line, vt, dt,
                per > 0.0 ? dt / (per * 1e6) * 100.0 : 0.0);
        if(bg_events == BG_BUDGET)
            fprintf(stderr, "[ball] event budget reached, logger off\n");
    }
}

void fantasies_ballgap_present(int fallback){
    int vt = 0;
    double line;
    if(!balldbg_on || !balldbg_entry) return;
    bg_presents++;
    if(bg_inside) bg_presents_inside++;
    if(fallback) bg_fallback++;
    line = vga_scanline_now(&vt);
    if(vt > 0){
        int k = (int)(line * BG_PROF / vt);
        if(k >= 0 && k < BG_PROF) bg_pres[k]++;
    }
    if(bg_have_start){
        int pitch = 0;
        uint32_t st;
        vga_start_now(&pitch);
        st = vga_displayed_start_now();    /* what the frame actually shows */
        if(st != bg_last_start && pitch > 0){
            double rows = (double)(int32_t)(st - bg_last_start) / (double)pitch;
            if(rows < 0.0) rows = -rows;
            if(rows < 1000.0){                 /* ignore mode changes / wraps */
                double tn = emu_now();
                bg_skew_n++;
                bg_skew_sum += rows;
                if(rows > bg_skew_max) bg_skew_max = rows;
                /* Which is it: the game skipped a ball redraw (authentic - the
                 * raster handler moves the camera BEFORE the INSIDE_BALLHANDLER
                 * bail-out, so an overrunning vblank handler leaves the ball
                 * behind), or did we sample a camera write the hardware would
                 * not have shown yet (ours - real VGA latches the start address
                 * at vertical retrace, we read it live)?  age_ball large means
                 * the former; age_write small means the latter. */
                if(bg_skew_n <= 300)
                    fprintf(stderr, "[skew] t=%.6f rows=%.2f age_ball=%.2fms age_write=%.3fms\n",
                            tn, rows, (tn - bg_last_ball_t) * 1e3,
                            vga_last_start_write >= 0.0 ? (tn - vga_last_start_write) * 1e3 : -1.0);
            }
        }
    }
}

/* one character per 1/64th of the frame, log-ish magnitude */
static void bg_profile(const char *label, const unsigned long *h){
    static const char ramp[] = " .:-=+*#%@";
    unsigned long mx = 0;
    int k;
    for(k = 0; k < BG_PROF; k++) if(h[k] > mx) mx = h[k];
    fprintf(stderr, "[ball] %-9s |", label);
    for(k = 0; k < BG_PROF; k++){
        int v = 0;
        if(mx && h[k]){
            v = (int)(h[k] * 9 / mx);
            if(v < 1) v = 1;
        }
        fputc(ramp[v], stderr);
    }
    fprintf(stderr, "|\n");
}

void fantasies_ballgap_report(void){
    int k;
    if(!balldbg_on || !bg_events) return;
    fprintf(stderr,
        "[ball] %lu redraws: gap min=%.1fus mean=%.1fus max=%.1fus; orphans=%lu\n",
        bg_events, bg_min, bg_sum / bg_events, bg_max, bg_orphan);
    fprintf(stderr,
        "[ball] presents=%lu inside-gap=%lu (%.2f%% - these frames lost the ball)\n",
        bg_presents, bg_presents_inside,
        bg_presents ? bg_presents_inside * 100.0 / bg_presents : 0.0);
    fprintf(stderr, "[ball] entry scanline histogram (16ths of frame):\n       ");
    for(k = 0; k < BG_BUCKETS; k++) fprintf(stderr, "%6lu", bg_hist_entry[k]);
    fprintf(stderr, "\n[ball] exit  scanline histogram:\n       ");
    for(k = 0; k < BG_BUCKETS; k++) fprintf(stderr, "%6lu", bg_hist_exit[k]);
    fprintf(stderr, "\n[ball] fallback presents (phase test bypassed) = %lu of %lu (%.2f%%)\n",
            bg_fallback, bg_presents,
            bg_presents ? bg_fallback * 100.0 / bg_presents : 0.0);
    fprintf(stderr, "[ball] camera/ball skew at present: %lu of %lu (%.2f%%)"
            " mean=%.2f rows max=%.2f rows\n",
            bg_skew_n, bg_presents,
            bg_presents ? bg_skew_n * 100.0 / bg_presents : 0.0,
            bg_skew_n ? bg_skew_sum / bg_skew_n : 0.0, bg_skew_max);
    if(pw_valid)
        fprintf(stderr, "[ball] derived present window: %.4f-%.4f (lines %.0f-%.0f of %d)\n",
                pw_lo, pw_hi, pw_lo * pw_vt, pw_hi * pw_vt, pw_vt);
    else
        fprintf(stderr, "[ball] present window not derived (%u redraws seen, need %d)\n",
                pw_n, PW_WARMUP);
    fprintf(stderr, "[ball] frame profile, 64ths of frame (0 = frame start):\n");
    bg_profile("redraws", bg_occ);
    bg_profile("presents", bg_pres);
}

/* -------------------------------------------------- dot-matrix pacing ---- */
/* -matdbg: why the dot-matrix panel updates in bursts.
 *
 * The panel is not paced by a timer of its own.  FANTASIE.ASM's DO_THE_REST
 * (the tail of the VBLANK callback, which runs every emulated frame - SLOW=0,
 * so there is no 30 Hz divider in the engine) guards the whole update:
 *
 *      cmp  time_left,false
 *      je   skip_matrix           ; no time left!  Skip it!!
 *      mov  al,cs:INSIDE_MATRIX   ; ...and skip it again if the previous
 *      mov  cs:INSIDE_MATRIX,TRUE ;    update is still running
 *      cmp  al,TRUE
 *      je   skip_matrix
 *      call DO_THE_DOTMATRIX
 *
 * and TIME_LEFT is not the game's own judgement - it is a flag the *sound
 * driver* hands in.  Both callbacks the game registers through int 66h start
 * with
 *
 *      or ax,ax / mov time_left,TRUE / jz .. / mov time_left,FALSE
 *
 * where AX is an in-parameter from the driver's timer ISR.  Disassembling
 * SBLASTER.SDR (image 1C65 / 1C8D / 1CB1) shows what sets it: the ISR
 * measures how many bytes of already-mixed audio sit ahead of the DMA read
 * pointer, and passes AX=FFFF once that headroom has fallen to one mix block
 * - two blocks if the ring is five blocks or deeper.  A block is rate/50
 * bytes (one MOD tick; image 1712), and the caller asks for the ring in
 * blocks (image 05E9): the intro asks for 26, which is the 10920-byte ring
 * -snddbg measured at 21 kHz, and a table asks for 2, the 840-byte one.  So
 * while a table runs, the ring is two blocks deep and the panel is dropped
 * whenever the mixer is less than one block ahead of the card.
 *
 * That is the engine trading dot-matrix frames for mixer headroom, by design
 * - so the question is not whether it happens but how often, and whether the
 * rate is one a period machine would also have produced or an artefact of how
 * much CPU pfemu hands the guest.  This counts it: game ticks, how many
 * carried the crisis flag, how many actually reached DO_THE_DOTMATRIX, and
 * the gaps between updates.  A steady gap of 2 is the authentic half-rate
 * panel; a long tail is the burstiness.
 *
 * Nothing here touches the guest - three address compares and counters. */
int mat_dbg = 0;
uint32_t mat_tick_site = 0;   /* cmp time_left,false   - once per game tick   */
uint32_t mat_call_site = 0;   /* call DO_THE_DOTMATRIX - an update really ran */
uint32_t mat_crisis_site = 0; /* or ax,ax in VBLANK_INT - AX is the flag      */

#define MAT_GAPS 12
static unsigned long mat_ticks, mat_updates, mat_crisis, mat_vbl;
static unsigned long mat_gap_hist[MAT_GAPS];
static unsigned long mat_since;          /* ticks since the last update */
static double mat_t0 = -1.0, mat_report_t = 0.0;
static unsigned long mat_win_ticks, mat_win_upd, mat_win_crisis;

static void mat_reset(void){
    /* The PIT counters in dev.c run from boot, but everything reported here is
     * divided by the window since this table loaded - so zero them with the
     * rest or the rates come out inflated (a first cut printed 146.5 one-shots
     * a second for what was really 113). */
    { extern unsigned long pit0_m0_reads, pit0_m0_wrapped, pit0_irqs, pit0_lat_n;
      extern double pit0_lat_sum, pit0_lat_max;
      extern double pit0_over_sum, pit0_over_max, pit0_wait_sum, pit0_wait_max;
      pit0_m0_reads = pit0_m0_wrapped = pit0_irqs = pit0_lat_n = 0;
      pit0_lat_sum = pit0_lat_max = 0.0;
      pit0_over_sum = pit0_over_max = pit0_wait_sum = pit0_wait_max = 0.0; }
    mat_ticks = mat_updates = mat_crisis = mat_vbl = 0;
    mat_since = 0;
    memset(mat_gap_hist, 0, sizeof(mat_gap_hist));
    mat_t0 = -1.0; mat_report_t = 0.0;
    mat_win_ticks = mat_win_upd = mat_win_crisis = 0;
}

/* Locate the three sites in a freshly loaded table image.  The shape below is
 * identical in every TABLE1-4.PRG of every release checked - the assembler
 * pads each short conditional with three NOPs, which is what makes these
 * sequences long enough to be unambiguous - and the TIME_LEFT cell address
 * falls out of the first match, so nothing is hard-coded per table. */
void fantasies_find_matrix(const char *dospath, uint32_t load_base, uint32_t imglen){
    /* or ax,ax / mov [TIME_LEFT],TRUE / jz +8 / nop*3 / mov [TIME_LEFT],FALSE */
    static const uint8_t sigA[17] = {
        0x0B,0xC0, 0xC6,0x06,0,0,0xFF, 0x74,0x08, 0x90,0x90,0x90,
        0xC6,0x06,0,0,0x00 };
    static const uint8_t maskA[17] = {
        1,1, 1,1,0,0,1, 1,1, 1,1,1, 1,1,0,0,1 };
    char b[64];
    const char *dot;
    uint32_t i;
    uint8_t cell[2];

    if(!mat_dbg || !session_armed) return;
    base_up(dospath, b, sizeof(b));
    if(!is_table_prog(b)){
        /* A running table EXECs its own .SDR, and that must not clear the
         * hooks or the counters out from under the measurement; leaving the
         * table for the intro must, or the hooks would go on matching
         * addresses that now hold somebody else's code. */
        dot = strrchr(b, '.');
        if(dot && (!strcmp(dot,".PRG") || !strcmp(dot,".EXE") || !strcmp(dot,".COM")))
            mat_tick_site = mat_call_site = mat_crisis_site = 0;
        return;
    }
    mat_tick_site = mat_call_site = mat_crisis_site = 0;
    mat_reset();
    if(load_base + imglen > RAM_SIZE || imglen < sizeof(sigA)) return;

    for(i = 0; i + sizeof(sigA) <= imglen; i++){
        uint32_t at = load_base + i, k;
        for(k = 0; k < sizeof(sigA); k++)
            if(maskA[k] && ram[at+k] != sigA[k]) break;
        if(k < sizeof(sigA)) continue;
        if(ram[at+4] != ram[at+14] || ram[at+5] != ram[at+15]) continue;
        mat_crisis_site = at;
        break;
    }
    if(!mat_crisis_site){
        fprintf(stderr, "[mat] TIME_LEFT signature not found in %s;"
                " -matdbg is idle for this image\n", b);
        return;
    }
    cell[0] = ram[mat_crisis_site+4];
    cell[1] = ram[mat_crisis_site+5];

    /* cmp byte [TIME_LEFT],0 / je short - the in-game guard is the first of
     * the two matches (the second is VBLANK_INT_DEMO's copy, which paces the
     * panel in attract mode).  DO_THE_DOTMATRIX's CALL sits a fixed 0x1C
     * further on, past the INSIDE_MATRIX re-entrancy guard. */
    for(i = 0; i + 0x20 <= imglen; i++){
        uint32_t at = load_base + i;
        if(ram[at] != 0x80 || ram[at+1] != 0x3E) continue;
        if(ram[at+2] != cell[0] || ram[at+3] != cell[1]) continue;
        if(ram[at+4] != 0x00 || ram[at+5] != 0x74) continue;
        if(ram[at+0x1C] != 0xE8) continue;   /* the CALL must be where it always is */
        mat_tick_site = at;
        mat_call_site = at + 0x1C;
        break;
    }
    if(!mat_tick_site){
        fprintf(stderr, "[mat] TIME_LEFT cell %02X%02X found but no matrix guard;"
                " -matdbg is idle\n", cell[1], cell[0]);
        mat_crisis_site = 0;
        return;
    }
    fprintf(stderr, "[mat] %s: TIME_LEFT=[%02X%02X] crisis=%05X tick=%05X call=%05X\n",
            b, cell[1], cell[0], (unsigned)mat_crisis_site,
            (unsigned)mat_tick_site, (unsigned)mat_call_site);
}

/* Called from cpu_step() only while -matdbg is on, and only after the three
 * address compares there have already matched. */
void fantasies_matrix_exec(uint32_t lin){
    double now = emu_now();
    if(mat_t0 < 0.0){ mat_t0 = now; mat_report_t = now + 1.0; }

    if(lin == mat_crisis_site){
        mat_vbl++;
        if(REG16(R_EAX) != 0){ mat_crisis++; mat_win_crisis++; }
        return;
    }
    if(lin == mat_tick_site){
        mat_ticks++; mat_win_ticks++; mat_since++;
        /* one line per emulated second, so a long capture stays readable and
         * a burst reads as a dip in `dmd` rather than as a wall of text */
        if(now >= mat_report_t){
            fprintf(stderr,
                "[mat] t=%6.1f  ticks=%3lu  dmd=%3lu  crisis=%3lu  (%.0f%% of ticks dropped)\n",
                now, mat_win_ticks, mat_win_upd, mat_win_crisis,
                mat_win_ticks ? (mat_win_ticks - mat_win_upd) * 100.0 / mat_win_ticks : 0.0);
            mat_win_ticks = mat_win_upd = mat_win_crisis = 0;
            mat_report_t = now + 1.0;
        }
        return;
    }
    /* lin == mat_call_site: an update really is about to run */
    mat_updates++; mat_win_upd++;
    mat_gap_hist[mat_since < MAT_GAPS ? mat_since : MAT_GAPS-1]++;
    mat_since = 0;
}

void fantasies_matrix_report(void){
    double secs;
    int k;
    if(!mat_dbg || !mat_ticks) return;
    secs = emu_time - (mat_t0 < 0.0 ? 0.0 : mat_t0);
    if(secs <= 0.0) secs = 1.0;
    fprintf(stderr,
        "[mat] %lu game ticks in %.1fs (%.1f/s), %lu dot-matrix updates (%.1f/s),"
        " %lu dropped (%.1f%%)\n",
        mat_ticks, secs, mat_ticks/secs, mat_updates, mat_updates/secs,
        mat_ticks - mat_updates, (mat_ticks - mat_updates) * 100.0 / mat_ticks);
    fprintf(stderr,
        "[mat] driver crisis flag set on %lu of %lu vblank callbacks (%.1f%%)\n",
        mat_crisis, mat_vbl, mat_vbl ? mat_crisis * 100.0 / mat_vbl : 0.0);
    /* The PIT/IRQ0 health lines that used to sit here now print from
     * dev_state_dump(): they describe the timer, not the dot matrix, and
     * being behind this function's `!mat_ticks` guard meant they were
     * unavailable for exactly the releases that never load a table. */
    fprintf(stderr, "[mat] ticks between updates (1 = every tick, the smooth case):\n"
                    "      gap:");
    for(k = 1; k < MAT_GAPS; k++)
        fprintf(stderr, "%7d%s", k, k == MAT_GAPS-1 ? "+" : " ");
    fprintf(stderr, "\n      n:  ");
    for(k = 1; k < MAT_GAPS; k++) fprintf(stderr, "%7lu ", mat_gap_hist[k]);
    fprintf(stderr, "\n");
}

/* ------------------------------------------------------ direct-to-table --- */
/* Starting at a table instead of the intro, without breaking the way out of
 * one.  The naive version - EXEC TABLEn.PRG as the boot program - is what
 * -p already does, and it is why quitting a table booted that way dies: the
 * tables are not standalone.  The boot program installs an INT 65h API, EXECs
 * the intro, then loops EXECing Table<n>.Prg; a table that quits hands the
 * next program index back through that API and terminates, expecting the loop
 * to still be there.  Boot it alone and there is no handler and no loop.
 *
 * So the boot program stays exactly where it is, resident, with its API and
 * its loop intact, and only its FIRST EXEC is redirected from the intro to a
 * table.  Everything after that is the game's own control flow: quitting the
 * table returns to the loop, which goes back to the menu as it always does.
 *
 * Two locators in the boot program's resident segment make it work, both
 * derived from the loaded image the way cfg_buf is derived from the intro's
 * rather than baked in as constants - Deluxe puts them at CS:01F7/CS:0020,
 * and there is no reason for PF.EXE to agree:
 *
 *   blob  the six bytes INT 65h AX=0100 stashes and AX=0200 hands back.  The
 *         tables read their options from here, NOT from PINBALL.CFG - a scan
 *         of TABLE1.PRG finds no six-byte config transfer at all, which is
 *         why skipping the intro would otherwise silently drop every setting
 *         the launcher offers.  Poking it is what keeps them.
 *   next  the one-byte program index the main loop EXECs through its 12-byte
 *         name table, set by INT 65h AX=FFFF.  Not written here (the first
 *         EXEC is redirected instead), but deriving it confirms the handler
 *         really is the one this code thinks it is.
 *
 * Signatures, from the handler's own dispatch:
 *   MOV DI,imm16 / MOV CX,6 / REP MOVSB      the 0100 stash  -> blob
 *   MOV SI,imm16 / MOV CX,6 / REP MOVSB      the 0200 fetch  -> blob (agree)
 *   CMP AX,FFFFh / JNZ rel8 / MOV CS:[imm16],BL              -> next
 * Both blob sites must name the same address; disagreement, or a miss on
 * either, leaves the whole feature off rather than half-configured. */
static uint32_t boot_base = 0;   /* linear load address of the boot program */
static uint32_t boot_blob = 0;   /* CS offset of the six-byte options blob */
static uint32_t boot_next = 0;   /* CS offset of the next-program index */
static int boot_locs = 0;        /* both derived and in agreement */
static int start_table = 0;      /* 1-4: redirect the first EXEC to this */
static int start_done = 0;       /* ...which has now happened */

void fantasies_set_start_table(int n){
    start_table = (n >= 1 && n <= 4) ? n : 0;
    start_done = 0;
}
int fantasies_start_table(void){ return start_table; }

void fantasies_patch_boot(const char *dospath, uint32_t load_base,
                          uint32_t imglen, uint32_t cs_base){
    static const uint8_t st[8]  = {0xBF,0,0,0xB9,0x06,0x00,0xF3,0xA4};
    static const uint8_t stm[8] = {1,0,0,1,1,1,1,1};
    static const uint8_t fe[8]  = {0xBE,0,0,0xB9,0x06,0x00,0xF3,0xA4};
    static const uint8_t nx[8]  = {0x3D,0xFF,0xFF,0x75,0,0x2E,0x88,0x1E};
    static const uint8_t nxm[8] = {1,1,1,1,0,1,1,1};
    char b[64];
    uint32_t at = 0, blob_st = 0, blob_fe = 0;
    int h;
    boot_base = boot_blob = boot_next = 0;
    boot_locs = 0;
    if(!session_armed || dos_no_patch || !rel) return;
    if(load_base + imglen > RAM_SIZE) return;
    base_up(dospath, b, sizeof(b));
    if(_stricmp(b, rel->boot)) return;      /* only the boot program */
    /* Scanning is over the whole loaded image, but the operands those
     * instructions carry are CS-relative, so the data lives at the CS
     * base.  Using the load base instead works only by coincidence on a
     * build whose e_cs is 0. */
    boot_base = cs_base;

    h = scan_sig(load_base, imglen, st, stm, sizeof(st), &at);
    if(h != 1){
        trc("[fantasies] INT 65h stash site %s; direct-to-table off\n",
            h ? "ambiguous" : "not found");
        return;
    }
    blob_st = (uint32_t)ram[load_base+at+1] | ((uint32_t)ram[load_base+at+2] << 8);
    h = scan_sig(load_base, imglen, fe, stm, sizeof(fe), &at);
    if(h != 1){
        trc("[fantasies] INT 65h fetch site %s; direct-to-table off\n",
            h ? "ambiguous" : "not found");
        return;
    }
    blob_fe = (uint32_t)ram[load_base+at+1] | ((uint32_t)ram[load_base+at+2] << 8);
    if(blob_st != blob_fe){
        trc("[fantasies] INT 65h stash CS:%04X and fetch CS:%04X disagree; "
            "direct-to-table off\n", blob_st, blob_fe);
        return;
    }
    h = scan_sig(load_base, imglen, nx, nxm, sizeof(nx), &at);
    if(h != 1){
        trc("[fantasies] INT 65h next-program site %s; direct-to-table off\n",
            h ? "ambiguous" : "not found");
        return;
    }
    boot_next = (uint32_t)ram[load_base+at+8] | ((uint32_t)ram[load_base+at+9] << 8);
    boot_blob = blob_st;
    boot_locs = 1;
    trc("[fantasies] '%s' INT 65h: options blob CS:%04X, next program CS:%04X"
        " (linear %05X/%05X)\n", b, boot_blob, boot_next,
        boot_base + boot_blob, boot_base + boot_next);
}

int fantasies_direct_ready(void){ return boot_locs; }

/* Skip the boot program's first EXEC of the intro, so its loop runs the
 * wanted table in the slot that owns it.  The loop is:
 *
 *     again:  EXEC intro                  <- skipped, once
 *             if(next == 0) exit
 *             EXEC name_table[next]       <- our table lands here
 *             goto again                  <- quit returns to the real intro
 *
 * Redirecting the intro EXEC to the table instead ran it twice: once in the
 * intro slot, then again in the table slot because next still pointed at it,
 * which is exactly what "quitting restarted the table" was.  Skipping leaves
 * the game's own structure untouched, so the way out of a table is the way
 * it has always been.
 *
 * Called from the INT 21h AH=4Bh handler for every EXEC, so the guards are
 * strict: armed, a table actually requested, locators derived, not already
 * done, and the name really is this release's intro. */
int fantasies_exec_skip(const char *dospath){
    char b[64];
    uint32_t a;
    int i;
    if(!start_table || start_done || !session_armed || dos_no_patch) return 0;
    if(!rel || !rel->layout) return 0;
    base_up(dospath, b, sizeof(b));
    if(prog_slot(b) != 0) return 0;             /* not the intro */
    if(start_table >= rel->layout->n) return 0;
    if(!boot_locs){
        /* Refused rather than approximated: without the blob the table would
         * run on its built-in defaults and silently ignore every launcher
         * option, which is worse than starting at the menu. */
        fprintf(stderr, "[fantasies] direct-to-table unavailable for '%s'"
                        " (INT 65h layout not derived); starting at the menu\n",
                rel->id);
        start_table = 0;
        return 0;
    }
    /* The options the intro would have stashed on its way out.  The tables
     * read these through INT 65h AX=0200, never from PINBALL.CFG. */
    if(!options_loaded) load_options_cache();
    a = boot_base + boot_blob;
    for(i = 0; i < 6; i++) mem_w8(a + (uint32_t)i, options_cache[i]);
    /* ...and the choice the intro would have made. */
    mem_w8(boot_base + boot_next, (uint8_t)start_table);
    start_done = 1;
    fprintf(stderr, "[fantasies] direct to table %d: intro skipped,"
                    " options and next program set\n", start_table);
    trc("[fantasies] options at %05X, next byte %05X = %02X\n",
        a, boot_base + boot_next, ram[boot_base + boot_next]);
    return 1;
}

/* Mid-table savestate (src/snapshot.c): the session gating, the resolved
 * code locators (addresses, not bytes - the RAM image travels separately),
 * the learned present window, and the ball/camera coherence scalars.
 * trainer_enabled is always 0 here (saving with it on is refused); mat_*
 * and the ballgap histograms are diagnostics and re-learn. */
void fantasies_save_state(SnapW *w){
    snap_w_u32(w, (uint32_t)session_armed);
    snap_w_bytes(w, rel ? rel->id : "", rel ? strlen(rel->id)+1 : 1);
    snap_w_u32(w, (uint32_t)fix_on);
    snap_w_u32(w, cfg_buf);
    snap_w_bytes(w, session_dir, sizeof(session_dir));
    snap_w_u32(w, (uint32_t)table_num);
    snap_w_u32(w, (uint32_t)spring_cheat_on);
    snap_w_bytes(w, options_cache, sizeof(options_cache));
    snap_w_u32(w, (uint32_t)options_loaded);
    snap_w_bytes(w, pause_addr_pauseflag, sizeof(pause_addr_pauseflag));
    snap_w_bytes(w, pause_addr_last_was_vb, sizeof(pause_addr_last_was_vb));
    snap_w_bytes(w, pause_prev_flag, sizeof(pause_prev_flag));
    snap_w_bytes(w, balls_addr, sizeof(balls_addr));
    snap_w_bytes(w, balls_counter_val, sizeof(balls_counter_val));
    snap_w_bytes(w, tilt_addr, sizeof(tilt_addr));
    snap_w_bytes(w, tilt_orig, sizeof(tilt_orig));
    snap_w_bytes(w, spring_valid_addr, sizeof(spring_valid_addr));
    snap_w_bytes(w, jump_vel_addr, sizeof(jump_vel_addr));
    snap_w_u32(w, balldbg_entry);
    snap_w_u32(w, balldbg_exit);
    snap_w_u32(w, balldbg_pos);
    snap_w_bytes(w, pw_occ, sizeof(pw_occ));
    snap_w_u32(w, pw_n);
    snap_w_u32(w, (uint32_t)pw_vt);
    snap_w_u32(w, (uint32_t)pw_valid);
    snap_w_dbl(w, pw_lo);
    snap_w_dbl(w, pw_hi);
    snap_w_u32(w, bg_last_start);
    snap_w_u32(w, (uint32_t)bg_have_start);
    snap_w_dbl(w, bg_last_ball_t);
    /* In-flight ball-draw span.  The entry hook sets bg_inside/bg_line0 and
     * the position hook consumes them; a snapshot taken between the two
     * dropped that pairing on load, losing exactly one pw_mark() sample and
     * leaving the restored run one present-window observation behind a run
     * that was never interrupted.  Render-path only - the guest never sees
     * it - but it is real state, so it travels. */
    snap_w_u32(w, (uint32_t)bg_inside);
    snap_w_dbl(w, bg_t0);
    snap_w_dbl(w, bg_line0);
    snap_w_u32(w, (uint32_t)bg_vtotal);
}
int fantasies_load_state(SnapR *r){
    char id[64];
    size_t idl = 0;
    session_armed = (int)snap_r_u32(r);
    /* NUL-terminated rel id. */
    while(idl + 1 < sizeof(id)){
        id[idl] = (char)snap_r_u8(r);
        if(r->err) return -1;
        if(id[idl] == 0) break;
        idl++;
    }
    if(id[idl] != 0) return -1;
    rel = id[0] ? release_by_id(id) : NULL;
    if(session_armed && id[0] && !rel) return -1;
    fix_on = (int)snap_r_u32(r);
    cfg_buf = snap_r_u32(r);
    snap_r_bytes(r, session_dir, sizeof(session_dir));
    if(r->err) return -1;
    session_dir[sizeof(session_dir)-1] = 0;
    table_num = (int)snap_r_u32(r);
    spring_cheat_on = (int)snap_r_u32(r);
    snap_r_bytes(r, options_cache, sizeof(options_cache));
    options_loaded = (int)snap_r_u32(r);
    snap_r_bytes(r, pause_addr_pauseflag, sizeof(pause_addr_pauseflag));
    snap_r_bytes(r, pause_addr_last_was_vb, sizeof(pause_addr_last_was_vb));
    snap_r_bytes(r, pause_prev_flag, sizeof(pause_prev_flag));
    snap_r_bytes(r, balls_addr, sizeof(balls_addr));
    snap_r_bytes(r, balls_counter_val, sizeof(balls_counter_val));
    snap_r_bytes(r, tilt_addr, sizeof(tilt_addr));
    snap_r_bytes(r, tilt_orig, sizeof(tilt_orig));
    snap_r_bytes(r, spring_valid_addr, sizeof(spring_valid_addr));
    snap_r_bytes(r, jump_vel_addr, sizeof(jump_vel_addr));
    balldbg_entry = snap_r_u32(r);
    balldbg_exit = snap_r_u32(r);
    balldbg_pos = snap_r_u32(r);
    snap_r_bytes(r, pw_occ, sizeof(pw_occ));
    pw_n = snap_r_u32(r);
    pw_vt = (int)snap_r_u32(r);
    pw_valid = (int)snap_r_u32(r);
    pw_lo = snap_r_dbl(r);
    pw_hi = snap_r_dbl(r);
    bg_last_start = snap_r_u32(r);
    bg_have_start = (int)snap_r_u32(r);
    bg_last_ball_t = snap_r_dbl(r);
    bg_inside = (int)snap_r_u32(r);
    bg_t0 = snap_r_dbl(r);
    bg_line0 = snap_r_dbl(r);
    bg_vtotal = (int)snap_r_u32(r);
    return r->err ? -1 : 0;
}
