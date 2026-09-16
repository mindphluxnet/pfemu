/* Pinball Fantasies game-specific fixes.
 *
 * Everything that applies to Fantasies (and only Fantasies) lives in this
 * file, so dos.c/dev.c stay generic:
 *
 *  - the INTRO.PRG manual-lookup image patch (memory-only, signature-checked,
 *    -nopatch disables);
 *  - the flipper fix session/exec gating (see below);
 *  - the trainer hotkeys ('1'/'2', see fantasies_key_event below);
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
 * not enough - a sibling game could ship a same-named file.  So main() arms
 * this session from the launcher choice (or its CLI equivalent: -d/-p), and
 * fantasies_on_exec() then narrows it to the table programs.  Driver/data
 * children (.SDR/.BIN/.MOD) leave the state unchanged so a table keeps the
 * fix across EXECing its own sound driver.
 */
#include "pfemu.h"

extern double emu_time;

static int session_armed = 0;   /* user booted Fantasies (launcher or CLI equiv) */
static int fix_on = 0;          /* a TABLE1-4.PRG is currently running */
static char session_dir[512];   /* game directory, for the options file below */
static int table_num = 0;              /* 1-4 while a table is running, else 0 */
static int trainer_enabled = 0;        /* launcher: arm the '1'/'2' hotkeys below */
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

static int is_table_prog(const char *base){
    return strlen(base)==10 && memcmp(base,"TABLE",5)==0 &&
           base[5]>='1' && base[5]<='4' && memcmp(base+6,".PRG",4)==0;
}

/* Host-only launcher toggle file (src/launch.c writes it, the "Enable
 * trainer" checkbox): 2 identical bytes (old layout, kept so a config saved
 * by an earlier build of the launcher - which had two separate checkboxes -
 * still reads as "enabled" if either was on).  Nonzero just ARMS the '1'/'2'
 * hotkeys in fantasies_key_event() below; it does not itself turn either
 * cheat on.  Every fresh table load is the game's own unpatched image (see
 * fantasies_on_exec below - it no longer pre-applies anything), so with the
 * trainer enabled, both cheats still start OFF and stay off until the
 * player actually presses '1' or '2'.  Kept out of the 6-byte
 * pfemu_options.cfg blob since that one specifically mirrors PINBALL.CFG's
 * own layout. */
static void load_cheat_cfg(const char *dir){
    char path[600];
    FILE *f;
    uint8_t b[2] = {0,0};
    snprintf(path, sizeof(path), "%s/PFEMU-STATE/pfemu_cheats.cfg", dir);
    f = fopen(path, "rb");
    if(f){ if(fread(b,1,2,f) != 2){ b[0]=0; b[1]=0; } fclose(f); }
    trainer_enabled = (b[0] != 0) || (b[1] != 0);
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
    table_num = 0;
    snprintf(session_dir, sizeof(session_dir), "%s", dir ? dir : "");
    if(session_armed) load_cheat_cfg(session_dir);
    if(!session_armed) kbd_clear_held();
    trc("[fantasies] session %s (dir=%s prog=%s)\n",
        session_armed ? "armed" : "not armed", db, pb);
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
        table_num = b[5] - '0';
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
    base_up(fname, up, sizeof(up));
    if(strcmp(up, "CD.NFO")) return NULL;
    f = tmpfile();
    if(!f) return NULL;
    fputc('C', f);
    rewind(f);
    trc("[fantasies] cd.nfo presence faked (Deluxe CD-ROM check)\n");
    return f;
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
    tn = b[5] - '0';
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
    tn = b[5] - '0';
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

/* Called from dev.c's kbd_key() on every fresh (non-autorepeat) key make,
 * host scancode already stripped of the E0 prefix bit. '1' and '2' are
 * plain, unextended scancodes, so no E0 check is needed here.  Gated on
 * trainer_enabled (the launcher's "Enable trainer" checkbox, see
 * load_cheat_cfg above): unchecked, both hotkeys are completely inert - not
 * just "cheats start off", but no keypress here ever touches memory at
 * all - same as before this feature existed. */
void fantasies_key_event(int scancode, int down){
    int r;
    if(!down || !trainer_enabled || !fantasies_fix_active() || !table_num || dos_no_patch)
        return;
    if(scancode == 0x02){
        r = fantasies_toggle_balls();
        if(r >= 0) osd_show(r ? "INFINITE BALLS: ON" : "INFINITE BALLS: OFF");
    } else if(scancode == 0x03){
        r = fantasies_toggle_spring();
        if(r >= 0) osd_show(r ? "BALL CONTROL: ON" : "BALL CONTROL: OFF");
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
 * avoid - docs/OPTIMIZATIONS.md §16 locked presents to vsync, which is
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
