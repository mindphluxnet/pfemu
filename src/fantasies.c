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
