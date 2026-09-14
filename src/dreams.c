/* Pinball Dreams game-specific fixes.
 *
 * Counterpart to fantasies.c: everything Dreams-only lives here so dos.c
 * stays generic.  Currently just the PD.EXE manual-lookup image patch
 * (moved verbatim from dos.c).  Memory-only, signature-checked, -nopatch
 * disables.
 */
#include "pfemu.h"

/* PD.EXE compares the typed length against the expected length (JNE to fail)
 * then checksums the answer uppercased (JE to pass), with retries and a
 * silent exit on failure.  Forcing only the checksum JE is not enough - a
 * wrong-length word (e.g. "aaa") fails earlier and never reaches it, which is
 * why the prompt retried three times.  Retarget the length JNE at its pass
 * path too, so every input passes both gates. */
void dreams_patch_image(uint32_t load_base, uint32_t imglen){
    if(!dos_no_patch && imglen > 0x7022){
        static const uint8_t sig[30] = { 0x8D,0x1E,0xC9,0x00,0x8A,0x0F,0x3A,0x4C,0x03,0x75,
                                         0x13,0x2B,0xC0,0x2A,0xED,0x43,0x8A,0x27,0x80,0xE4,
                                         0xDF,0x02,0xC4,0xE2,0xF6,0x3A,0x44,0x04,0x74,0x05 };
        uint32_t at = load_base + 0x7004;
        if(at + 30 <= RAM_SIZE && memcmp(&ram[at], sig, sizeof(sig)) == 0){
            ram[at + 10] = 0x18;                    /* JNE fail -> JNE pass */
            ram[at + 28] = 0xEB;                    /* JE -> JMP */
            trc("[dos] dreams manual check patched in memory at image+0x700D/0x7020\n");
        }
    }
}
