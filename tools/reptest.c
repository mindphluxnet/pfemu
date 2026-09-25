/* Regression test for REP MOVS overlap semantics (src/cpu.c, strop).
 *
 * The bulk fast path there once handed overlapping forward copies to memmove,
 * which is specified to behave as if the source were read through a temporary
 * - the opposite of what the instruction does.  x86 copies one element at a
 * time in order, so a destination inside the source run reads back what it
 * just wrote and the run propagates.  That is the LZ77 run-expansion idiom
 * every self-extracting executable emits, and it silently corrupted
 * PKLITE-packed sound drivers.
 *
 * Build and run from the repo root:
 *   cl /nologo /W3 /wd4996 /I src /Fe:reptest.exe tools/reptest.c src/cpu.c
 *   reptest.exe
 *
 * Every case is checked against a reference that does it element by element,
 * and the non-overlapping and just-clear-of-the-run cases are here so a fix
 * that simply disables the fast path does not pass quietly.
 */
#include <stdio.h>
#include <stdarg.h>
#include "pfemu.h"

int trace_level = 0;
void trc(const char *fmt, ...){ (void)fmt; }

/* --- stubs for everything cpu.c calls out to --- */
uint8_t vga_mem_r(uint32_t a){ return ram[a]; }
void vga_mem_w(uint32_t a, uint8_t v){ ram[a] = v; }
uint8_t io_r8(uint16_t p){ (void)p; return 0xFF; }
uint16_t io_r16(uint16_t p){ (void)p; return 0xFFFF; }
void io_w8(uint16_t p, uint8_t v){ (void)p; (void)v; }
void io_w16(uint16_t p, uint16_t v){ (void)p; (void)v; }
void fantasies_ballgap_exec(uint32_t l){ (void)l; }
void fantasies_matrix_exec(uint32_t l){ (void)l; }
int mat_dbg = 0;
uint32_t mat_tick_site = 0, mat_call_site = 0, mat_crisis_site = 0;
int balldbg_on = 0;
uint32_t balldbg_pos = 0, balldbg_entry = 0, balldbg_exit = 0;
void (*cb_table[256])(void);
/* cpu.c owns memwatch_addr and memwatch_hit; it only needs the clock here. */
double emu_now(void){ return 0.0; }

#define CODE 0x20000u
#define DATA 0x30000u

static int fails = 0;

/* Reference: exactly what the hardware does - one element, in order, in the
 * direction DF selects.  Direction is not cosmetic: it decides which way an
 * overlap propagates, so a backwards copy needs its own reference rather than
 * a reversed forward one. */
static void ref_movs(uint8_t *m, uint32_t d, uint32_t s, uint32_t cnt, int el, int df){
    uint32_t k; int e;
    int32_t step = df ? -el : el;
    for(k = 0; k < cnt; k++){
        uint8_t tmp[4];
        uint32_t sk = (uint32_t)((int32_t)s + (int32_t)k * step);
        uint32_t dk = (uint32_t)((int32_t)d + (int32_t)k * step);
        for(e = 0; e < el; e++) tmp[e] = m[sk + e];
        for(e = 0; e < el; e++) m[dk + e] = tmp[e];
    }
}

static void one(const char *name, int el, uint32_t cnt, uint32_t soff, uint32_t doff, int df){
    static uint8_t want[0x20000];
    uint32_t i;
    uint8_t opc = el == 1 ? 0xA4 : 0xA5;
    int32_t adv = (df ? -el : el) * (int32_t)cnt;

    /* A recognisable pattern, so a wrong copy cannot accidentally match. */
    for(i = 0; i < 0x4000; i++) ram[DATA + i] = (uint8_t)(i * 7 + 11);
    memcpy(want, &ram[DATA], 0x4000);
    ref_movs(want, doff, soff, cnt, el, df);

    cpu_reset();
    set_sreg(S_CS, CODE >> 4); cpu.eip = 0;
    set_sreg(S_DS, DATA >> 4); set_sreg(S_ES, DATA >> 4);
    REG16(R_ESI) = (uint16_t)soff; REG16(R_EDI) = (uint16_t)doff;
    REG16(R_ECX) = (uint16_t)cnt;
    /* STD/CLD as a real instruction, so the flag is set the way the guest
     * sets it rather than poked into the struct behind the decoder's back. */
    ram[CODE + 0] = df ? 0xFD : 0xFC;
    ram[CODE + 1] = 0xF3; ram[CODE + 2] = opc;   /* REP MOVSB / REP MOVSW */
    cpu_step();
    cpu_step();

    if(memcmp(want, &ram[DATA], 0x4000)){
        uint32_t bad = 0;
        while(bad < 0x4000 && want[bad] == ram[DATA + bad]) bad++;
        printf("  FAIL %-46s first wrong byte at +0x%X (want %02X got %02X)\n",
               name, bad, want[bad], ram[DATA + bad]);
        fails++;
    } else if(REG16(R_ECX) != 0 ||
              REG16(R_ESI) != (uint16_t)((int32_t)soff + adv) ||
              REG16(R_EDI) != (uint16_t)((int32_t)doff + adv)){
        printf("  FAIL %-46s registers: cx=%04X si=%04X di=%04X\n",
               name, REG16(R_ECX), REG16(R_ESI), REG16(R_EDI));
        fails++;
    } else {
        printf("  ok   %s\n", name);
    }
}

int main(void){
    ram = (uint8_t*)calloc(RAM_SIZE, 1);
    if(!ram) return 2;

    puts("non-overlapping (the fast path must still be taken):");
    one("movsb cnt=64 src<dst far apart",        1, 64,  0x0100, 0x1000, 0);
    one("movsb cnt=64 dst<src far apart",        1, 64,  0x1000, 0x0100, 0);
    one("movsw cnt=64 far apart",                2, 64,  0x0100, 0x1000, 0);
    one("movsb cnt=8 (below bulk threshold)",    1, 8,   0x0100, 0x1000, 0);

    puts("\noverlapping, destination behind source (memmove-safe direction):");
    one("movsb cnt=64 dst=src-1",                1, 64,  0x1001, 0x1000, 0);
    one("movsb cnt=64 dst=src-32",               1, 64,  0x1020, 0x1000, 0);

    puts("\noverlapping run expansion (the LZ77 idiom a packer emits):");
    one("movsb cnt=64 dst=src+1  (byte run)",    1, 64,  0x1000, 0x1001, 0);
    one("movsb cnt=64 dst=src+2  (word run)",    1, 64,  0x1000, 0x1002, 0);
    one("movsb cnt=64 dst=src+3",                1, 64,  0x1000, 0x1003, 0);
    one("movsb cnt=200 dst=src+5",               1, 200, 0x1000, 0x1005, 0);
    one("movsb cnt=64 dst=src+63 (just inside)", 1, 64,  0x1000, 0x103F, 0);
    one("movsb cnt=64 dst=src+64 (just clear)",  1, 64,  0x1000, 0x1040, 0);
    one("movsw cnt=64 dst=src+2  (word run)",    2, 64,  0x1000, 0x1002, 0);
    one("movsw cnt=64 dst=src+4",                2, 64,  0x1000, 0x1004, 0);
    one("movsb cnt=16 dst=src+1  (at threshold)",1, 16,  0x1000, 0x1001, 0);

    /* Backwards.  The sound driver inserts an event into its callback list
     * with `std; rep movsb` at DI = SI + 9, shifting the tail up to open a
     * slot - so this direction is on the path that decides whether a
     * registered callback is ever dispatched, not a curiosity. */
    puts("\nbackwards (STD), the event-list insert idiom a driver emits:");
    one("std movsb cnt=81 dst=src+9  (list shift)", 1, 81, 0x1000, 0x1009, 1);
    one("std movsb cnt=64 dst=src+1",            1, 64,  0x1000, 0x1001, 1);
    one("std movsw cnt=64 dst=src+2",            2, 64,  0x1000, 0x1002, 1);
    one("std movsb cnt=64 far apart",            1, 64,  0x1000, 0x2000, 1);
    one("std movsb cnt=8 (below threshold)",     1, 8,   0x1000, 0x1009, 1);

    puts("\nbackwards run expansion (propagates the other way round):");
    one("std movsb cnt=64 dst=src-1",            1, 64,  0x1040, 0x103F, 1);
    one("std movsb cnt=64 dst=src-3",            1, 64,  0x1040, 0x103D, 1);
    one("std movsw cnt=64 dst=src-2",            2, 64,  0x1080, 0x107E, 1);

    printf("\n%s (%d failure%s)\n", fails ? "FAILED" : "all passed",
           fails, fails == 1 ? "" : "s");
    return fails != 0;
}
