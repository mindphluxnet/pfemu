/* 386 real-mode CPU interpreter */
#include "pfemu.h"

CPU cpu;
uint8_t *ram;
uint32_t a20_mask = 0xFFFFFFFFu;

static uint8_t ptab[256];
static void init_ptab(void){
    int i,j,c;
    for(i=0;i<256;i++){ c=0; for(j=0;j<8;j++) if(i&(1<<j)) c++; ptab[i] = (c&1)?0:1; }
}

/* ------------------------------------------------------------------ flags */
uint32_t cpu_getflags(void){
    return 0x0002u | (cpu.cf) | (cpu.pf<<2) | (cpu.af<<4) | (cpu.zf<<6) |
           (cpu.sf<<7) | (cpu.tf<<8) | (cpu.iflag<<9) | (cpu.df<<10) |
           (cpu.of<<11) | (cpu.iopl<<12) | (cpu.nt<<14) | (cpu.ac<<18);
}
void cpu_setflags(uint32_t f){
    cpu.cf=f&1; cpu.pf=(f>>2)&1; cpu.af=(f>>4)&1; cpu.zf=(f>>6)&1;
    cpu.sf=(f>>7)&1; cpu.tf=(f>>8)&1; cpu.iflag=(f>>9)&1; cpu.df=(f>>10)&1;
    cpu.of=(f>>11)&1; cpu.iopl=(f>>12)&3; cpu.nt=(f>>14)&1; cpu.ac=(f>>18)&1;
}

/* ------------------------------------------------------------- decode ctx */
static int opsz, adsz, segovr, rep;
static uint32_t cs_base;
static int mod_, reg_, rm_;
static uint32_t ea, ea_off;
static int ea_isreg;
static int no_iret;

#define MASK(sz) ((sz)==32?0xFFFFFFFFu:((sz)==16?0xFFFFu:0xFFu))

/* Local memory helpers: byte-for-byte the semantics of mem_r8/mem_w8 in
 * vga.c (A20 + 16 MB wrap, VGA dispatch, ROM write-ignore), but inlined in
 * this translation unit.  The interpreter executes millions of instructions
 * per second and almost every one fetches bytes and touches memory through
 * here, so removing the per-access cross-TU call (and the repeated
 * mask/branch inside mem_r16/mem_r32) matters.  Behaviour is unchanged. */
#define CPU_VGA_LO 0xA0000u
#define CPU_VGA_HI 0xC0000u
static inline uint8_t cpu_ld8(uint32_t a){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a >= CPU_VGA_LO && a < CPU_VGA_HI) return vga_mem_r(a);
    return ram[a];
}
static inline uint16_t cpu_ld16(uint32_t a){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 1u >= CPU_VGA_LO && a < CPU_VGA_HI) return (uint16_t)(cpu_ld8(a) | ((uint16_t)cpu_ld8(a+1) << 8));
    return (uint16_t)(ram[a] | ((uint16_t)ram[a+1] << 8));
}
static inline uint32_t cpu_ld32(uint32_t a){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 3u >= CPU_VGA_LO && a < CPU_VGA_HI)
        return (uint32_t)cpu_ld8(a) | ((uint32_t)cpu_ld8(a+1) << 8) |
               ((uint32_t)cpu_ld8(a+2) << 16) | ((uint32_t)cpu_ld8(a+3) << 24);
    return (uint32_t)ram[a] | ((uint32_t)ram[a+1] << 8) |
           ((uint32_t)ram[a+2] << 16) | ((uint32_t)ram[a+3] << 24);
}
static inline void cpu_st8(uint32_t a, uint8_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a >= CPU_VGA_LO && a < CPU_VGA_HI){ vga_mem_w(a, v); return; }
    if(a >= 0xC0000 && a < 0x100000) return;   /* ROM */
    ram[a] = v;
}
static inline void cpu_st16(uint32_t a, uint16_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 1u >= CPU_VGA_LO && a < 0x100000){ cpu_st8(a, (uint8_t)v); cpu_st8(a+1, (uint8_t)(v >> 8)); return; }
    ram[a] = (uint8_t)v; ram[a+1] = (uint8_t)(v >> 8);
}
static inline void cpu_st32(uint32_t a, uint32_t v){
    a &= a20_mask; a &= (RAM_SIZE-1);
    if(a + 3u >= CPU_VGA_LO && a < 0x100000){
        cpu_st8(a, (uint8_t)v); cpu_st8(a+1, (uint8_t)(v >> 8));
        cpu_st8(a+2, (uint8_t)(v >> 16)); cpu_st8(a+3, (uint8_t)(v >> 24)); return; }
    ram[a] = (uint8_t)v; ram[a+1] = (uint8_t)(v >> 8);
    ram[a+2] = (uint8_t)(v >> 16); ram[a+3] = (uint8_t)(v >> 24);
}

static uint8_t fetch8(void){ uint8_t v = cpu_ld8(cs_base + cpu.eip); cpu.eip = (cpu.eip+1)&0xFFFF; return v; }
static uint16_t fetch16(void){ uint16_t v = cpu_ld16(cs_base + cpu.eip); cpu.eip=(cpu.eip+2)&0xFFFF; return v; }
static uint32_t fetch32(void){ uint32_t v = cpu_ld32(cs_base + cpu.eip); cpu.eip=(cpu.eip+4)&0xFFFF; return v; }

void set_sreg(int s, uint16_t v){ cpu.sreg[s]=v; cpu.sbase[s]=(uint32_t)v<<4; if(s==S_CS) cs_base=cpu.sbase[S_CS]; }
static uint32_t sb(int s){ return cpu.sbase[segovr>=0 ? segovr : s]; }

static void push16(uint16_t v){ REG16(R_ESP) -= 2; cpu_st16(cpu.sbase[S_SS] + REG16(R_ESP), v); }
static uint16_t pop16(void){ uint16_t v = cpu_ld16(cpu.sbase[S_SS] + REG16(R_ESP)); REG16(R_ESP)+=2; return v; }
static void push32(uint32_t v){ REG16(R_ESP) -= 4; cpu_st32(cpu.sbase[S_SS] + REG16(R_ESP), v); }
static uint32_t pop32(void){ uint32_t v = cpu_ld32(cpu.sbase[S_SS] + REG16(R_ESP)); REG16(R_ESP)+=4; return v; }
static void pushv(uint32_t v){ if(opsz==32) push32(v); else push16((uint16_t)v); }
static uint32_t popv(void){ return opsz==32?pop32():pop16(); }

void cpu_push16(uint16_t v){ push16(v); }
uint16_t cpu_pop16(void){ return pop16(); }

/* ------------------------------------------------------------- modrm     */
static void modrm(void){
    uint8_t m = fetch8();
    mod_ = m>>6; reg_ = (m>>3)&7; rm_ = m&7;
    ea_isreg = 0;
    if(mod_==3){ ea_isreg=1; return; }
    if(adsz==16){
        uint32_t a=0; int dseg=S_DS;
        switch(rm_){
        case 0: a=(uint16_t)(REG16(R_EBX)+REG16(R_ESI)); break;
        case 1: a=(uint16_t)(REG16(R_EBX)+REG16(R_EDI)); break;
        case 2: a=(uint16_t)(REG16(R_EBP)+REG16(R_ESI)); dseg=S_SS; break;
        case 3: a=(uint16_t)(REG16(R_EBP)+REG16(R_EDI)); dseg=S_SS; break;
        case 4: a=REG16(R_ESI); break;
        case 5: a=REG16(R_EDI); break;
        case 6: if(mod_==0){ a=fetch16(); } else { a=REG16(R_EBP); dseg=S_SS; } break;
        case 7: a=REG16(R_EBX); break;
        }
        if(mod_==1) a += (int8_t)fetch8();
        else if(mod_==2) a += (int16_t)fetch16();
        ea_off = a & 0xFFFF;
        ea = sb(dseg) + ea_off;
    } else {
        uint32_t a=0; int dseg=S_DS;
        if(rm_==4){
            uint8_t sib=fetch8();
            int base=sib&7, idx=(sib>>3)&7, sc=sib>>6;
            if(idx!=4) a += REG32(idx)<<sc;
            if(base==5 && mod_==0) a += fetch32();
            else { a += REG32(base); if(base==4||base==5) dseg=S_SS; }
        } else if(rm_==5 && mod_==0){ a = fetch32(); }
        else { a = REG32(rm_); if(rm_==5) dseg=S_SS; }
        if(mod_==1) a += (int32_t)(int8_t)fetch8();
        else if(mod_==2) a += fetch32();
        ea_off = a;
        ea = sb(dseg) + a;
    }
}

static uint32_t rdE(int sz){
    if(ea_isreg) return sz==8?REG8(rm_): sz==16?REG16(rm_):REG32(rm_);
    return sz==8?cpu_ld8(ea): sz==16?cpu_ld16(ea):cpu_ld32(ea);
}
static void wrE(int sz, uint32_t v){
    if(ea_isreg){ if(sz==8) REG8(rm_)=(uint8_t)v; else if(sz==16) REG16(rm_)=(uint16_t)v; else REG32(rm_)=v; return; }
    if(sz==8) cpu_st8(ea,(uint8_t)v); else if(sz==16) cpu_st16(ea,(uint16_t)v); else cpu_st32(ea,v);
}
static uint32_t rdG(int sz){ return sz==8?REG8(reg_): sz==16?REG16(reg_):REG32(reg_); }
static void wrG(int sz, uint32_t v){ if(sz==8) REG8(reg_)=(uint8_t)v; else if(sz==16) REG16(reg_)=(uint16_t)v; else REG32(reg_)=v; }

/* ------------------------------------------------------------- ALU       */
static void setlog(uint32_t r, int sz){
    cpu.cf=0; cpu.of=0; cpu.af=0;
    cpu.zf = (r & MASK(sz))==0;
    cpu.sf = (r >> (sz-1)) & 1;
    cpu.pf = ptab[r & 0xFF];
}
static uint32_t alu(int op, uint32_t a, uint32_t b, int sz){
    uint64_t r; uint32_t m = MASK(sz); uint32_t res; int c;
    a &= m; b &= m;
    switch(op){
    case 0: case 2:
        c = (op==2) ? (int)cpu.cf : 0;
        r = (uint64_t)a + b + c; res = (uint32_t)r & m;
        cpu.cf = (uint32_t)((r >> sz) & 1);
        cpu.af = ((a ^ b ^ res) >> 4) & 1;
        cpu.of = (((~(a^b)) & (a^res)) >> (sz-1)) & 1;
        break;
    case 5: case 3: case 7:
        c = (op==3) ? (int)cpu.cf : 0;
        r = (uint64_t)a - b - c; res = (uint32_t)r & m;
        cpu.cf = (uint32_t)((r >> sz) & 1);
        cpu.af = ((a ^ b ^ res) >> 4) & 1;
        cpu.of = (((a^b) & (a^res)) >> (sz-1)) & 1;
        break;
    case 1: res = (a|b)&m; setlog(res,sz); return res;
    case 4: res = (a&b)&m; setlog(res,sz); return res;
    case 6: res = (a^b)&m; setlog(res,sz); return res;
    default: res=0; break;
    }
    cpu.zf = res==0; cpu.sf = (res>>(sz-1))&1; cpu.pf = ptab[res&0xFF];
    return res;
}
static uint32_t do_inc(uint32_t a, int sz){
    uint32_t m=MASK(sz), r=(a+1)&m;
    cpu.af = ((a ^ 1 ^ r)>>4)&1;
    cpu.of = (r == ((m>>1)+1));
    cpu.zf = r==0; cpu.sf=(r>>(sz-1))&1; cpu.pf=ptab[r&0xFF];
    return r;
}
static uint32_t do_dec(uint32_t a, int sz){
    uint32_t m=MASK(sz), r=(a-1)&m;
    cpu.af = ((a ^ 1 ^ r)>>4)&1;
    cpu.of = (r == (m>>1));
    cpu.zf = r==0; cpu.sf=(r>>(sz-1))&1; cpu.pf=ptab[r&0xFF];
    return r;
}

/* ------------------------------------------------------------- shifts    */
static uint32_t do_shift(int op, uint32_t v, int cnt, int sz){
    uint32_t m = MASK(sz); uint32_t r = v & m; int i;
    cnt &= 31;
    if(!cnt) return r;
    switch(op){
    case 0: { int n = cnt % sz;
        if(n){ r = ((r<<n)|(r>>(sz-n))) & m; }
        cpu.cf = r & 1; cpu.of = (((r>>(sz-1))&1) ^ cpu.cf);
        return r; }
    case 1: { int n = cnt % sz;
        if(n){ r = ((r>>n)|(r<<(sz-n))) & m; }
        cpu.cf = (r>>(sz-1))&1; cpu.of = (((r>>(sz-1))&1) ^ ((r>>(sz-2))&1));
        return r; }
    case 2: { int n = cnt % (sz+1);
        for(i=0;i<n;i++){ uint32_t nc=(r>>(sz-1))&1; r=((r<<1)|cpu.cf)&m; cpu.cf=nc; }
        cpu.of = (((r>>(sz-1))&1) ^ cpu.cf);
        return r; }
    case 3: { int n = cnt % (sz+1);
        for(i=0;i<n;i++){ uint32_t nc=r&1; r=(r>>1)|(cpu.cf<<(sz-1)); cpu.cf=nc; }
        r &= m; cpu.of = (((r>>(sz-1))&1) ^ ((r>>(sz-2))&1));
        return r; }
    case 4: case 6:
        cpu.cf = (cnt<=sz) ? ((r >> (sz-cnt)) & 1) : 0;
        r = (cnt<32) ? ((r<<cnt)&m) : 0;
        cpu.of = (((r>>(sz-1))&1) ^ cpu.cf);
        break;
    case 5:
        cpu.cf = (cnt<=sz) ? ((r>>(cnt-1))&1) : 0;
        cpu.of = (v>>(sz-1))&1;
        r = (cnt<sz) ? (r>>cnt) : 0;
        break;
    case 7: {
        int32_t s = (int32_t)(r << (32-sz));
        s >>= (32-sz);
        cpu.cf = (cnt<sz) ? (uint32_t)((s>>(cnt-1))&1) : (uint32_t)(s<0?1:0);
        s = (cnt<sz) ? (s>>cnt) : (s<0?-1:0);
        r = ((uint32_t)s) & m; cpu.of = 0;
        break; }
    }
    cpu.zf = r==0; cpu.sf=(r>>(sz-1))&1; cpu.pf=ptab[r&0xFF];
    return r;
}

static int cond(int c){
    switch(c){
    case 0: return cpu.of;            case 1: return !cpu.of;
    case 2: return cpu.cf;            case 3: return !cpu.cf;
    case 4: return cpu.zf;            case 5: return !cpu.zf;
    case 6: return cpu.cf||cpu.zf;    case 7: return !(cpu.cf||cpu.zf);
    case 8: return cpu.sf;            case 9: return !cpu.sf;
    case 10: return cpu.pf;           case 11: return !cpu.pf;
    case 12: return cpu.sf!=cpu.of;   case 13: return cpu.sf==cpu.of;
    case 14: return cpu.zf||(cpu.sf!=cpu.of); case 15: return !cpu.zf&&(cpu.sf==cpu.of);
    }
    return 0;
}

void cpu_interrupt(int n, int soft){
    uint32_t v = cpu_ld32((uint32_t)n*4);
    push16((uint16_t)cpu_getflags());
    push16(cpu.sreg[S_CS]);
    push16((uint16_t)cpu.eip);
    cpu.iflag = 0; cpu.tf = 0;
    set_sreg(S_CS, (uint16_t)(v>>16));
    cpu.eip = v & 0xFFFF;
    cpu.halted = 0;
    (void)soft;
}

/* ------------------------------------------------------------- string    */
static void strop(int op, int sz){
    int step = (sz/8) * (cpu.df ? -1 : 1);
    uint32_t dsb = sb(S_DS), esb = cpu.sbase[S_ES];
    uint32_t cnt = 1, i;
    int use_rep = rep != 0;
    if(use_rep){ cnt = adsz==16 ? REG16(R_ECX) : REG32(R_ECX); if(cnt==0) return;
        /* Bulk fast path: REP MOVS/STOS forward over plain RAM.  The game
         * uses these for asset copies and buffer clears; the per-byte loop
         * below costs a full decode's worth of branches per byte.  Anything
         * touching VGA/ROM, wrapping the segment, or running backwards keeps
         * the exact slow path.  memmove covers (unlikely) overlap. */
        if((op == 0 || op == 2) && !cpu.df && cnt >= 16){
            int el = sz / 8;
            uint64_t n = ((uint64_t)cnt - 1u) * (uint64_t)el;
            int ok = 0;
            uint32_t s0 = 0, d0 = 0;
            if(adsz == 16){
                uint32_t soff = REG16(R_ESI), doff = REG16(R_EDI);
                if((uint64_t)soff + n <= 0xFFFFu && (uint64_t)doff + n <= 0xFFFFu &&
                   (uint64_t)dsb + soff + n < 0xA0000u && (uint64_t)esb + doff + n < 0xA0000u){
                    s0 = dsb + soff; d0 = esb + doff; ok = 1;
                }
            } else {
                uint64_t soff = REG32(R_ESI), doff = REG32(R_EDI);
                if(soff + n < 0xA0000u && doff + n < 0xA0000u &&
                   (uint64_t)dsb + soff + n < 0xA0000u && (uint64_t)esb + doff + n < 0xA0000u){
                    s0 = (uint32_t)(dsb + soff); d0 = (uint32_t)(esb + doff); ok = 1;
                }
            }
            if(ok){
                size_t len = (size_t)cnt * (size_t)el;
                if(op == 0) memmove(&ram[d0], &ram[s0], len);
                else if(sz == 8) memset(&ram[d0], REG8(0), len);
                else if(sz == 16){
                    uint16_t v = REG16(0); uint32_t d;
                    for(d = 0; d < cnt; d++){ ram[d0 + d*2] = (uint8_t)v; ram[d0 + d*2 + 1] = (uint8_t)(v >> 8); }
                } else {
                    uint32_t v = REG32(0); uint32_t d;
                    for(d = 0; d < cnt; d++){
                        ram[d0 + d*4] = (uint8_t)v; ram[d0 + d*4 + 1] = (uint8_t)(v >> 8);
                        ram[d0 + d*4 + 2] = (uint8_t)(v >> 16); ram[d0 + d*4 + 3] = (uint8_t)(v >> 24);
                    }
                }
                if(adsz == 16){
                    REG16(R_ESI) += (uint16_t)len; REG16(R_EDI) += (uint16_t)len;
                    REG16(R_ECX) = 0;
                } else {
                    REG32(R_ESI) += (uint32_t)len; REG32(R_EDI) += (uint32_t)len;
                    REG32(R_ECX) = 0;
                }
                cpu.cycles += cnt;
                return;
            }
        }
    }
    for(i=0;i<cnt;i++){
        uint32_t s = adsz==16 ? REG16(R_ESI) : REG32(R_ESI);
        uint32_t d = adsz==16 ? REG16(R_EDI) : REG32(R_EDI);
        uint32_t a,b;
        switch(op){
        case 0:
            a = sz==8?cpu_ld8(dsb+s): sz==16?cpu_ld16(dsb+s):cpu_ld32(dsb+s);
            if(sz==8) cpu_st8(esb+d,(uint8_t)a); else if(sz==16) cpu_st16(esb+d,(uint16_t)a); else cpu_st32(esb+d,a);
            break;
        case 1:
            a = sz==8?cpu_ld8(dsb+s): sz==16?cpu_ld16(dsb+s):cpu_ld32(dsb+s);
            b = sz==8?cpu_ld8(esb+d): sz==16?cpu_ld16(esb+d):cpu_ld32(esb+d);
            alu(7,a,b,sz); break;
        case 2:
            a = sz==8?REG8(0): sz==16?REG16(0):REG32(0);
            if(sz==8) cpu_st8(esb+d,(uint8_t)a); else if(sz==16) cpu_st16(esb+d,(uint16_t)a); else cpu_st32(esb+d,a);
            break;
        case 3:
            a = sz==8?cpu_ld8(dsb+s): sz==16?cpu_ld16(dsb+s):cpu_ld32(dsb+s);
            if(sz==8) REG8(0)=(uint8_t)a; else if(sz==16) REG16(0)=(uint16_t)a; else REG32(0)=a;
            break;
        case 4:
            a = sz==8?REG8(0): sz==16?REG16(0):REG32(0);
            b = sz==8?cpu_ld8(esb+d): sz==16?cpu_ld16(esb+d):cpu_ld32(esb+d);
            alu(7,a,b,sz); break;
        case 5:
            a = sz==8?io_r8(REG16(R_EDX)):io_r16(REG16(R_EDX));
            if(sz==8) cpu_st8(esb+d,(uint8_t)a); else cpu_st16(esb+d,(uint16_t)a);
            break;
        case 6:
            a = sz==8?cpu_ld8(dsb+s): cpu_ld16(dsb+s);
            if(sz==8) io_w8(REG16(R_EDX),(uint8_t)a); else io_w16(REG16(R_EDX),(uint16_t)a);
            break;
        }
        if(op==0||op==1||op==3||op==6){ if(adsz==16) REG16(R_ESI)+=(uint16_t)step; else REG32(R_ESI)+=step; }
        if(op==0||op==1||op==2||op==4||op==5){ if(adsz==16) REG16(R_EDI)+=(uint16_t)step; else REG32(R_EDI)+=step; }
        if(use_rep){
            if(adsz==16) REG16(R_ECX)--; else REG32(R_ECX)--;
            if(op==1||op==4){ if(rep==2 && !cpu.zf) break; if(rep==1 && cpu.zf) break; }
        }
    }
    cpu.cycles += cnt;
}

/* ---------------------------------------------------- emulator callbacks */
void (*cb_table[256])(void);
void cpu_no_iret(void){ no_iret = 1; }

static void op0f(void){
    uint8_t op = fetch8();
    uint32_t a,b,r; int sz = opsz;
    switch(op){
    case 0x00: modrm(); break;
    case 0x01:
        modrm();
        if(reg_==4) wrE(16, 0x0010);
        break;
    case 0x06: break;
    case 0x09: break;
    case 0x20: modrm(); REG32(rm_) = 0x00000010; break;
    case 0x22: modrm(); break;
    case 0x23: modrm(); break;
    case 0x21: modrm(); REG32(rm_) = 0; break;
    case 0x31: REG32(R_EAX)=(uint32_t)cpu.cycles; REG32(R_EDX)=(uint32_t)(cpu.cycles>>32); break;
    case 0xA2: REG32(R_EAX)=0; REG32(R_EBX)=0; REG32(R_ECX)=0; REG32(R_EDX)=0; break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F: {
        int32_t d = opsz==32 ? (int32_t)fetch32() : (int32_t)(int16_t)fetch16();
        if(cond(op&15)) cpu.eip = (cpu.eip + d) & 0xFFFF;
        break; }
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        modrm(); wrE(8, cond(op&15) ? 1 : 0); break;
    case 0xA0: pushv(cpu.sreg[S_FS]); break;
    case 0xA1: set_sreg(S_FS, (uint16_t)popv()); break;
    case 0xA8: pushv(cpu.sreg[S_GS]); break;
    case 0xA9: set_sreg(S_GS, (uint16_t)popv()); break;
    case 0xB2: modrm(); { uint32_t o=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); wrG(opsz,o); set_sreg(S_SS,s); } break;
    case 0xB4: modrm(); { uint32_t o=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); wrG(opsz,o); set_sreg(S_FS,s); } break;
    case 0xB5: modrm(); { uint32_t o=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); wrG(opsz,o); set_sreg(S_GS,s); } break;
    case 0xB6: modrm(); wrG(sz, (uint8_t)rdE(8)); break;
    case 0xB7: modrm(); wrG(sz, (uint16_t)rdE(16)); break;
    case 0xBE: modrm(); wrG(sz, (uint32_t)(int32_t)(int8_t)rdE(8)); break;
    case 0xBF: modrm(); wrG(sz, (uint32_t)(int32_t)(int16_t)rdE(16)); break;
    case 0xAF: modrm();
        if(sz==16){ int32_t p=(int32_t)(int16_t)rdG(16)*(int16_t)rdE(16); wrG(16,(uint16_t)p);
                    cpu.cf=cpu.of=((int32_t)(int16_t)p != p); }
        else { int64_t p=(int64_t)(int32_t)rdG(32)*(int32_t)rdE(32); wrG(32,(uint32_t)p);
               cpu.cf=cpu.of=((int64_t)(int32_t)p != p); }
        break;
    case 0xA3: case 0xAB: case 0xB3: case 0xBB: {
        int32_t bit; modrm(); bit = (int32_t)rdG(sz);
        if(sz==16) bit = (int16_t)bit;
        if(!ea_isreg){ ea += (uint32_t)((bit >> (sz==32?5:4)) * (sz/8)); }
        bit &= (sz-1);
        a = rdE(sz); cpu.cf = (a>>bit)&1;
        if(op==0xAB) a |= (1u<<bit); else if(op==0xB3) a &= ~(1u<<bit); else if(op==0xBB) a ^= (1u<<bit);
        if(op!=0xA3) wrE(sz,a);
        break; }
    case 0xBA: { int bit; modrm(); bit = fetch8() & (sz-1);
        a = rdE(sz); cpu.cf = (a>>bit)&1;
        if(reg_==5) a |= (1u<<bit); else if(reg_==6) a &= ~(1u<<bit); else if(reg_==7) a ^= (1u<<bit);
        if(reg_!=4) wrE(sz,a);
        break; }
    case 0xBC: { modrm(); a = rdE(sz); if(!a){cpu.zf=1;} else { int i=0; cpu.zf=0; while(!((a>>i)&1)) i++; wrG(sz,(uint32_t)i);} break; }
    case 0xBD: { modrm(); a = rdE(sz); if(!a){cpu.zf=1;} else { int i=sz-1; cpu.zf=0; while(!((a>>i)&1)) i--; wrG(sz,(uint32_t)i);} break; }
    case 0xA4: case 0xAC: case 0xA5: case 0xAD: {
        int c; int left = (op==0xA4||op==0xA5);
        modrm();
        if(op==0xA4||op==0xAC) c = fetch8() & 31; else c = REG8(1) & 31;
        a = rdE(sz); b = rdG(sz);
        if(c){
            if(left){ cpu.cf=(a>>(sz-c))&1; r = (a<<c)|(b>>(sz-c)); }
            else    { cpu.cf=(a>>(c-1))&1;  r = (a>>c)|(b<<(sz-c)); }
            r &= MASK(sz); wrE(sz,r); cpu.zf=r==0; cpu.sf=(r>>(sz-1))&1; cpu.pf=ptab[r&0xFF];
        }
        break; }
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF: {
        int i = op&7; uint32_t v = REG32(i);
        REG32(i) = (v>>24)|((v>>8)&0xFF00)|((v<<8)&0xFF0000)|(v<<24); break; }
    case 0xFF: {
        uint8_t id = fetch8();
        no_iret = 0;
        if(cb_table[id]) cb_table[id]();
        if(!no_iret){ uint16_t ip=pop16(), c=pop16(), f=pop16();
                      cpu.eip=ip; set_sreg(S_CS,c); cpu_setflags(f); }
        break; }
    default:
        trc("[cpu] unhandled 0F %02X at %04X:%04X\n", op, cpu.sreg[S_CS], cpu.eip);
        break;
    }
}

/* Ring of recently executed instruction addresses.  When the guest runs off
 * into data (a bad far return, a jump through a clobbered pointer) the final
 * CS:IP tells you nothing; the ring shows where it left real code. */
#define XRING 8192
uint32_t x_ring[XRING]; uint16_t x_cs[XRING], x_ip[XRING];
unsigned x_pos = 0; int x_on = 0;
uint32_t x_trap_lo = 1, x_trap_hi = 0;   /* -trap LO HI : stop on entry */
int int_watch = -1;                     /* -intwatch NN : log INT NN calls */

void cpu_step(void){
    uint8_t op;
    uint32_t a,b,r;
    int sz;
    opsz = 16; adsz = 16; segovr = -1; rep = 0;
    cs_base = cpu.sbase[S_CS];
    /* -balldbg: two compares against PUTTHEBALL's entry/RETN (see
     * fantasies.c).  One predictable branch per instruction when off. */
    /* PUTTHEBALL hooks.  The epilogue (balldbg_pos) is live in normal play -
     * it feeds the camera/ball pairing in vga.c - and so is the entry, which
     * the present-window derivation needs; two compares per instruction
     * whenever a table is loaded.  exit is only interesting to the -balldbg
     * logger and stays behind its flag. */
    if(balldbg_pos){
        uint32_t la = cs_base + cpu.eip;
        if(la == balldbg_pos || la == balldbg_entry) fantasies_ballgap_exec(la);
        else if(balldbg_on && la == balldbg_exit) fantasies_ballgap_exec(la);
    }
    /* -matdbg: the dot-matrix guard, its tick, and the driver's crisis flag
     * (see fantasies.c).  Behind the flag, so normal play pays one compare. */
    if(mat_tick_site){
        uint32_t la = cs_base + cpu.eip;
        if(la == mat_tick_site || la == mat_call_site || la == mat_crisis_site)
            fantasies_matrix_exec(la);
    }
    if(x_on){
        x_ring[x_pos & (XRING-1)] = cs_base + cpu.eip;
        x_cs[x_pos & (XRING-1)] = cpu.sreg[S_CS];
        x_ip[x_pos & (XRING-1)] = (uint16_t)cpu.eip;
        x_pos++;
        if(cs_base + cpu.eip >= x_trap_lo && cs_base + cpu.eip <= x_trap_hi){
            printf("[trap] hit at %04X:%04X lin=%05X after %llu instructions\n",
                   cpu.sreg[S_CS], (unsigned)cpu.eip, (unsigned)(cs_base+cpu.eip),
                   (unsigned long long)cpu.cycles);
            cpu.shutdown = 1; x_trap_lo = 1; x_trap_hi = 0;   /* disarm, keep ring */
        }
    }

again:
    op = fetch8();
    switch(op){
    case 0x26: segovr=S_ES; goto again;
    case 0x2E: segovr=S_CS; goto again;
    case 0x36: segovr=S_SS; goto again;
    case 0x3E: segovr=S_DS; goto again;
    case 0x64: segovr=S_FS; goto again;
    case 0x65: segovr=S_GS; goto again;
    case 0x66: opsz = (opsz==16)?32:16; goto again;
    case 0x67: adsz = (adsz==16)?32:16; goto again;
    case 0xF0: goto again;
    case 0xF2: rep=1; goto again;
    case 0xF3: rep=2; goto again;

    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30: case 0x38:
        modrm(); r = alu(op>>3, rdE(8), rdG(8), 8); if((op>>3)!=7) wrE(8,r); break;
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31: case 0x39:
        modrm(); sz=opsz; r = alu(op>>3, rdE(sz), rdG(sz), sz); if((op>>3)!=7) wrE(sz,r); break;
    case 0x02: case 0x0A: case 0x12: case 0x1A: case 0x22: case 0x2A: case 0x32: case 0x3A:
        modrm(); r = alu(op>>3, rdG(8), rdE(8), 8); if((op>>3)!=7) wrG(8,r); break;
    case 0x03: case 0x0B: case 0x13: case 0x1B: case 0x23: case 0x2B: case 0x33: case 0x3B:
        modrm(); sz=opsz; r = alu(op>>3, rdG(sz), rdE(sz), sz); if((op>>3)!=7) wrG(sz,r); break;
    case 0x04: case 0x0C: case 0x14: case 0x1C: case 0x24: case 0x2C: case 0x34: case 0x3C:
        a=fetch8(); r = alu(op>>3, REG8(0), a, 8); if((op>>3)!=7) REG8(0)=(uint8_t)r; break;
    case 0x05: case 0x0D: case 0x15: case 0x1D: case 0x25: case 0x2D: case 0x35: case 0x3D:
        sz=opsz; a = (sz==32)?fetch32():fetch16();
        r = alu(op>>3, sz==32?REG32(0):REG16(0), a, sz);
        if((op>>3)!=7){ if(sz==32) REG32(0)=r; else REG16(0)=(uint16_t)r; } break;

    case 0x06: pushv(cpu.sreg[S_ES]); break;
    case 0x07: set_sreg(S_ES, (uint16_t)popv()); break;
    case 0x0E: pushv(cpu.sreg[S_CS]); break;
    case 0x16: pushv(cpu.sreg[S_SS]); break;
    case 0x17: set_sreg(S_SS, (uint16_t)popv()); break;
    case 0x1E: pushv(cpu.sreg[S_DS]); break;
    case 0x1F: set_sreg(S_DS, (uint16_t)popv()); break;

    case 0x0F: op0f(); break;

    case 0x27: { uint8_t al=REG8(0); uint32_t oc=cpu.cf; uint32_t oa;
        cpu.cf=0; oa = cpu.af;
        if((al&15)>9 || oa){ int t=al+6; al=(uint8_t)t; cpu.cf=oc|((t>>8)&1); cpu.af=1; } else cpu.af=0;
        if((REG8(0))>0x99 || oc){ al+=0x60; cpu.cf=1; }
        REG8(0)=al; cpu.zf=al==0; cpu.sf=al>>7; cpu.pf=ptab[al]; break; }
    case 0x2F: { uint8_t al=REG8(0); uint32_t oc=cpu.cf; uint32_t oa=cpu.af;
        cpu.cf=0;
        if((al&15)>9 || oa){ int t=al-6; al=(uint8_t)t; cpu.cf=oc|((t>>8)&1); cpu.af=1; } else cpu.af=0;
        if((REG8(0))>0x99 || oc){ al-=0x60; cpu.cf=1; }
        REG8(0)=al; cpu.zf=al==0; cpu.sf=al>>7; cpu.pf=ptab[al]; break; }
    case 0x37:
        if(((REG8(0))&15)>9 || cpu.af){ REG16(0)+=0x106; cpu.af=1; cpu.cf=1; } else { cpu.af=0; cpu.cf=0; }
        REG8(0)&=0x0F; break;
    case 0x3F:
        if(((REG8(0))&15)>9 || cpu.af){ REG8(0)-=6; REG8(4)-=1; cpu.af=1; cpu.cf=1; } else { cpu.af=0; cpu.cf=0; }
        REG8(0)&=0x0F; break;

    case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45: case 0x46: case 0x47:
        if(opsz==32) REG32(op&7)=do_inc(REG32(op&7),32); else REG16(op&7)=(uint16_t)do_inc(REG16(op&7),16); break;
    case 0x48: case 0x49: case 0x4A: case 0x4B: case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        if(opsz==32) REG32(op&7)=do_dec(REG32(op&7),32); else REG16(op&7)=(uint16_t)do_dec(REG16(op&7),16); break;

    case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55: case 0x56: case 0x57:
        pushv(opsz==32?REG32(op&7):REG16(op&7)); break;
    case 0x58: case 0x59: case 0x5A: case 0x5B: case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        if(opsz==32) REG32(op&7)=pop32(); else REG16(op&7)=pop16(); break;

    case 0x60: { int i;
        if(opsz==32){ uint32_t sp=REG32(R_ESP); for(i=0;i<8;i++) push32(i==4?sp:REG32(i)); }
        else { uint16_t sp=REG16(R_ESP); for(i=0;i<8;i++) push16(i==4?sp:REG16(i)); }
        break; }
    case 0x61: { int i;
        if(opsz==32){ for(i=7;i>=0;i--){ uint32_t v=pop32(); if(i!=4) REG32(i)=v; } }
        else { for(i=7;i>=0;i--){ uint16_t v=pop16(); if(i!=4) REG16(i)=v; } }
        break; }
    case 0x62: modrm(); break;
    case 0x63: modrm(); break;
    case 0x68: pushv(opsz==32?fetch32():fetch16()); break;
    case 0x6A: pushv((uint32_t)(int32_t)(int8_t)fetch8()); break;
    case 0x69: modrm(); sz=opsz; a=rdE(sz); b=(sz==32)?fetch32():fetch16();
        if(sz==16){ int32_t p=(int32_t)(int16_t)a*(int16_t)b; wrG(16,(uint16_t)p); cpu.cf=cpu.of=((int32_t)(int16_t)p!=p); }
        else { int64_t p=(int64_t)(int32_t)a*(int32_t)b; wrG(32,(uint32_t)p); cpu.cf=cpu.of=((int64_t)(int32_t)p!=p); }
        break;
    case 0x6B: modrm(); sz=opsz; a=rdE(sz); b=(uint32_t)(int32_t)(int8_t)fetch8();
        if(sz==16){ int32_t p=(int32_t)(int16_t)a*(int16_t)b; wrG(16,(uint16_t)p); cpu.cf=cpu.of=((int32_t)(int16_t)p!=p); }
        else { int64_t p=(int64_t)(int32_t)a*(int32_t)b; wrG(32,(uint32_t)p); cpu.cf=cpu.of=((int64_t)(int32_t)p!=p); }
        break;
    case 0x6C: strop(5,8); break;
    case 0x6D: strop(5,opsz); break;
    case 0x6E: strop(6,8); break;
    case 0x6F: strop(6,opsz); break;

    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75: case 0x76: case 0x77:
    case 0x78: case 0x79: case 0x7A: case 0x7B: case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t d = (int8_t)fetch8();
        if(cond(op&15)) cpu.eip = (cpu.eip + d) & 0xFFFF;
        break; }

    case 0x80: case 0x82: modrm(); a=rdE(8); b=fetch8(); r=alu(reg_,a,b,8); if(reg_!=7) wrE(8,r); break;
    case 0x81: modrm(); sz=opsz; a=rdE(sz); b=(sz==32)?fetch32():fetch16(); r=alu(reg_,a,b,sz); if(reg_!=7) wrE(sz,r); break;
    case 0x83: modrm(); sz=opsz; a=rdE(sz); b=(uint32_t)(int32_t)(int8_t)fetch8(); r=alu(reg_,a,b,sz); if(reg_!=7) wrE(sz,r); break;

    case 0x84: modrm(); alu(4, rdE(8), rdG(8), 8); break;
    case 0x85: modrm(); sz=opsz; alu(4, rdE(sz), rdG(sz), sz); break;
    case 0x86: modrm(); a=rdE(8); wrE(8, rdG(8)); wrG(8,a); break;
    case 0x87: modrm(); sz=opsz; a=rdE(sz); wrE(sz, rdG(sz)); wrG(sz,a); break;
    case 0x88: modrm(); wrE(8, rdG(8)); break;
    case 0x89: modrm(); sz=opsz; wrE(sz, rdG(sz)); break;
    case 0x8A: modrm(); wrG(8, rdE(8)); break;
    case 0x8B: modrm(); sz=opsz; wrG(sz, rdE(sz)); break;
    case 0x8C: modrm(); if(ea_isreg) REG16(rm_)=cpu.sreg[reg_&7]; else cpu_st16(ea, cpu.sreg[reg_&7]); break;
    case 0x8D: modrm(); wrG(opsz, ea_off); break;
    case 0x8E: modrm(); set_sreg(reg_&7, (uint16_t)rdE(16)); break;
    case 0x8F: modrm(); { uint32_t v = popv(); wrE(opsz, v); } break;

    case 0x90: break;
    case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
        if(opsz==32){ uint32_t t=REG32(0); REG32(0)=REG32(op&7); REG32(op&7)=t; }
        else { uint16_t t=REG16(0); REG16(0)=REG16(op&7); REG16(op&7)=t; } break;
    case 0x98: if(opsz==32) REG32(0)=(uint32_t)(int32_t)(int16_t)REG16(0); else REG16(0)=(uint16_t)(int16_t)(int8_t)REG8(0); break;
    case 0x99: if(opsz==32) REG32(R_EDX)=((int32_t)REG32(0)<0)?0xFFFFFFFFu:0; else REG16(R_EDX)=((int16_t)REG16(0)<0)?0xFFFF:0; break;
    case 0x9A: { uint32_t noff = (opsz==32)?fetch32():fetch16(); uint16_t nseg=fetch16();
        pushv(cpu.sreg[S_CS]); pushv(cpu.eip); set_sreg(S_CS,nseg); cpu.eip=noff&0xFFFF; break; }
    case 0x9B: break;
    case 0x9C: pushv(cpu_getflags()); break;
    case 0x9D: cpu_setflags(popv()); break;
    case 0x9E: cpu_setflags((cpu_getflags()&0xFFFFFF00u)|REG8(4)); break;
    case 0x9F: REG8(4) = (uint8_t)((cpu_getflags()&0xD5)|2); break;

    case 0xA0: { uint32_t o = (adsz==32)?fetch32():fetch16(); REG8(0)=cpu_ld8(sb(S_DS)+o); break; }
    case 0xA1: { uint32_t o = (adsz==32)?fetch32():fetch16();
        if(opsz==32) REG32(0)=cpu_ld32(sb(S_DS)+o); else REG16(0)=cpu_ld16(sb(S_DS)+o); break; }
    case 0xA2: { uint32_t o = (adsz==32)?fetch32():fetch16(); cpu_st8(sb(S_DS)+o, REG8(0)); break; }
    case 0xA3: { uint32_t o = (adsz==32)?fetch32():fetch16();
        if(opsz==32) cpu_st32(sb(S_DS)+o, REG32(0)); else cpu_st16(sb(S_DS)+o, REG16(0)); break; }
    case 0xA4: strop(0,8); break;
    case 0xA5: strop(0,opsz); break;
    case 0xA6: strop(1,8); break;
    case 0xA7: strop(1,opsz); break;
    case 0xA8: a=fetch8(); alu(4, REG8(0), a, 8); break;
    case 0xA9: sz=opsz; a=(sz==32)?fetch32():fetch16(); alu(4, sz==32?REG32(0):REG16(0), a, sz); break;
    case 0xAA: strop(2,8); break;
    case 0xAB: strop(2,opsz); break;
    case 0xAC: strop(3,8); break;
    case 0xAD: strop(3,opsz); break;
    case 0xAE: strop(4,8); break;
    case 0xAF: strop(4,opsz); break;

    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
        REG8(op&7) = fetch8(); break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        if(opsz==32) REG32(op&7)=fetch32(); else REG16(op&7)=fetch16(); break;

    case 0xC0: modrm(); { int c=fetch8(); wrE(8, do_shift(reg_, rdE(8), c, 8)); } break;
    case 0xC1: modrm(); sz=opsz; { int c=fetch8(); wrE(sz, do_shift(reg_, rdE(sz), c, sz)); } break;
    case 0xC2: { uint16_t n=fetch16(); cpu.eip = popv() & 0xFFFF; REG16(R_ESP)+=n; break; }
    case 0xC3: cpu.eip = popv() & 0xFFFF; break;
    case 0xC4: modrm(); { uint32_t v=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); wrG(opsz,v); set_sreg(S_ES,s); } break;
    case 0xC5: modrm(); { uint32_t v=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); wrG(opsz,v); set_sreg(S_DS,s); } break;
    case 0xC6: modrm(); { uint8_t v=fetch8(); wrE(8,v); } break;
    case 0xC7: modrm(); sz=opsz; { uint32_t v=(sz==32)?fetch32():fetch16(); wrE(sz,v); } break;
    case 0xC8: { uint16_t nb=fetch16(); uint8_t lvl=fetch8(); uint16_t fp; int i;
        push16(REG16(R_EBP)); fp=REG16(R_ESP);
        for(i=1;i<lvl;i++){ REG16(R_EBP)-=2; push16(cpu_ld16(cpu.sbase[S_SS]+REG16(R_EBP))); }
        if(lvl>0) push16(fp);
        REG16(R_EBP)=fp; REG16(R_ESP)-=nb; break; }
    case 0xC9: REG16(R_ESP)=REG16(R_EBP); REG16(R_EBP)=pop16(); break;
    case 0xCA: { uint16_t n=fetch16(); uint32_t o=popv(); uint16_t s=(uint16_t)popv();
        cpu.eip=o&0xFFFF; set_sreg(S_CS,s); REG16(R_ESP)+=n; break; }
    case 0xCB: { uint32_t o=popv(); uint16_t s=(uint16_t)popv(); cpu.eip=o&0xFFFF; set_sreg(S_CS,s); break; }
    case 0xCC: cpu_interrupt(3,1); break;
    case 0xCD: { uint8_t n=fetch8();
        if(int_watch >= 0 && n == int_watch)
            printf("[int%02X] AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X ES=%04X from %04X:%04X\n",
                   n, REG16(R_EAX), REG16(R_EBX), REG16(R_ECX), REG16(R_EDX),
                   cpu.sreg[S_DS], cpu.sreg[S_ES],
                   cpu.sreg[S_CS], (unsigned)(cpu.eip-2));
        cpu_interrupt(n,1); break; }
    case 0xCE: if(cpu.of) cpu_interrupt(4,1); break;
    case 0xCF: { uint16_t ip=pop16(), s=pop16(), f=pop16();
        cpu.eip=ip; set_sreg(S_CS,s); cpu_setflags(f); break; }

    case 0xD0: modrm(); wrE(8, do_shift(reg_, rdE(8), 1, 8)); break;
    case 0xD1: modrm(); sz=opsz; wrE(sz, do_shift(reg_, rdE(sz), 1, sz)); break;
    case 0xD2: modrm(); wrE(8, do_shift(reg_, rdE(8), REG8(1), 8)); break;
    case 0xD3: modrm(); sz=opsz; wrE(sz, do_shift(reg_, rdE(sz), REG8(1), sz)); break;
    case 0xD4: { uint8_t base=fetch8(); if(base){ REG8(4)=REG8(0)/base; REG8(0)=REG8(0)%base; }
        cpu.zf=REG8(0)==0; cpu.sf=REG8(0)>>7; cpu.pf=ptab[REG8(0)]; break; }
    case 0xD5: { uint8_t base=fetch8(); REG8(0)=(uint8_t)(REG8(0)+REG8(4)*base); REG8(4)=0;
        cpu.zf=REG8(0)==0; cpu.sf=REG8(0)>>7; cpu.pf=ptab[REG8(0)]; break; }
    case 0xD6: REG8(0) = cpu.cf ? 0xFF : 0x00; break;
    case 0xD7: REG8(0) = cpu_ld8(sb(S_DS) + ((REG16(R_EBX)+REG8(0))&0xFFFF)); break;
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:
        modrm();
        trc("[cpu] x87 esc %02X at %04X:%04X\n", op, cpu.sreg[S_CS], cpu.eip);
        break;

    case 0xE0: case 0xE1: case 0xE2: { int8_t d=(int8_t)fetch8(); uint32_t c; int take;
        if(adsz==32) c = --REG32(R_ECX); else c = --REG16(R_ECX);
        take = c!=0;
        if(op==0xE0) take = take && !cpu.zf;
        if(op==0xE1) take = take && cpu.zf;
        if(take) cpu.eip=(cpu.eip+d)&0xFFFF; break; }
    case 0xE3: { int8_t d=(int8_t)fetch8(); uint32_t c = adsz==32?REG32(R_ECX):REG16(R_ECX);
        if(!c) cpu.eip=(cpu.eip+d)&0xFFFF; break; }
    case 0xE4: { uint8_t p=fetch8(); REG8(0)=io_r8(p); break; }
    case 0xE5: { uint8_t p=fetch8(); REG16(0)=io_r16(p); break; }
    case 0xE6: { uint8_t p=fetch8(); io_w8(p, REG8(0)); break; }
    case 0xE7: { uint8_t p=fetch8(); io_w16(p, REG16(0)); break; }
    case 0xE8: { int32_t d = (opsz==32)?(int32_t)fetch32():(int32_t)(int16_t)fetch16();
        pushv(cpu.eip); cpu.eip=(cpu.eip+d)&0xFFFF; break; }
    case 0xE9: { int32_t d = (opsz==32)?(int32_t)fetch32():(int32_t)(int16_t)fetch16();
        cpu.eip=(cpu.eip+d)&0xFFFF; break; }
    case 0xEA: { uint32_t o=(opsz==32)?fetch32():fetch16(); uint16_t s=fetch16();
        set_sreg(S_CS,s); cpu.eip=o&0xFFFF; break; }
    case 0xEB: { int8_t d=(int8_t)fetch8(); cpu.eip=(cpu.eip+d)&0xFFFF; break; }
    case 0xEC: REG8(0)=io_r8(REG16(R_EDX)); break;
    case 0xED: REG16(0)=io_r16(REG16(R_EDX)); break;
    case 0xEE: io_w8(REG16(R_EDX), REG8(0)); break;
    case 0xEF: io_w16(REG16(R_EDX), REG16(0)); break;

    case 0xF4: cpu.halted = 1; break;
    case 0xF5: cpu.cf = !cpu.cf; break;
    case 0xF6: modrm(); a=rdE(8);
        switch(reg_){
        case 0: case 1: b=fetch8(); alu(4,a,b,8); break;
        case 2: wrE(8, (~a)&0xFF); break;
        case 3: r=alu(5,0,a,8); wrE(8,r); break;
        case 4: { uint16_t p=(uint16_t)((uint16_t)REG8(0)*(uint8_t)a); REG16(0)=p;
                  cpu.cf=cpu.of=((p>>8)!=0); cpu.zf=(p&0xFF)==0; cpu.sf=(p>>7)&1; cpu.pf=ptab[p&0xFF]; break; }
        case 5: { int16_t p=(int16_t)((int16_t)(int8_t)REG8(0)*(int8_t)a); REG16(0)=(uint16_t)p;
                  cpu.cf=cpu.of=((int16_t)(int8_t)p != p); break; }
        case 6: { uint16_t n; if(!a){ cpu_interrupt(0,0); break; } n=REG16(0);
                  if(n/a > 0xFF){ cpu_interrupt(0,0); break; } REG8(0)=(uint8_t)(n/a); REG8(4)=(uint8_t)(n%a); break; }
        case 7: { int16_t n; int8_t d; int32_t q; if(!a){ cpu_interrupt(0,0); break; }
                  n=(int16_t)REG16(0); d=(int8_t)a; q=n/d;
                  if(q>127||q<-128){ cpu_interrupt(0,0); break; }
                  REG8(0)=(uint8_t)q; REG8(4)=(uint8_t)(n%d); break; }
        } break;
    case 0xF7: modrm(); sz=opsz; a=rdE(sz);
        switch(reg_){
        case 0: case 1: b=(sz==32)?fetch32():fetch16(); alu(4,a,b,sz); break;
        case 2: wrE(sz, (~a)&MASK(sz)); break;
        case 3: r=alu(5,0,a,sz); wrE(sz,r); break;
        case 4: if(sz==16){ uint32_t p=(uint32_t)REG16(0)*(uint16_t)a; REG16(0)=(uint16_t)p; REG16(R_EDX)=(uint16_t)(p>>16);
                            cpu.cf=cpu.of=((p>>16)!=0); }
                else { uint64_t p=(uint64_t)REG32(0)*a; REG32(0)=(uint32_t)p; REG32(R_EDX)=(uint32_t)(p>>32);
                       cpu.cf=cpu.of=((p>>32)!=0); } break;
        case 5: if(sz==16){ int32_t p=(int32_t)(int16_t)REG16(0)*(int16_t)a; REG16(0)=(uint16_t)p; REG16(R_EDX)=(uint16_t)(p>>16);
                            cpu.cf=cpu.of=((int32_t)(int16_t)p != p); }
                else { int64_t p=(int64_t)(int32_t)REG32(0)*(int32_t)a; REG32(0)=(uint32_t)p; REG32(R_EDX)=(uint32_t)(p>>32);
                       cpu.cf=cpu.of=((int64_t)(int32_t)p != p); } break;
        case 6: if(sz==16){ uint32_t n; if(!a){cpu_interrupt(0,0);break;} n=((uint32_t)REG16(R_EDX)<<16)|REG16(0);
                            if(n/a > 0xFFFF){ cpu_interrupt(0,0); break; } REG16(0)=(uint16_t)(n/a); REG16(R_EDX)=(uint16_t)(n%a); }
                else { uint64_t n; if(!a){cpu_interrupt(0,0);break;} n=((uint64_t)REG32(R_EDX)<<32)|REG32(0);
                       if(n/a > 0xFFFFFFFFull){ cpu_interrupt(0,0); break; } REG32(0)=(uint32_t)(n/a); REG32(R_EDX)=(uint32_t)(n%a); } break;
        case 7: if(sz==16){ int32_t n,q; if(!a){cpu_interrupt(0,0);break;} n=(int32_t)(((uint32_t)REG16(R_EDX)<<16)|REG16(0));
                            q=n/(int16_t)a; if(q>32767||q<-32768){cpu_interrupt(0,0);break;}
                            REG16(0)=(uint16_t)q; REG16(R_EDX)=(uint16_t)(n%(int16_t)a); }
                else { int64_t n,q; if(!a){cpu_interrupt(0,0);break;} n=(int64_t)(((uint64_t)REG32(R_EDX)<<32)|REG32(0));
                       q=n/(int32_t)a; if(q>2147483647LL||q<-2147483648LL){cpu_interrupt(0,0);break;}
                       REG32(0)=(uint32_t)q; REG32(R_EDX)=(uint32_t)(n%(int32_t)a); } break;
        } break;
    case 0xF8: cpu.cf=0; break;
    case 0xF9: cpu.cf=1; break;
    case 0xFA: cpu.iflag=0; break;
    case 0xFB: cpu.iflag=1; break;
    case 0xFC: cpu.df=0; break;
    case 0xFD: cpu.df=1; break;
    case 0xFE: modrm(); a=rdE(8);
        if(reg_==0) wrE(8, do_inc(a,8)); else wrE(8, do_dec(a,8)); break;
    case 0xFF: modrm(); sz=opsz;
        switch(reg_){
        case 0: wrE(sz, do_inc(rdE(sz),sz)); break;
        case 1: wrE(sz, do_dec(rdE(sz),sz)); break;
        case 2: { uint32_t t=rdE(sz); pushv(cpu.eip); cpu.eip=t&0xFFFF; break; }
        case 3: { uint32_t o=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2);
                  pushv(cpu.sreg[S_CS]); pushv(cpu.eip); set_sreg(S_CS,s); cpu.eip=o; break; }
        case 4: cpu.eip = rdE(sz)&0xFFFF; break;
        case 5: { uint32_t o=cpu_ld16(ea); uint16_t s=cpu_ld16(ea+2); set_sreg(S_CS,s); cpu.eip=o; break; }
        case 6: pushv(rdE(sz)); break;
        } break;

    default:
        trc("[cpu] unhandled opcode %02X at %04X:%04X\n", op, cpu.sreg[S_CS], cpu.eip-1);
        break;
    }
    cpu.cycles++;
}

void cpu_reset(void){
    init_ptab();
    memset(&cpu,0,sizeof(cpu));
    cpu.iflag = 1;
    set_sreg(S_CS,0xF000); cpu.eip=0xFFF0;
}
