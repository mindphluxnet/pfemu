/* BIOS services: INT 10h/11h/12h/13h/15h/16h/1Ah, IRQ0/IRQ1 handlers, IVT set-up */
#include "pfemu.h"

extern void (*cb_table[256])(void);
extern void set_sreg(int s, uint16_t v);
extern void cpu_no_iret(void);
extern double emu_time;
extern int vga_get_mode(void);
extern uint8_t vga_vram[256*1024];

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

#define BDA8(o)   ram[0x400+(o)]
#define BDA16(o)  (*(uint16_t*)&ram[0x400+(o)])

/* ------------------------------------------------------------ scancodes */
static const uint8_t sc2asc_lo[128] = {
 0,27,'1','2','3','4','5','6','7','8','9','0','-','=','\b','\t',
 'q','w','e','r','t','y','u','i','o','p','[',']','\r',0,'a','s',
 'd','f','g','h','j','k','l',';','\'','`',0,'\\','z','x','c','v',
 'b','n','m',',','.','/',0,'*',0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,'-',0,'5',0,'+',0,
 0,0,0,0,0,0,'\\',0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };
static const uint8_t sc2asc_hi[128] = {
 0,27,'!','@','#','$','%','^','&','*','(',')','_','+','\b','\t',
 'Q','W','E','R','T','Y','U','I','O','P','{','}','\r',0,'A','S',
 'D','F','G','H','J','K','L',':','"','~',0,'|','Z','X','C','V',
 'B','N','M','<','>','?',0,'*',0,' ',0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,'-',0,'5',0,'+',0,
 0,0,0,0,0,0,'|',0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0 };

static void kbuf_put(uint16_t v){
    uint16_t tail = BDA16(0x1C), next = (uint16_t)(tail + 2);
    if(next >= BDA16(0x82)) next = BDA16(0x80);
    if(next == BDA16(0x1A)) return;               /* full */
    *(uint16_t*)&ram[0x400 + tail] = v;
    BDA16(0x1C) = next;
}
static int kbuf_get(uint16_t *out){
    uint16_t head = BDA16(0x1A);
    if(head == BDA16(0x1C)) return 0;
    *out = *(uint16_t*)&ram[0x400 + head];
    head = (uint16_t)(head + 2);
    if(head >= BDA16(0x82)) head = BDA16(0x80);
    BDA16(0x1A) = head;
    return 1;
}
static int kbuf_peek(uint16_t *out){
    uint16_t head = BDA16(0x1A);
    if(head == BDA16(0x1C)) return 0;
    *out = *(uint16_t*)&ram[0x400 + head];
    return 1;
}
/* DOS buffered input (INT 21h AH=0Ah) consumes the same type-ahead queue. */
int bios_kbuf_get(uint16_t *out){ return kbuf_get(out); }
int bios_kbuf_peek(uint16_t *out){ return kbuf_peek(out); }

/* ----------------------------------------------------------- IRQ1 (int9) */
static void bios_int9(void){
    uint8_t sc = io_r8(0x60);
    uint8_t shift = BDA8(0x17);
    uint8_t code = sc & 0x7F;
    int down = !(sc & 0x80);
    switch(code){
    case 0x2A: shift = (uint8_t)(down ? (shift|0x02) : (shift&~0x02)); break;
    case 0x36: shift = (uint8_t)(down ? (shift|0x01) : (shift&~0x01)); break;
    case 0x1D: shift = (uint8_t)(down ? (shift|0x04) : (shift&~0x04)); break;
    case 0x38: shift = (uint8_t)(down ? (shift|0x08) : (shift&~0x08)); break;
    default:
        if(down){
            uint8_t a = (shift & 0x03) ? sc2asc_hi[code] : sc2asc_lo[code];
            kbuf_put((uint16_t)((code<<8) | a));
        }
        break;
    }
    BDA8(0x17) = shift;
    io_w8(0x20, 0x20);
}

/* ------------------------------------------------------------- INT 10h  */
void bios_tty_a(uint8_t c, uint8_t attr);
static void putch_tty(uint8_t c, uint8_t attr){
    uint16_t pos = BDA16(0x50);
    int col = pos & 0xFF, row = pos >> 8;
    int cols = BDA16(0x4A); if(!cols) cols = 80;
    if(c=='\r'){ col=0; }
    else if(c=='\n'){ row++; }
    else if(c=='\b'){ if(col) col--; }
    else if(c==7){ }
    else {
        uint32_t o = (uint32_t)(row*cols + col);
        vga_vram[o*4+0] = c; vga_vram[o*4+1] = attr;
        col++;
    }
    if(col >= cols){ col = 0; row++; }
    if(row >= 25){
        int i;
        memmove(&vga_vram[0], &vga_vram[cols*4], (size_t)(24*cols*4));
        for(i=0;i<cols;i++){ uint32_t o=(uint32_t)(24*cols+i); vga_vram[o*4+0]=' '; vga_vram[o*4+1]=attr; }
        row = 24;
    }
    BDA16(0x50) = (uint16_t)((row<<8)|col);
    vga_dirty = 1;
}

static void bios_int10(void){
    switch(AH){
    case 0x00: vga_set_mode_bios(AL & 0x7F); BDA8(0x49)=AL&0x7F; BDA16(0x4A)=80; BDA16(0x50)=0; break;
    case 0x01: BDA16(0x60) = CX; break;
    case 0x02: BDA16(0x50 + (BH&7)*2) = DX; break;
    case 0x03: DX = BDA16(0x50 + (BH&7)*2); CX = BDA16(0x60); break;
    case 0x05: BDA8(0x62) = AL; break;
    case 0x06: case 0x07: {
        int top = CH, left = CL, bot = DH, right = DL, n = AL, cols = BDA16(0x4A);
        int r,c;
        if(!cols) cols = 80;
        if(n==0 || n > (bot-top+1)){
            for(r=top;r<=bot;r++) for(c=left;c<=right;c++){
                uint32_t o=(uint32_t)(r*cols+c); vga_vram[o*4+0]=' '; vga_vram[o*4+1]=BH; }
        } else if(AH==6){
            for(r=top;r<=bot-n;r++) for(c=left;c<=right;c++){
                uint32_t d=(uint32_t)(r*cols+c), s=(uint32_t)((r+n)*cols+c);
                vga_vram[d*4+0]=vga_vram[s*4+0]; vga_vram[d*4+1]=vga_vram[s*4+1]; }
            for(r=bot-n+1;r<=bot;r++) for(c=left;c<=right;c++){
                uint32_t o=(uint32_t)(r*cols+c); vga_vram[o*4+0]=' '; vga_vram[o*4+1]=BH; }
        } else {
            for(r=bot;r>=top+n;r--) for(c=left;c<=right;c++){
                uint32_t d=(uint32_t)(r*cols+c), s=(uint32_t)((r-n)*cols+c);
                vga_vram[d*4+0]=vga_vram[s*4+0]; vga_vram[d*4+1]=vga_vram[s*4+1]; }
            for(r=top;r<top+n;r++) for(c=left;c<=right;c++){
                uint32_t o=(uint32_t)(r*cols+c); vga_vram[o*4+0]=' '; vga_vram[o*4+1]=BH; }
        }
        vga_dirty = 1; break; }
    case 0x08: { uint16_t pos = BDA16(0x50); int cols = BDA16(0x4A);
        uint32_t o = (uint32_t)((pos>>8)*(cols?cols:80) + (pos&0xFF));
        AL = vga_vram[o*4+0]; AH = vga_vram[o*4+1]; break; }
    case 0x09: case 0x0A: { uint16_t pos = BDA16(0x50); int cols = BDA16(0x4A), i;
        if(!cols) cols=80;
        for(i=0;i<CX;i++){
            uint32_t o = (uint32_t)((pos>>8)*cols + (pos&0xFF) + i);
            vga_vram[o*4+0] = AL;
            if(AH==0x09) vga_vram[o*4+1] = BL;
        }
        vga_dirty=1; break; }
    case 0x0E: putch_tty(AL, BL?BL:0x07); break;
    case 0x0F: AL = (uint8_t)vga_get_mode(); AH = (uint8_t)BDA16(0x4A); BH = BDA8(0x62); break;
    case 0x10:
        switch(AL){
        case 0x00: io_r8(0x3DA); io_w8(0x3C0, BL); io_w8(0x3C0, BH); io_w8(0x3C0, 0x20); break;
        case 0x02: { int i; uint32_t p = cpu.sbase[S_ES] + DX;
                     /* 16 palette registers, then the OVERSCAN register (0x11) -
                      * not 0x10, which is the attribute mode control. */
                     io_r8(0x3DA);
                     for(i=0;i<16;i++){ io_w8(0x3C0,(uint8_t)i); io_w8(0x3C0, mem_r8(p+i)); }
                     io_w8(0x3C0, 0x11); io_w8(0x3C0, mem_r8(p+16));
                     io_w8(0x3C0, 0x20); break; }
        case 0x10: io_w8(0x3C8,(uint8_t)BX); io_w8(0x3C9,DH); io_w8(0x3C9,CH); io_w8(0x3C9,CL); break;
        case 0x12: { int i; uint32_t p = cpu.sbase[S_ES] + DX;
                     io_w8(0x3C8,(uint8_t)BX);
                     for(i=0;i<CX*3;i++) io_w8(0x3C9, mem_r8(p+i));
                     break; }
        case 0x15: { io_w8(0x3C7,(uint8_t)BX); DH=io_r8(0x3C9); CH=io_r8(0x3C9); CL=io_r8(0x3C9); break; }
        case 0x17: { int i; uint32_t p = cpu.sbase[S_ES] + DX;
                     io_w8(0x3C7,(uint8_t)BX);
                     for(i=0;i<CX*3;i++) mem_w8(p+i, io_r8(0x3C9));
                     break; }
        case 0x13: break;
        default: break;
        } break;
    case 0x11:
        if(AL==0x30){ set_sreg(S_ES, 0xF000); REG16(R_EBP) = 0xFA6E; CX = 16; DL = 24; }
        break;
    case 0x12:
        if(BL==0x10){ BH=0; BL=3; CH=0; CL=0x09; }
        else if(BL==0x30){ AL=0x12; }
        else if(BL==0x32){ AL=0x12; }
        else if(BL==0x34){ AL=0x12; }
        else if(BL==0x36){ AL=0x12; }
        break;
    case 0x13: { int i; uint32_t p = cpu.sbase[S_ES] + REG16(R_EBP);
        BDA16(0x50) = DX;
        for(i=0;i<CX;i++){
            uint8_t c  = mem_r8(p + ((AL&2)?(uint32_t)i*2:(uint32_t)i));
            uint8_t at = (AL&2) ? mem_r8(p + (uint32_t)i*2 + 1) : BL;
            putch_tty(c, at);
        }
        break; }
    case 0x1A: AL = 0x1A; BL = 0x08; BH = 0x00; break;
    case 0x1B: break;
    case 0x4F: AH = 0x01; break;                /* no VESA */
    default: break;
    }
}

/* ------------------------------------------------------------- INT 16h  */
static void bios_int16(void){
    uint16_t k;
    switch(AH){
    case 0x00: case 0x10:
        if(kbuf_get(&k)){ AX = k; }
        else {
            /* Block: rewind onto the stub and idle, but re-enable interrupts the
             * way the caller had them, or nothing could ever wake us. */
            uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
            cpu.iflag = (mem_r16(sp+4) >> 9) & 1;
            cpu.eip -= 3; cpu_no_iret(); cpu.halted = 1;
        }
        break;
    case 0x01: case 0x11:
        if(kbuf_peek(&k)){ AX = k; cpu.zf = 0; }
        else { cpu.zf = 1; AX = 0; }
        { uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
          uint16_t f = mem_r16(sp+4);
          f = (uint16_t)(cpu.zf ? (f|0x40) : (f&~0x40));
          mem_w16(sp+4, f); }
        break;
    case 0x02: case 0x12: AL = BDA8(0x17); AH = BDA8(0x18); break;
    case 0x03: break;
    case 0x05: kbuf_put(CX); AL = 0; break;
    default: AX = 0; break;
    }
}

/* ------------------------------------------------------------- INT 1Ah  */
static void bios_int1a(void){
    switch(AH){
    case 0x00: { uint32_t t = BDA16(0x6C) | ((uint32_t)BDA16(0x6E)<<16);
        CX = (uint16_t)(t>>16); DX = (uint16_t)t; AL = 0; break; }
    case 0x01: BDA16(0x6C)= DX; BDA16(0x6E)=CX; break;
    case 0x02: CH=0x12; CL=0x00; DH=0x00; cpu.cf=0; break;
    case 0x04: CH=0x19; CL=0x93; DH=0x01; DL=0x01; cpu.cf=0; break;
    default: cpu.cf = 1; break;
    }
}

/* ------------------------------------------------- other BIOS interrupts */
static void bios_int11(void){ AX = BDA16(0x10); }
static void bios_int12(void){ AX = BDA16(0x13); }
static void bios_int13(void){ AH = 0x01; cpu.cf = 1; }
static void bios_int15(void){
    switch(AH){
    case 0x88: AX = 0; cpu.cf = 0; break;                 /* extended memory size */
    case 0xC0: cpu.cf = 1; AH = 0x86; break;
    case 0x86: cpu.cf = 0; break;
    case 0x87: cpu.cf = 1; AH = 0x86; break;
    default: cpu.cf = 1; AH = 0x86; break;
    }
    { uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
      uint16_t f = mem_r16(sp+4);
      f = (uint16_t)(cpu.cf ? (f|1) : (f&~1));
      mem_w16(sp+4, f); }
}
static void bios_int1b(void){ }
static void bios_int1c(void){ }
static void bios_stub(void){ }

/* the caller's FLAGS image on the stack must reflect CF/ZF we return */
void bios_set_cf(int v){
    uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
    uint16_t f = mem_r16(sp+4);
    f = (uint16_t)(v ? (f|1) : (f&~1));
    mem_w16(sp+4, f);
    cpu.cf = v;
}
void bios_set_zf(int v){
    uint32_t sp = cpu.sbase[S_SS] + REG16(R_ESP);
    uint16_t f = mem_r16(sp+4);
    f = (uint16_t)(v ? (f|0x40) : (f&~0x40));
    mem_w16(sp+4, f);
    cpu.zf = v;
}

/* --------------------------------------------------------------- set-up */
static void put_stub(int intno, uint16_t off){
    ram[0xF0000 + off + 0] = 0x0F;
    ram[0xF0000 + off + 1] = 0xFF;
    ram[0xF0000 + off + 2] = (uint8_t)intno;
    ram[0xF0000 + off + 3] = 0xCF;
    *(uint32_t*)&ram[intno*4] = ((uint32_t)0xF000<<16) | off;
}

void bios_init(void){
    int i;
    static const uint8_t int8code[] = {
        0x1E,                    /* push ds          */
        0x50,                    /* push ax          */
        0xB8,0x40,0x00,          /* mov ax,0040h     */
        0x8E,0xD8,               /* mov ds,ax        */
        0xFF,0x06,0x6C,0x00,     /* inc word [6Ch]   */
        0x75,0x04,               /* jnz +4           */
        0xFF,0x06,0x6E,0x00,     /* inc word [6Eh]   */
        0xCD,0x1C,               /* int 1Ch          */
        0xB0,0x20,               /* mov al,20h       */
        0xE6,0x20,               /* out 20h,al       */
        0x58,                    /* pop ax           */
        0x1F,                    /* pop ds           */
        0xCF                     /* iret             */
    };

    memset(&ram[0x400], 0, 0x100);
    BDA16(0x10) = 0x0021;          /* equipment: 80x25 colour, 1 floppy, FPU  */
    BDA16(0x13) = 640;             /* KB of conventional memory               */
    BDA8(0x49)  = 0x03;
    BDA16(0x4A) = 80;
    BDA16(0x4C) = 4096;
    BDA8(0x62)  = 0;
    BDA16(0x63) = 0x03D4;
    BDA8(0x84)  = 24;
    BDA16(0x85) = 16;
    BDA8(0x87)  = 0x60;
    BDA8(0x88)  = 0x09;
    BDA16(0x1A) = 0x001E;
    BDA16(0x1C) = 0x001E;
    BDA16(0x80) = 0x001E;
    BDA16(0x82) = 0x003E;

    for(i=0;i<256;i++) cb_table[i] = bios_stub;

    /* every vector defaults to a harmless IRET stub */
    for(i=0;i<256;i++) put_stub(i, (uint16_t)(0x1000 + i*4));

    cb_table[0x09] = bios_int9;
    cb_table[0x10] = bios_int10;
    cb_table[0x11] = bios_int11;
    cb_table[0x12] = bios_int12;
    cb_table[0x13] = bios_int13;
    cb_table[0x15] = bios_int15;
    cb_table[0x16] = bios_int16;
    cb_table[0x1A] = bios_int1a;
    cb_table[0x1B] = bios_int1b;
    cb_table[0x1C] = bios_int1c;

    /* real machine code for the timer tick so hooks can chain into INT 1Ch */
    memcpy(&ram[0xF0000 + 0x0E00], int8code, sizeof(int8code));
    *(uint32_t*)&ram[0x08*4] = ((uint32_t)0xF000<<16) | 0x0E00;

    /* INT 1Ch, 1Dh..1Fh: plain IRETs / table pointers */
    ram[0xF0000 + 0x0E80] = 0xCF;
    *(uint32_t*)&ram[0x1C*4] = ((uint32_t)0xF000<<16) | 0x0E80;

    /* ROM identification */
    memcpy(&ram[0xFE000], "PFEMU BIOS (c) 2026 - not IBM", 29);
    memcpy(&ram[0xFFFF5], "01/01/93", 8);
    ram[0xFFFFE] = 0xFC;
}

void bios_tty(uint8_t c){ putch_tty(c, 0x07); }
