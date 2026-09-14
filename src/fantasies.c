/* Pinball Fantasies game-specific fixes.
 *
 * Everything that applies to Fantasies (and only Fantasies) lives in this
 * file, so dos.c/dev.c stay generic:
 *
 *  - the INTRO.PRG manual-lookup image patch (memory-only, signature-checked,
 *    -nopatch disables);
 *  - the flipper fix session/exec gating (see below).
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
        return;
    }
    base_up(dospath, b, sizeof(b));
    if(is_table_prog(b)){
        if(!fix_on) trc("[fantasies] flipper fix on (%s)\n", b);
        fix_on = 1;
        return;
    }
    dot = strrchr(b,'.');
    if(dot && (!strcmp(dot,".SDR")||!strcmp(dot,".BIN")||!strcmp(dot,".MOD")))
        return;                         /* child driver/data: keep state */
    if(dot && (!strcmp(dot,".PRG")||!strcmp(dot,".EXE")||!strcmp(dot,".COM"))){
        if(fix_on) trc("[fantasies] flipper fix off (%s)\n", b);
        fix_on = 0;
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
            " ([%04X] preset 1)\n", i + 6, dcell);
        return;
    }
}

/* INTRO.PRG manual-lookup protection, patched in the loaded image rather
 * than on disk (moved verbatim from dos.c): INTRO.PRG asks for a word from
 * the manual and branches on the result with a JNC; CRACK.COM (which shipped
 * with the game) turns that into a JMP.  The signature check means a
 * different build is left alone instead of being corrupted. */
void fantasies_patch_image(uint32_t load_base, uint32_t imglen){
    if(!dos_no_patch && imglen > 238210){
        static const uint8_t sig[6] = { 0x81, 0xFB, 0xE7, 0x51, 0x73, 0x32 };
        uint32_t at = load_base + 238204;
        if(memcmp(&ram[at], sig, sizeof(sig)) == 0){
            ram[at + 4] = 0xEB;                     /* JNC -> JMP */
            trc("[dos] manual check patched in memory at image+238208\n");
        }
    }
}
