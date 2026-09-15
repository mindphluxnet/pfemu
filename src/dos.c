/* A small MS-DOS 5 work-alike: MCB memory chain, PSPs, file handles, EXEC */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pfemu.h"
#include <sys/stat.h>
#include <direct.h>
#include <time.h>

extern void (*cb_table[256])(void);
extern void set_sreg(int s, uint16_t v);
extern void cpu_no_iret(void);
extern void bios_set_cf(int v);
extern void bios_tty(uint8_t c);
extern double emu_time;

#define AX REG16(R_EAX)
#define BX REG16(R_EBX)
#define CX REG16(R_ECX)
#define DX REG16(R_EDX)
#define SI REG16(R_ESI)
#define DI REG16(R_EDI)
#define AL REG8(0)
#define CL REG8(1)
#define DL REG8(2)
#define BL REG8(3)
#define AH REG8(4)
#define CH REG8(5)
#define DH REG8(6)
#define BH REG8(7)

int dos_done = 0;
int dos_no_patch = 0;      /* -nopatch : leave the manual check in place */
static char gamedir[512];

#define MEM_FIRST 0x0060
#define MEM_LAST  0x9FC0

/* ------------------------------------------------------------------ MCB */
static void mcb_init(void){
    uint32_t a = MEM_FIRST*16;
    ram[a] = 'Z';
    *(uint16_t*)&ram[a+1] = 0;
    *(uint16_t*)&ram[a+3] = (uint16_t)(MEM_LAST - MEM_FIRST - 1);
    memset(&ram[a+8], 0, 8);
}
/* DOS allocation strategy (INT 21h AH=58h): 0 first fit, 1 best fit,
 * 2 last fit.  Programs that install a resident driver switch to last fit so
 * the driver lands at the top of memory instead of fragmenting the middle -
 * Pinball Fantasies depends on this, or the 524 KB table program no longer
 * fits once the sound driver has gone resident. */
uint16_t dos_alloc_strategy = 0;

static uint16_t mcb_alloc(uint16_t paras, uint16_t owner, uint16_t *largest){
    uint16_t seg = MEM_FIRST;
    uint16_t chosen = 0, chosen_sz = 0;
    int strat = dos_alloc_strategy & 3;
    *largest = 0;
    for(;;){
        uint32_t a = (uint32_t)seg*16;
        uint8_t sig = ram[a];
        uint16_t own = *(uint16_t*)&ram[a+1];
        uint16_t sz  = *(uint16_t*)&ram[a+3];
        if(sig!='M' && sig!='Z') return 0;
        if(own==0){
            if(sz > *largest) *largest = sz;
            if(sz >= paras){
                if(!chosen) { chosen = seg; chosen_sz = sz; }
                else if(strat==1 && sz < chosen_sz){ chosen = seg; chosen_sz = sz; }
                else if(strat==2){ chosen = seg; chosen_sz = sz; }
                if(strat==0) break;
            }
        }
        if(sig=='Z') break;
        seg = (uint16_t)(seg + sz + 1);
    }
    if(!chosen) return 0;
    {
        uint32_t a = (uint32_t)chosen*16;
        uint8_t sig = ram[a];
        uint16_t sz = chosen_sz;
        if(strat==2 && sz > paras + 1){
            /* last fit: carve the block off the TOP of the free area */
            uint16_t nseg = (uint16_t)(chosen + (sz - paras - 1) + 1);
            uint32_t na = (uint32_t)nseg*16;
            ram[na] = sig;
            *(uint16_t*)&ram[na+1] = owner;
            *(uint16_t*)&ram[na+3] = paras;
            memset(&ram[na+8],0,8);
            ram[a] = 'M';
            *(uint16_t*)&ram[a+3] = (uint16_t)(sz - paras - 1);
            return (uint16_t)(nseg + 1);
        }
        if(sz > paras + 1){
            uint16_t nseg = (uint16_t)(chosen + paras + 1);
            uint32_t na = (uint32_t)nseg*16;
            ram[na] = sig;
            *(uint16_t*)&ram[na+1] = 0;
            *(uint16_t*)&ram[na+3] = (uint16_t)(sz - paras - 1);
            memset(&ram[na+8],0,8);
            ram[a] = 'M';
            *(uint16_t*)&ram[a+3] = paras;
        }
        *(uint16_t*)&ram[a+1] = owner;
        return (uint16_t)(chosen+1);
    }
}
static void mcb_coalesce(void){
    uint16_t seg = MEM_FIRST;
    for(;;){
        uint32_t a = (uint32_t)seg*16;
        uint8_t sig = ram[a];
        uint16_t own = *(uint16_t*)&ram[a+1];
        uint16_t sz  = *(uint16_t*)&ram[a+3];
        if(sig=='Z') break;
        if(own==0){
            uint16_t nseg = (uint16_t)(seg + sz + 1);
            uint32_t na = (uint32_t)nseg*16;
            if((ram[na]=='M'||ram[na]=='Z') && *(uint16_t*)&ram[na+1]==0){
                *(uint16_t*)&ram[a+3] = (uint16_t)(sz + *(uint16_t*)&ram[na+3] + 1);
                ram[a] = ram[na];
                continue;
            }
        }
        seg = (uint16_t)(seg + sz + 1);
    }
}
/* A segment only names a block if there is room for an MCB paragraph below it
 * and the block lies inside conventional memory.  The intro hands us 0000 on
 * one of its error paths - (uint16_t)(0-1)*16 is 0xFFFFFFF0, which indexed
 * straight off the end of the RAM array. */
static int mcb_valid(uint16_t seg){
    return seg > MEM_FIRST && (uint32_t)seg*16 < RAM_SIZE;
}
static int mcb_free(uint16_t seg){
    uint32_t a;
    if(!mcb_valid(seg)) return 0;
    a = (uint32_t)(seg-1)*16;
    if(ram[a]!='M' && ram[a]!='Z') return 0;
    *(uint16_t*)&ram[a+1] = 0;
    mcb_coalesce();
    return 1;
}
static int mcb_resize(uint16_t seg, uint16_t paras, uint16_t *avail){
    uint32_t a;
    uint16_t sz;
    uint8_t sig;
    if(!mcb_valid(seg)){ if(avail) *avail = 0; return 0; }
    a = (uint32_t)(seg-1)*16;
    sz = *(uint16_t*)&ram[a+3];
    sig = ram[a];
    if(paras <= sz){
        if(sz > paras){
            uint16_t nseg = (uint16_t)(seg + paras);
            uint32_t na = (uint32_t)nseg*16;
            ram[na] = sig;
            *(uint16_t*)&ram[na+1] = 0;
            *(uint16_t*)&ram[na+3] = (uint16_t)(sz - paras - 1);
            ram[a] = 'M';
            *(uint16_t*)&ram[a+3] = paras;
            mcb_coalesce();
        }
        *avail = paras; return 1;
    }
    {   /* try to grow into the following free block */
        uint16_t nseg = (uint16_t)(seg + sz);
        uint32_t na = (uint32_t)nseg*16;
        if(sig=='M' && (ram[na]=='M'||ram[na]=='Z') && *(uint16_t*)&ram[na+1]==0){
            uint16_t total = (uint16_t)(sz + *(uint16_t*)&ram[na+3] + 1);
            if(total >= paras){
                ram[a] = ram[na];
                *(uint16_t*)&ram[a+3] = total;
                return mcb_resize(seg, paras, avail);
            }
            *avail = total; return 0;
        }
    }
    *avail = sz;
    return 0;
}

/* Release memory still held by resident children of `parent`.
 *
 * Strict DOS keeps a TSR alive forever, but this game's lifecycle depends on
 * the sound driver going away with the program that installed it: INTRO.PRG
 * shrinks itself to 238 KB and then EXECs a .SDR driver, which goes resident
 * with INT 21h AH=31h right above it.  If that block survived, the free store
 * would be split into 238 KB + 392 KB and the 524 KB TABLEn.PRG could never
 * load again - and every table switch would leak another driver.  Treating a
 * resident child as owned by its parent keeps conventional memory contiguous,
 * which is the behaviour the game was clearly written against. */
/* Interrupt vector table as it stood when the last child was EXEC'd, so that
 * releasing that child can undo the hooks it installed - see the note above
 * mcb_free_children and the "uninstall" discussion in NOTES.md. */
uint32_t ivt_snap[256];
int ivt_snap_valid = 0;

void dos_snapshot_ivt(void){
    int i;
    for(i=0;i<256;i++) ivt_snap[i] = mem_r32((uint32_t)i*4);
    ivt_snap_valid = 1;
}

/* Undo the interrupt hooks of a block that is being taken away.  A TSR that
 * unloaded itself properly would restore these itself; since we are the ones
 * pulling its memory out from under it, we have to do it on its behalf, or the
 * next program to be loaded over that memory gets control on the next IRQ and
 * executes its own bitmap data. */
static void ivt_unhook_range(uint16_t lo_seg, uint16_t hi_seg){
    uint32_t lo = (uint32_t)lo_seg * 16, hi = (uint32_t)hi_seg * 16 + 15;
    int i, n = 0;
    if(!ivt_snap_valid) return;
    for(i=0;i<256;i++){
        uint32_t v = mem_r32((uint32_t)i*4);
        uint32_t lin = ((v >> 16) << 4) + (v & 0xFFFF);
        if(lin >= lo && lin <= hi && v != ivt_snap[i]){
            mem_w32((uint32_t)i*4, ivt_snap[i]);
            n++;
            trc("[dos]   int %02X restored %08X -> %08X\n", i, v, ivt_snap[i]);
        }
    }
    if(n) trc("[dos]   %d vector(s) unhooked from %04X..%04X\n", n, lo_seg, hi_seg);
}

static void mcb_free_children(uint16_t parent){
    uint16_t seg = MEM_FIRST;
    for(;;){
        uint32_t a = (uint32_t)seg*16;
        uint8_t sig = ram[a];
        uint16_t own = *(uint16_t*)&ram[a+1];
        uint16_t sz  = *(uint16_t*)&ram[a+3];
        if(sig!='M' && sig!='Z') break;
        if(own && own != 8 && own != parent){
            /* owner is a PSP: does its parent field point at us? */
            uint16_t its_parent = *(uint16_t*)&ram[(uint32_t)own*16 + 0x16];
            if(its_parent == parent){
                trc("[dos] releasing resident child %04X of %04X\n", own, parent);
                ivt_unhook_range((uint16_t)(seg+1), (uint16_t)(seg+sz));
                *(uint16_t*)&ram[a+1] = 0;
            }
        }
        if(sig=='Z') break;
        seg = (uint16_t)(seg + sz + 1);
    }
    mcb_coalesce();
}

static void mcb_dump(const char *why){
    uint16_t seg = MEM_FIRST;
    int n = 0;
    if(!trace_level) return;
    trc("[mcb] chain (%s):", why);
    for(;;){
        uint32_t a = (uint32_t)seg*16;
        uint8_t sig = ram[a];
        uint16_t own = *(uint16_t*)&ram[a+1];
        uint16_t sz  = *(uint16_t*)&ram[a+3];
        if(sig!='M' && sig!='Z'){ trc(" BAD@%04X", seg); break; }
        trc(" [%c %04X..%04X own=%04X %uK]", sig, (unsigned)(seg+1),
            (unsigned)(seg+sz), own, (unsigned)(sz/64));
        if(sig=='Z') break;
        seg = (uint16_t)(seg + sz + 1);
        if(++n > 40) break;
    }
    trc("\n");
}

/* -------------------------------------------------------------- handles */
typedef struct { FILE *f; int used; char name[260]; } DFile;
static DFile fh[64];
static uint16_t cur_psp;
static uint16_t dta_seg, dta_off;
static uint8_t  cur_drive = 2;             /* C: */
static uint16_t last_retcode = 0;
static int oa_active = 0;             /* INT 21h AH=0Ah line in progress */

/* --------------------------------------------------------- path mapping */
/* ------------------------------------------------------- write overlay
 *
 * The game writes to its own data files: the intro seeks near the end of
 * Intro.Mod and rewrites two bytes there, and the tables create .hi files.
 * Passing those writes through to the real directory silently corrupts the
 * installed game - INTRO.MOD stops matching the CRC recorded in the archive,
 * which invalidates every integrity check made afterwards.
 *
 * So every write goes to a copy instead.  A file opened for writing is copied
 * into PFEMU-STATE/ on first use and the handle is opened there; a file that
 * already has a copy is read from the copy.  Originals are never touched, and
 * the game's own state (PINBALL.CFG, the high-score tables) still persists
 * across runs.  Sol's runtime does the same thing, and it is the right call.
 */
static char writedir[600];

static int file_exists(const char *p){
    FILE *f = fopen(p, "rb");
    if(f){ fclose(f); return 1; }
    return 0;
}
static void overlay_path(const char *in, char *out, size_t n){
    const char *base = in, *p;
    for(p = in; *p; p++) if(*p=='/' || *p=='\\' || *p==':') base = p+1;
    snprintf(out, n, "%s/%s", writedir, base);
}
static int copy_to_overlay(const char *src, const char *dst){
    FILE *a = fopen(src, "rb"), *b;
    static char buf[32768];
    size_t got;
    if(!a) return 0;
    b = fopen(dst, "wb");
    if(!b){ fclose(a); return 0; }
    while((got = fread(buf, 1, sizeof(buf), a)) > 0) fwrite(buf, 1, got, b);
    fclose(a); fclose(b);
    return 1;
}

static void dos_path(const char *in, char *out, size_t n){
    const char *p = in;
    char tmp[512]; size_t i = 0;
    if(p[0] && p[1]==':') p += 2;
    while(*p && i < sizeof(tmp)-1){
        char c = *p++;
        tmp[i++] = (c=='\\') ? '/' : c;
    }
    tmp[i] = 0;
    if(tmp[0]=='/')
        snprintf(out, n, "%s%s", gamedir, tmp);
    else
        snprintf(out, n, "%s/%s", gamedir, tmp);
}
static void read_dosstr(uint32_t a, char *out, int n){
    int i;
    for(i=0;i<n-1;i++){ uint8_t c = mem_r8(a+i); if(!c) break; out[i]=(char)c; }
    out[i]=0;
}

/* ----------------------------------------------------------- EXEC / load */
typedef struct {
    uint16_t psp, parent_psp, parent_ss, parent_sp, env;
    /* The caller's registers across EXEC.  Real DOS hands the parent its own
     * register set back when the child terminates, and programs rely on it:
     * TABLE1.PRG EXECs the .SDR driver and then, without reloading DS, does
     * INT 66h AL=12h with DS:DX pointing at its module-name list.  Leave DS
     * as the child left it and the driver reads the name out of the driver's
     * own segment. */
    uint16_t r_ax, r_bx, r_cx, r_dx, r_si, r_di, r_bp, r_ds, r_es;
} Proc;
static Proc procs[8];
static int nproc = 0;

static void make_psp(uint16_t seg, uint16_t memtop, uint16_t parent, uint16_t env,
                     const char *tail){
    uint32_t a = (uint32_t)seg*16;
    int i;
    memset(&ram[a], 0, 256);
    ram[a+0]=0xCD; ram[a+1]=0x20;
    *(uint16_t*)&ram[a+2] = memtop;
    ram[a+5]=0xCD; ram[a+6]=0x21; ram[a+7]=0xCB;
    *(uint32_t*)&ram[a+0x0A] = mem_r32(0x22*4);
    *(uint32_t*)&ram[a+0x0E] = mem_r32(0x23*4);
    *(uint32_t*)&ram[a+0x12] = mem_r32(0x24*4);
    *(uint16_t*)&ram[a+0x16] = parent;
    for(i=0;i<20;i++) ram[a+0x18+i] = (i<5)?(uint8_t)i:0xFF;
    *(uint16_t*)&ram[a+0x2C] = env;
    *(uint16_t*)&ram[a+0x32] = 20;
    *(uint32_t*)&ram[a+0x34] = ((uint32_t)seg<<16) | 0x18;
    ram[a+0x50]=0xCD; ram[a+0x51]=0x21; ram[a+0x52]=0xCB;
    memset(&ram[a+0x5C], 0, 36);
    ram[a+0x5C]=0; ram[a+0x6C]=0;
    {
        int n = tail ? (int)strlen(tail) : 0;
        if(n>126) n=126;
        ram[a+0x80] = (uint8_t)n;
        if(n) memcpy(&ram[a+0x81], tail, (size_t)n);
        ram[a+0x81+n] = 0x0D;
    }
}

static uint16_t make_env(const char *progpath){
    static const char *vars[] = { "PATH=C:\\", "COMSPEC=C:\\COMMAND.COM", "PROMPT=$p$g", NULL };
    char buf[512]; int i, n = 0;
    uint16_t paras, seg, largest;
    for(i=0;vars[i];i++){ int l=(int)strlen(vars[i]); memcpy(buf+n, vars[i], (size_t)l+1); n += l+1; }
    buf[n++] = 0;
    *(uint16_t*)&buf[n] = 1; n += 2;
    { int l=(int)strlen(progpath); memcpy(buf+n, progpath, (size_t)l+1); n += l+1; }
    paras = (uint16_t)((n + 15)/16);
    seg = mcb_alloc(paras, 8, &largest);
    if(!seg) return 0;
    memcpy(&ram[(uint32_t)seg*16], buf, (size_t)n);
    return seg;
}

/* Load an MZ image.  Returns 0 on success. */
static int load_mz(const char *host, uint16_t *out_cs, uint16_t *out_ip,
                   uint16_t *out_ss, uint16_t *out_sp, uint16_t *out_psp,
                   uint16_t env, const char *tail, const char *dospath){
    FILE *f = fopen(host, "rb");
    uint8_t hdr[32];
    uint32_t hdrsize, imgend, imglen;
    uint16_t paras, seg, psp, load, largest;
    long fsize;
    if(!f) return 2;
    fseek(f,0,SEEK_END); fsize = ftell(f); fseek(f,0,SEEK_SET);
    if(fread(hdr,1,32,f)!=32){ fclose(f); return 2; }
    if(hdr[0]!='M'||hdr[1]!='Z'){
        /* a .COM: no header at all - give it every free paragraph, load the
         * whole file at PSP:0100 and run it with all four segments = PSP */
        uint16_t lg;
        fseek(f,0,SEEK_SET);
        seg = mcb_alloc(0xFFFF, 0, &lg);
        if(!seg) seg = mcb_alloc(lg, 0, &lg);
        if(!seg){ fclose(f); return 8; }
        paras = *(uint16_t*)&ram[(uint32_t)(seg-1)*16 + 3];
        psp = seg;
        *(uint16_t*)&ram[(uint32_t)(seg-1)*16 + 1] = psp;
        make_psp(psp, (uint16_t)(psp + paras), cur_psp ? cur_psp : psp, env, tail);
        if(fsize > 0xFF00) fsize = 0xFF00;
        if(fread(&ram[(uint32_t)psp*16 + 0x100], 1, (size_t)fsize, f) != (size_t)fsize){ }
        fclose(f);
        *out_cs = psp; *out_ip = 0x0100;
        *out_ss = psp; *out_sp = 0xFFFE;
        mem_w16((uint32_t)psp*16 + 0xFFFE, 0x0000);   /* the INT 20h return word */
        *out_psp = psp;
        return 0;
    }
    {
        uint16_t cblp = *(uint16_t*)&hdr[2], cp = *(uint16_t*)&hdr[4];
        uint16_t crlc = *(uint16_t*)&hdr[6], cparhdr = *(uint16_t*)&hdr[8];
        uint16_t minalloc = *(uint16_t*)&hdr[10], maxalloc = *(uint16_t*)&hdr[12];
        uint16_t ss = *(uint16_t*)&hdr[14], sp = *(uint16_t*)&hdr[16];
        uint16_t ip = *(uint16_t*)&hdr[20], cs = *(uint16_t*)&hdr[22];
        uint16_t lfarlc = *(uint16_t*)&hdr[24];
        uint32_t need;
        int i;
        hdrsize = (uint32_t)cparhdr*16;
        imgend = cblp ? ((uint32_t)(cp-1)*512 + cblp) : ((uint32_t)cp*512);
        if(imgend > (uint32_t)fsize) imgend = (uint32_t)fsize;
        imglen = imgend - hdrsize;
        need = (imglen + 15)/16 + 16 + minalloc;
        if(maxalloc > minalloc){
            uint32_t want = (imglen + 15)/16 + 16 + maxalloc;
            if(want > 0xFFFF) want = 0xFFFF;
            need = want;
        }
        paras = (uint16_t)(need > 0xFFFF ? 0xFFFF : need);
        seg = mcb_alloc(paras, 0, &largest);
        if(!seg){
            if(largest < (imglen+15)/16 + 16 + minalloc){ fclose(f); return 8; }
            paras = largest;
            seg = mcb_alloc(paras, 0, &largest);
            if(!seg){ fclose(f); return 8; }
        }
        psp = seg;
        load = (uint16_t)(seg + 16);
        *(uint16_t*)&ram[(uint32_t)(seg-1)*16 + 1] = psp;
        make_psp(psp, (uint16_t)(psp + paras), cur_psp ? cur_psp : psp, env, tail);

        fseek(f, (long)hdrsize, SEEK_SET);
        if(fread(&ram[(uint32_t)load*16], 1, imglen, f) != imglen){ }

        fseek(f, lfarlc, SEEK_SET);
        for(i=0;i<crlc;i++){
            uint8_t rb[4]; uint16_t ro, rs; uint32_t a;
            if(fread(rb,1,4,f)!=4) break;
            ro = (uint16_t)(rb[0]|(rb[1]<<8)); rs = (uint16_t)(rb[2]|(rb[3]<<8));
            a = (uint32_t)(load + rs)*16 + ro;
            *(uint16_t*)&ram[a] = (uint16_t)(*(uint16_t*)&ram[a] + load);
        }
        fclose(f);

        /* Game-specific image patches (memory-only, signature-checked,
         * -nopatch disables).  Implemented per game in src/fantasies.c and
         * src/dreams.c; dos.c only dispatches.
         *
         * Fantasies' manual-lookup check is no longer handled here: the old
         * CRACK.COM-style JNC->JMP edit only forced acceptance of whatever
         * was typed at the screen, so the screen still appeared once per
         * fresh INTRO.MOD and still rewrote it on "success".  Replaced by
         * fantasies_filter_read() (called from the AH=3Fh handler below),
         * which makes INTRO.PRG believe the check already passed before it
         * ever draws the screen - see WRITEUP-PHASE2.md §5.13.1. */
        dreams_patch_image((uint32_t)load*16, imglen);
        fantasies_patch_intro(dospath, (uint32_t)load*16, imglen);
        fantasies_patch_pause(dospath, (uint32_t)load*16, imglen);
        fantasies_patch_spring(dospath, (uint32_t)load*16, imglen);
        /* fantasies_patch_sdr() (SDR PLL seed preset + pass-1 skip) is
         * disabled: it computes a "poke" address from a segment immediate
         * baked into the .SDR file (confirmed unaffected by the relocation
         * loop above - traced at 0x01E2 for SBLASTER.SDR both before and
         * after moving this call past it), not from anything tied to where
         * THIS EXEC's driver or caller actually landed in memory.  That
         * fixed address happened to fall inside whichever process was
         * resident there at the time - harmless-looking cosmetic corruption
         * when it landed in the intro's own memory, an instant crash when a
         * table's own .SDR load put it inside the table's live code/data.
         * Confirmed by the user: both the doc-check screen corruption (with
         * a sound driver loaded) and the Table 4 crash disappear under
         * -nopatch, which is what disables this. Re-enabling it needs the
         * poke address validated against the actual owning process's MCB
         * block before writing, not just pattern-matched from the file. */
        *out_cs = (uint16_t)(load + cs);
        *out_ip = ip;
        *out_ss = (uint16_t)(load + ss);
        *out_sp = sp;
        *out_psp = psp;
        (void)dospath;
        return 0;
    }
}

int dos_exec(const char *dospath, uint16_t parblk_seg, uint32_t parblk_off,
             uint32_t f1, uint32_t f2){
    char host[512], tail[130];
    uint16_t cs,ip,ss,sp,psp,env;
    uint32_t pb = (uint32_t)parblk_seg*16 + parblk_off;
    uint16_t tail_seg, tail_off;
    int r, n;
    (void)f1; (void)f2;
    dos_path(dospath, host, sizeof(host));

    env = mem_r16(pb+0);
    tail_off = mem_r16(pb+2); tail_seg = mem_r16(pb+4);
    n = mem_r8((uint32_t)tail_seg*16 + tail_off);
    if(n>126) n=126;
    { int i; for(i=0;i<n;i++) tail[i] = (char)mem_r8((uint32_t)tail_seg*16 + tail_off + 1 + i); tail[n]=0; }
    if(!env) env = make_env(dospath);

    r = load_mz(host, &cs,&ip,&ss,&sp,&psp, env, tail, dospath);
    if(r) return r;

    fantasies_on_exec(dospath, cs);

    if(nproc < 8){
        procs[nproc].psp = psp;
        procs[nproc].parent_psp = cur_psp;
        procs[nproc].parent_ss = cpu.sreg[S_SS];
        procs[nproc].parent_sp = REG16(R_ESP);
        procs[nproc].env = env;
        procs[nproc].r_ax = AX; procs[nproc].r_bx = BX;
        procs[nproc].r_cx = CX; procs[nproc].r_dx = DX;
        procs[nproc].r_si = SI; procs[nproc].r_di = DI;
        procs[nproc].r_bp = REG16(R_EBP);
        procs[nproc].r_ds = cpu.sreg[S_DS]; procs[nproc].r_es = cpu.sreg[S_ES];
        nproc++;
    }
    cur_psp = psp;
    oa_active = 0;          /* fresh input state for the new program */
    dos_snapshot_ivt();       /* so a resident child's hooks can be undone */
    /* DS/ES point at the PSP, SS:SP and CS:IP from the header */
    set_sreg(S_DS, psp); set_sreg(S_ES, psp);
    set_sreg(S_SS, ss);  REG16(R_ESP) = sp;
    set_sreg(S_CS, cs);  cpu.eip = ip;
    AX = 0; BX = 0; CX = 0; DX = 0; SI = ip; DI = sp;
    REG16(R_EBP) = 0x091C;
    mcb_dump("after exec");
    trc("[dos] exec %s -> cs=%04X ip=%04X ss=%04X sp=%04X psp=%04X tail=\"%s\"\n",
        dospath, cs, ip, ss, sp, psp, tail);
    return 0;
}

/* keep_paras < 0 => normal exit (free everything); >= 0 => TSR */
int dos_trap_exit = 0;     /* -trapexit : stop the emulator on a child's exit */

static void dos_terminate2(uint16_t code, int keep_paras){
    last_retcode = code;
    if(dos_trap_exit && keep_paras < 0 && nproc > 1){
        uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
        printf("[dos] child psp %04X terminating, code %u, called from %04X:%04X\n",
               cur_psp, code, mem_r16(sp+2), mem_r16(sp));
        cpu.shutdown = 1;
        return;
    }
    if(nproc == 0){ dos_done = 1; cpu.shutdown = 1; cpu_no_iret(); return; }
    nproc--;
    {
        Proc *p = &procs[nproc];
        uint32_t a = (uint32_t)p->psp*16;
        /* restore the vectors the PSP saved */
        if(keep_paras < 0){
            *(uint32_t*)&ram[0x22*4] = *(uint32_t*)&ram[a+0x0A];
            *(uint32_t*)&ram[0x23*4] = *(uint32_t*)&ram[a+0x0E];
            *(uint32_t*)&ram[0x24*4] = *(uint32_t*)&ram[a+0x12];
            if(p->env) mcb_free(p->env);
            mcb_free(p->psp);
            mcb_free_children(p->psp);
        } else {
            uint16_t avail;
            if(keep_paras < 17) keep_paras = 17;
            mcb_resize(p->psp, (uint16_t)keep_paras, &avail);
            if(p->env) mcb_free(p->env);      /* DOS releases the environment */
            trc("[dos] TSR: psp %04X stays resident, %d paragraphs\n", p->psp, keep_paras);
        }
        cur_psp = p->parent_psp;
        set_sreg(S_SS, p->parent_ss);
        REG16(R_ESP) = p->parent_sp;
        /* hand the parent its registers back (see Proc) */
        BX = p->r_bx; CX = p->r_cx; DX = p->r_dx;
        SI = p->r_si; DI = p->r_di; REG16(R_EBP) = p->r_bp;
        set_sreg(S_DS, p->r_ds); set_sreg(S_ES, p->r_es);
        /* the parent's pending IRET frame is now on top of its stack */
        {   uint32_t sp32 = cpu.sbase[S_SS] + REG16(R_ESP);
            uint16_t fl = mem_r16(sp32+4);
            mem_w16(sp32+4, (uint16_t)(fl & ~1u));       /* CF = 0: EXEC succeeded */
        }
        AX = 0;
    }
    /* the root program (the launcher) has exited - there is nothing left to
     * return to, so close the machine down instead of idling on the HLT in the
     * synthetic return frame */
    if(nproc == 0 && keep_paras < 0){ dos_done = 1; cpu.shutdown = 1; }
}
static void dos_terminate(uint16_t code){ dos_terminate2(code, -1); }

/* ------------------------------------------------------------- INT 21h  */
static int alloc_handle(void){
    int i; for(i=5;i<64;i++) if(!fh[i].used) return i; return -1;
}

/* ------------------------------------------------------- FindFirst/Next */
static HANDLE find_h = INVALID_HANDLE_VALUE;
static void find_state_reset(void){
    if(find_h != INVALID_HANDLE_VALUE){ FindClose(find_h); find_h = INVALID_HANDLE_VALUE; }
}
static void fill_dta(const WIN32_FIND_DATAA *fd){
    uint32_t d = (uint32_t)dta_seg*16 + dta_off;
    int i;
    char nm[16];
    SYSTEMTIME st; FILETIME lf;
    uint16_t dt = 0, tm = 0;
    uint8_t attr = 0;
    if(fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) attr |= 0x10;
    if(fd->dwFileAttributes & FILE_ATTRIBUTE_READONLY)  attr |= 0x01;
    if(fd->dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)    attr |= 0x02;
    if(fd->dwFileAttributes & FILE_ATTRIBUTE_SYSTEM)    attr |= 0x04;
    if(FileTimeToLocalFileTime(&fd->ftLastWriteTime,&lf) && FileTimeToSystemTime(&lf,&st)){
        dt = (uint16_t)(((st.wYear-1980)<<9) | (st.wMonth<<5) | st.wDay);
        tm = (uint16_t)((st.wHour<<11) | (st.wMinute<<5) | (st.wSecond/2));
    }
    /* DOS reports a plain 8.3 name in upper case */
    for(i=0;i<13 && fd->cFileName[i];i++){
        char c = fd->cFileName[i];
        nm[i] = (char)((c>='a'&&c<='z') ? c-32 : c);
    }
    nm[i<13?i:12] = 0;
    mem_w8(d+0x15, attr);
    mem_w16(d+0x16, tm);
    mem_w16(d+0x18, dt);
    mem_w16(d+0x1A, (uint16_t)fd->nFileSizeLow);
    mem_w16(d+0x1C, (uint16_t)(fd->nFileSizeLow>>16));
    for(i=0;i<13;i++) mem_w8(d+0x1E+i, (uint8_t)(i<(int)strlen(nm)?nm[i]:0));
}

int dos_log_all = 0;
void dos_int21(void){
    /* the "from" that matters is the guest that issued the INT, which is the
     * return address our stub is sitting on, not the stub itself */
    if(dos_log_all){
        uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
        trc("[21] AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X from %04X:%04X\n",
            AX,BX,CX,DX,cpu.sreg[S_DS],cpu.sreg[S_ES],
            mem_r16(sp+2), mem_r16(sp));
    }
    switch(AH){
    case 0x00: dos_terminate(0); return;
    case 0x01: case 0x07: case 0x08: AL = 0; break;
    case 0x02: { extern void bios_tty(uint8_t c); bios_tty(DL); break; }
    case 0x06:
        if(DL != 0xFF){ extern void bios_tty(uint8_t c); bios_tty(DL); }
        else { AL = 0; bios_set_cf(0); cpu.zf = 1; }
        break;
    case 0x09: {
        uint32_t a = cpu.sbase[S_DS] + DX;
        extern void bios_tty(uint8_t c);
        int i;
        for(i=0;i<2000;i++){ uint8_t c = mem_r8(a+i); if(c=='$') break; bios_tty(c); }
        AL = '$';
        break; }
    case 0x0A: {
        /* Buffered line input.  DS:DX: [0]=max chars, [1]=count (out),
         * chars at [2..], CR-terminated (stored, not counted).  Keystrokes
         * come from the BIOS type-ahead queue with DOS echo; Backspace
         * erases; extended keys store as 0x00 + scancode.  Empty queue
         * blocks exactly like INT 16h AH=00 (rewind, idle, resume on IRQ);
         * partial input already sits in the guest buffer, and oa_active
         * tells re-entry to resume it instead of restarting the line. */
        uint32_t a = cpu.sbase[S_DS] + DX;
        uint8_t maxlen = mem_r8(a);
        uint8_t n;
        if(!oa_active) mem_w8(a+1, 0);
        n = mem_r8(a+1);
        if(n > maxlen) n = maxlen;
        for(;;){
            uint16_t k;
            if(!bios_kbuf_peek(&k)){
                uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
                oa_active = 1;
                cpu.iflag = (mem_r16(sp+4) >> 9) & 1;
                cpu.eip -= 3; cpu_no_iret(); cpu.halted = 1;
                return;
            }
            bios_kbuf_get(&k);
            { uint8_t asc = (uint8_t)(k & 0xFF);
              uint8_t sc = (uint8_t)(k >> 8);
              if(asc == 0x0D){
                  mem_w8(a+2+n, 0x0D);
                  bios_tty(0x0D); bios_tty(0x0A);
                  break;
              } else if(asc == 0x08){
                  if(n){ n--; bios_tty(0x08); bios_tty(' '); bios_tty(0x08); }
              } else if(asc == 0){
                  if(n+2 <= maxlen){ mem_w8(a+2+n,0); mem_w8(a+2+n+1,sc); n+=2; }
              } else if(asc >= 0x20){
                  if(n < maxlen){ mem_w8(a+2+n,asc); n++; bios_tty(asc); }
              } }
            mem_w8(a+1, n);
        }
        mem_w8(a+1, n);
        oa_active = 0;
        break; }
    case 0x0B: AL = 0; break;
    case 0x0C: AL = 0; break;
    case 0x0D: break;
    case 0x0E: cur_drive = DL; AL = 26; break;
    case 0x19: AL = cur_drive; break;
    case 0x1A: dta_seg = cpu.sreg[S_DS]; dta_off = DX; break;
    case 0x25: *(uint32_t*)&ram[(uint32_t)AL*4] = ((uint32_t)cpu.sreg[S_DS]<<16) | DX; break;
    case 0x29: {
        /* Parse filename into FCB.  DS:SI source text, ES:DI 12-byte FCB
         * (drive + 8 + 3 used), AL control flags: bit 0 skip leading
         * separators, bits 1-3 fill drive/name/ext with defaults when that
         * part is absent.  Returns AL = 0 plain, 1 wildcards (?/* seen),
         * FF invalid drive; SI advances to the terminating character.
         * BAT2EXEC (the DREAMS.COM launcher) parses every program name
         * through here (AX=2903) before EXEC, so the old "unimplemented"
         * return (AX=1, CF=1) broke it. */
        uint32_t s = cpu.sbase[S_DS] + SI;
        uint32_t f = cpu.sbase[S_ES] + DI;
        int wild = 0, i, had;
        uint8_t c;
        if(AL & 1){
            for(;;){ c = mem_r8(s);
                if(c<=0x20||c=='"'||c=='+'||c==','||c==';'||c=='='||c=='['||
                   c==']'||c==':'||c=='.'||c=='/'||c=='\\'||c=='<'||c=='>'||c=='|') s++;
                else break; }
        }
        c = mem_r8(s);
        if(((c>='A'&&c<='Z')||(c>='a'&&c<='z')) && mem_r8(s+1)==':'){
            if(c>='a'&&c<='z') c -= 32;
            if(c-'A'+1 > 26){ AL = 0xFF; break; }
            mem_w8(f, (uint8_t)(c-'A'+1)); s += 2;
        } else if(AL & 2) mem_w8(f, 0);
        had = 0;
        for(i=0;i<8;i++){
            c = mem_r8(s);
            if(c=='*'){ wild=1; had=1;
                for(;i<8;i++) mem_w8(f+1+i,'?');
                s++; break; }
            if(c=='?'){ wild=1; had=1; mem_w8(f+1+i,'?'); s++; continue; }
            if(c=='.'||c=='"'||c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||
               c==':'||c=='/'||c=='\\'||c=='<'||c=='>'||c=='|'||c<=0x20) break;
            had=1;
            if(c>='a'&&c<='z') c -= 32;
            mem_w8(f+1+i,c); s++;
        }
        for(;;){ c = mem_r8(s);   /* skip name overflow */
            if(c=='.'||c=='"'||c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||
               c==':'||c=='/'||c=='\\'||c=='<'||c=='>'||c=='|'||c<=0x20) break;
            if(c=='*'||c=='?') wild=1;
            s++; }
        if(!had && (AL&4)){ for(i=0;i<8;i++) mem_w8(f+1+i,' '); }
        if(mem_r8(s)=='.'){
            s++; had = 0;
            for(i=0;i<3;i++){
                c = mem_r8(s);
                if(c=='*'){ wild=1; had=1;
                    for(;i<3;i++) mem_w8(f+9+i,'?');
                    s++; break; }
                if(c=='?'){ wild=1; had=1; mem_w8(f+9+i,'?'); s++; continue; }
                if(c=='"'||c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||
                   c==':'||c=='.'||c=='/'||c=='\\'||c=='<'||c=='>'||c=='|'||c<=0x20) break;
                had=1;
                if(c>='a'&&c<='z') c -= 32;
                mem_w8(f+9+i,c); s++;
            }
            for(;;){ c = mem_r8(s);   /* skip ext overflow */
                if(c=='"'||c=='+'||c==','||c==';'||c=='='||c=='['||c==']'||
                   c==':'||c=='.'||c=='/'||c=='\\'||c=='<'||c=='>'||c=='|'||c<=0x20) break;
                if(c=='*'||c=='?') wild=1;
                s++; }
            if(!had && (AL&8)){ for(i=0;i<3;i++) mem_w8(f+9+i,' '); }
        } else if(AL & 8){ for(i=0;i<3;i++) mem_w8(f+9+i,' '); }
        SI = (uint16_t)(s - cpu.sbase[S_DS]);
        AL = wild ? 1 : 0;
        break; }
    case 0x2A: { time_t t = time(NULL); struct tm *lt = localtime(&t);
        CX = (uint16_t)(lt->tm_year+1900); DH=(uint8_t)(lt->tm_mon+1); DL=(uint8_t)lt->tm_mday;
        AL=(uint8_t)lt->tm_wday; break; }
    case 0x2C: { time_t t = time(NULL); struct tm *lt = localtime(&t);
        CH=(uint8_t)lt->tm_hour; CL=(uint8_t)lt->tm_min; DH=(uint8_t)lt->tm_sec;
        DL=(uint8_t)((int)(emu_time*100)%100); break; }
    case 0x2F: set_sreg(S_ES, dta_seg); BX = dta_off; break;
    case 0x30: AL = 5; AH = 0; BH = 0xFF; BL = 0; CX = 0; break;
    case 0x33: if(AL==0) DL=0; break;
    case 0x35: { uint32_t v = *(uint32_t*)&ram[(uint32_t)AL*4];
        set_sreg(S_ES, (uint16_t)(v>>16)); BX = (uint16_t)v; break; }
    case 0x36: AX = 4; BX = 20000; CX = 512; DX = 30000; break;
    case 0x38: bios_set_cf(0); break;
    case 0x3B: bios_set_cf(0); break;
    case 0x3C: case 0x3D: {
        char name[260], host[512];
        int h;
        read_dosstr(cpu.sbase[S_DS] + DX, name, sizeof(name));
        /* Fantasies only, AH=3Dh (open existing) only: pretend PINBALL.CFG
         * isn't there and poke the launcher's chosen options straight into
         * INTRO.PRG's own buffer instead.  See fantasies.c - this keeps
         * boot-time instruction timing identical to a fresh install (the
         * one case already proven not to wedge the sound driver's PLL
         * calibration), regardless of what's actually on disk. */
        if(AH==0x3D && fantasies_intercept_cfg_open(name)){
            AX = 2; bios_set_cf(1);
            trc("[dos] open '%s' intercepted (Fantasies options poke)\n", name);
            break;
        }
        h = alloc_handle();
        dos_path(name, host, sizeof(host));
        if(h<0){ AX = 4; bios_set_cf(1); break; }
        {
            char ov[600];
            int writing = (AH==0x3C) || ((AL & 3) != 0);
            overlay_path(name, ov, sizeof(ov));
            if(file_exists(ov)) snprintf(host, sizeof(host), "%s", ov);
            else if(writing){
                _mkdir(writedir);
                if(AH != 0x3C) copy_to_overlay(host, ov);   /* 3Ch truncates anyway */
                snprintf(host, sizeof(host), "%s", ov);
            }
        }
        if(AH==0x3C) fh[h].f = fopen(host, "w+b");
        else {
            int mode = AL & 3;
            fh[h].f = fopen(host, mode ? "r+b" : "rb");
            if(!fh[h].f && mode) fh[h].f = fopen(host, "rb");
        }
        if(!fh[h].f){
            trc("[dos] open FAILED '%s' (%s)\n", name, host);
            AX = 2; bios_set_cf(1); break;
        }
        fh[h].used = 1; snprintf(fh[h].name,sizeof(fh[h].name),"%s",name);
        AX = (uint16_t)h; bios_set_cf(0);
        trc("[dos] open '%s' -> handle %d\n", name, h);
        break; }
    case 0x3E: {
        int h = BX;
        if(h>=5 && h<64 && fh[h].used){ fclose(fh[h].f); fh[h].used=0; }
        bios_set_cf(0); break; }
    case 0x3F: {
        int h = BX; uint32_t a = cpu.sbase[S_DS] + DX; uint16_t n = CX;
        if(h<5){ AX = 0; bios_set_cf(0); break; }
        if(h>=64 || !fh[h].used){ AX=6; bios_set_cf(1); break; }
        {
            static uint8_t buf[65536];
            long pos = ftell(fh[h].f);
            size_t got = fread(buf, 1, n, fh[h].f);
            uint32_t i;
            fantasies_filter_read(fh[h].name, pos, buf, (int)got);
            for(i=0;i<got;i++) mem_w8(a+i, buf[i]);
            AX = (uint16_t)got; bios_set_cf(0);
        }
        break; }
    case 0x40: {
        int h = BX; uint32_t a = cpu.sbase[S_DS] + DX; uint16_t n = CX;
        if(h<5){
            extern void bios_tty(uint8_t c);
            uint16_t i; for(i=0;i<n;i++) bios_tty(mem_r8(a+i));
            AX = n; bios_set_cf(0); break; }
        if(h>=64 || !fh[h].used){ AX=6; bios_set_cf(1); break; }
        {
            static uint8_t buf[65536];
            uint16_t i;
            for(i=0;i<n;i++) buf[i] = mem_r8(a+i);
            AX = (uint16_t)fwrite(buf,1,n,fh[h].f); bios_set_cf(0);
        }
        break; }
    case 0x41: { char name[260], host[512];
        read_dosstr(cpu.sbase[S_DS] + DX, name, sizeof(name));
        dos_path(name, host, sizeof(host));
        bios_set_cf(remove(host) ? 1 : 0); if(cpu.cf) AX = 2;
        break; }
    case 0x42: {
        int h = BX; long off = (long)(((uint32_t)CX<<16) | DX);
        if(h<5 || h>=64 || !fh[h].used){ AX=6; bios_set_cf(1); break; }
        fseek(fh[h].f, off, AL==0?SEEK_SET:(AL==1?SEEK_CUR:SEEK_END));
        { long p = ftell(fh[h].f); AX=(uint16_t)p; DX=(uint16_t)(p>>16); }
        bios_set_cf(0); break; }
    case 0x43: CX = 0x20; bios_set_cf(0); break;
    case 0x44:
        if(AL==0){ DX = (BX<5) ? 0x80D3 : 0x0000; bios_set_cf(0); }
        else bios_set_cf(0);
        break;
    case 0x45: { int h = alloc_handle(); AX = (uint16_t)(h<0?4:h); bios_set_cf(h<0); break; }
    case 0x47: { uint32_t a = cpu.sbase[S_DS] + SI; mem_w8(a,0); bios_set_cf(0); break; }
    case 0x48: { uint16_t largest, s = mcb_alloc(BX, cur_psp, &largest);
        trc("[dos] alloc %u paras -> %04X\n", (unsigned)BX, s);
        if(s){ AX = s; bios_set_cf(0); } else { AX = 8; BX = largest; bios_set_cf(1); }
        break; }
    case 0x49: trc("[dos] free block %04X\n", cpu.sreg[S_ES]);
        bios_set_cf(mcb_free(cpu.sreg[S_ES]) ? 0 : 1); if(cpu.cf) AX = 9; break;
    case 0x4A: { uint16_t avail;
        trc("[dos] resize %04X to %u paras\n", cpu.sreg[S_ES], (unsigned)BX);
        if(mcb_resize(cpu.sreg[S_ES], BX, &avail)) bios_set_cf(0);
        else { AX = 8; BX = avail; bios_set_cf(1); }
        break; }
    case 0x4B: {
        char name[260];
        int r;
        read_dosstr(cpu.sbase[S_DS] + DX, name, sizeof(name));
        if(AL != 0){ AX = 1; bios_set_cf(1); break; }
        r = dos_exec(name, cpu.sreg[S_ES], BX, 0, 0);
        if(r){ AX = (uint16_t)r; bios_set_cf(1); trc("[dos] exec '%s' failed (%d)\n", name, r); break; }
        cpu_no_iret();
        return; }
    case 0x31: dos_terminate2(AL, (int)DX); return;
    case 0x4C: dos_terminate(AL); return;
    case 0x4D: AX = last_retcode; break;
    case 0x4E: {
        char name[260], host[512];
        WIN32_FIND_DATAA fd;
        read_dosstr(cpu.sbase[S_DS] + DX, name, sizeof(name));
        dos_path(name, host, sizeof(host));
        find_state_reset();
        find_h = FindFirstFileA(host, &fd);
        while(find_h != INVALID_HANDLE_VALUE && (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)){
            if(!FindNextFileA(find_h,&fd)){ FindClose(find_h); find_h = INVALID_HANDLE_VALUE; }
        }
        if(find_h == INVALID_HANDLE_VALUE){ AX = 18; bios_set_cf(1); }
        else { fill_dta(&fd); AX = 0; bios_set_cf(0); }
        trc("[dos] findfirst '%s' -> %s\n", name, cpu.cf ? "none" : (char*)fd.cFileName);
        break; }
    case 0x4F: {
        WIN32_FIND_DATAA fd;
        int ok = 0;
        while(find_h != INVALID_HANDLE_VALUE && FindNextFileA(find_h,&fd)){
            if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            ok = 1; break;
        }
        if(ok){ fill_dta(&fd); AX = 0; bios_set_cf(0); trc("[dos] findnext -> %s\n", fd.cFileName); }
        else { find_state_reset(); AX = 18; bios_set_cf(1); }
        break; }
    case 0x50: cur_psp = BX; break;
    case 0x51: case 0x62: BX = cur_psp; break;
    case 0x54: AL = 0; break;
    case 0x58:
        switch(AL){
        case 0x00: AX = dos_alloc_strategy; bios_set_cf(0); break;
        case 0x01: dos_alloc_strategy = BX; AX = 0; bios_set_cf(0);
                   trc("[dos] allocation strategy := %u\n", (unsigned)BX); break;
        case 0x02: AX = 0; bios_set_cf(0); break;      /* UMB link state: off */
        case 0x03: AX = 0; bios_set_cf(0); break;
        default:   AX = 1; bios_set_cf(1); break;
        }
        break;
    case 0x59: AX = 0; break;
    case 0x5D: bios_set_cf(0); break;
    default:
        trc("[dos] unimplemented INT 21h AH=%02X AL=%02X\n", AH, AL);
        bios_set_cf(1); AX = 1;
        break;
    }
}

static void dos_int20(void){ dos_terminate(0); }
static void dos_int27(void){ dos_terminate(0); }
static void dos_int2f(void){ }
static void dos_int28(void){ }

void dos_init(const char *hostdir){
    int i;
    snprintf(gamedir, sizeof(gamedir), "%s", hostdir);
    snprintf(writedir, sizeof(writedir), "%s/PFEMU-STATE", gamedir);
    for(i=0;i<512;i++) if(gamedir[i]=='\\') gamedir[i]='/';
    { size_t l = strlen(gamedir); if(l && gamedir[l-1]=='/') gamedir[l-1]=0; }
    memset(fh,0,sizeof(fh));
    mcb_init();
    cur_psp = 0; nproc = 0;
    dta_seg = 0; dta_off = 0x80;
    cb_table[0x20] = dos_int20;
    cb_table[0x21] = dos_int21;
    cb_table[0x27] = dos_int27;
    cb_table[0x28] = dos_int28;
    cb_table[0x2F] = dos_int2f;
    find_state_reset();
}
