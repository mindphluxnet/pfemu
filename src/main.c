/* Win32 host: window, framebuffer presentation, keyboard, main emulation loop */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "pfemu.h"

extern void  emu_advance(void);
extern double emu_time;
extern double emu_ips;
extern void  dev_tick(void);
extern void  set_sreg(int s, uint16_t v);
extern int   vga_get_mode(void);
extern int   vga_force256, vga_nodbl;

uint8_t vga_font8x16[256*16];
uint8_t vga_font8x8[256*8];

int trace_level = 0;
static FILE *trace_fp = NULL;

void trc(const char *fmt, ...){
    va_list ap;
    if(!trace_level) return;
    va_start(ap, fmt);
    vfprintf(trace_fp ? trace_fp : stderr, fmt, ap);
    va_end(ap);
    if(trace_fp) fflush(trace_fp);
}

/* -------------------------------------------------------------- window  */
static HWND hwnd;
static HDC  hdc;
static BITMAPINFO bmi;
static uint32_t fb[1024*768];
static int fbw = 320, fbh = 200;
static int running = 1;
static int win_w = 960, win_h = 600;
static int integer_scale = 0;

static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l){
    switch(m){
    case WM_DESTROY: case WM_CLOSE: running = 0; PostQuitMessage(0); return 0;
    case WM_SIZE: win_w = LOWORD(l); win_h = HIWORD(l); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SYSKEYDOWN: case WM_KEYDOWN: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        /* Scroll Lock quits: F12 belongs to the game (its INT 9 maps scan
         * code 58h), and so do both shifts, both alts, both controls, the
         * arrows and space. */
        if(w == VK_SCROLL){ running = 0; return 0; }
        if(sc) kbd_key(sc | (ext?0xE000:0), 1);
        return 0; }
    case WM_SYSKEYUP: case WM_KEYUP: {
        int sc = (l >> 16) & 0xFF;
        int ext = (l >> 24) & 1;
        if(sc) kbd_key(sc | (ext?0xE000:0), 0);
        return 0; }
    }
    return DefWindowProc(h,m,w,l);
}

/* Build 8x16 and 8x8 character bitmaps from a host fixed-pitch font so text
 * mode (DOS messages, the sound-setup screen) is readable. */
static void build_fonts(void){
    HDC mdc = CreateCompatibleDC(NULL);
    HFONT f = CreateFontA(16,8,0,0,FW_NORMAL,0,0,0,OEM_CHARSET,
                          OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,
                          NONANTIALIASED_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
    BITMAPINFO bi; void *bits = NULL; HBITMAP bm;
    int c,y,x;
    memset(&bi,0,sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 8; bi.bmiHeader.biHeight = -16;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    bm = CreateDIBSection(mdc,&bi,DIB_RGB_COLORS,&bits,NULL,0);
    SelectObject(mdc,bm);
    SelectObject(mdc,f);
    SetBkColor(mdc, RGB(0,0,0));
    SetTextColor(mdc, RGB(255,255,255));
    for(c=0;c<256;c++){
        RECT r = {0,0,8,16};
        char ch = (char)c;
        uint32_t *px = (uint32_t*)bits;
        FillRect(mdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
        if(c >= 32) TextOutA(mdc,0,0,&ch,1);
        GdiFlush();
        for(y=0;y<16;y++){
            uint8_t row = 0;
            for(x=0;x<8;x++) if((px[y*8+x] & 0xFF) > 96) row |= (uint8_t)(0x80>>x);
            vga_font8x16[c*16+y] = row;
        }
        for(y=0;y<8;y++) vga_font8x8[c*8+y] = vga_font8x16[c*16+y*2];
    }
    DeleteObject(bm); DeleteObject(f); DeleteDC(mdc);
}

void plat_init(const char *title){
    WNDCLASSA wc;
    RECT r;
    memset(&wc,0,sizeof(wc));
    wc.lpfnWndProc = wndproc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = "pfemu";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    r.left=0; r.top=0; r.right=win_w; r.bottom=win_h;
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    hwnd = CreateWindowA("pfemu", title, WS_OVERLAPPEDWINDOW|WS_VISIBLE,
                         CW_USEDEFAULT, CW_USEDEFAULT,
                         r.right-r.left, r.bottom-r.top, NULL,NULL,wc.hInstance,NULL);
    hdc = GetDC(hwnd);
    memset(&bmi,0,sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    build_fonts();
}

int plat_pump(void){
    MSG msg;
    while(PeekMessage(&msg,NULL,0,0,PM_REMOVE)){
        TranslateMessage(&msg); DispatchMessage(&msg);
    }
    return running;
}

void plat_present(const uint32_t *pix, int w, int h){
    int dw = win_w, dh = win_h, dx = 0, dy = 0;
    double ar = (double)w / (double)h * (w==320 && h==200 ? 1.2 : 1.0);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h;
    if((double)dw/dh > ar){ dw = (int)(dh*ar); dx = (win_w-dw)/2; }
    else { dh = (int)(dw/ar); dy = (win_h-dh)/2; }
    if(integer_scale){ }
    if(dx>0){ RECT r={0,0,dx,win_h}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
              r.left=dx+dw; r.right=win_w; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
    if(dy>0){ RECT r={0,0,win_w,dy}; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));
              r.top=dy+dh; r.bottom=win_h; FillRect(hdc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH)); }
    SetStretchBltMode(hdc, COLORONCOLOR);
    StretchDIBits(hdc, dx,dy,dw,dh, 0,0,w,h, pix, &bmi, DIB_RGB_COLORS, SRCCOPY);
}

double plat_time(void){
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if(!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart / (double)freq.QuadPart;
}
void plat_sleep_ms(int ms){ Sleep(ms); }

static void save_ppm(const char *path, const uint32_t *pix, int w, int h){
    FILE *f = fopen(path,"wb");
    int i;
    if(!f) return;
    fprintf(f,"P6\n%d %d\n255\n",w,h);
    for(i=0;i<w*h;i++){
        uint8_t rgb[3];
        rgb[0]=(uint8_t)(pix[i]>>16); rgb[1]=(uint8_t)(pix[i]>>8); rgb[2]=(uint8_t)pix[i];
        fwrite(rgb,1,3,f);
    }
    fclose(f);
}

/* "t:scancode:updown,..." - drive the keyboard from a script for testing */
static void run_keyscript(const char *s, double now){
    static int done[256];
    const char *p = s;
    int idx = 0;
    while(*p){
        double t = atof(p);
        const char *c1 = strchr(p,':');
        if(!c1) break;
        {
            int sc = (int)strtol(c1+1,NULL,16);
            const char *c2 = strchr(c1+1,':');
            int dn = c2 ? atoi(c2+1) : 1;
            if(now >= t && !done[idx&255]){ done[idx&255]=1; kbd_key(sc,dn); }
        }
        p = strchr(p,',');
        if(!p) break;
        p++; idx++;
    }
}

static unsigned long irq_count[32];

/* ------------------------------------------------------------ main loop */
int main(int argc, char **argv){
    const char *dir = "FANTASY";
    const char *prog = "PINBALL.EXE";
    double t0, last_present = 0;
    double max_secs = 0;
    const char *shotfile = NULL;
    const char *keyscript = NULL;
    double shot_every = 0, next_shot = 0;
    double speed = 1.0;
    unsigned long mem_lo = 0;
    int shot_n = 0;
    int i;

    for(i=1;i<argc;i++){
        if(!strcmp(argv[i],"-d") && i+1<argc) dir = argv[++i];
        else if(!strcmp(argv[i],"-p") && i+1<argc) prog = argv[++i];
        else if(!strcmp(argv[i],"-t")){ trace_level = 1; trace_fp = fopen("pfemu.log","w"); }
        else if(!strcmp(argv[i],"-ips") && i+1<argc) emu_ips = atof(argv[++i]);
        else if(!strcmp(argv[i],"-secs") && i+1<argc) max_secs = atof(argv[++i]);
        else if(!strcmp(argv[i],"-shot") && i+1<argc) shotfile = argv[++i];
        else if(!strcmp(argv[i],"-keys") && i+1<argc) keyscript = argv[++i];
        else if(!strcmp(argv[i],"-shotevery") && i+1<argc) shot_every = atof(argv[++i]);
        else if(!strcmp(argv[i],"-force256")) vga_force256 = 1;
        else if(!strcmp(argv[i],"-nodbl")) vga_nodbl = 1;
        else if(!strcmp(argv[i],"-oldtiming")){ extern int vga_old_timing; vga_old_timing = 1; }
        else if(!strcmp(argv[i],"-dosdbg")){ extern int dos_log_all; dos_log_all = 1; }
        else if(!strcmp(argv[i],"-pll") && i+1<argc){ extern int pll_dbg; pll_dbg = atoi(argv[++i]); }
        else if(!strcmp(argv[i],"-xring")){ extern int x_on; x_on = 1; }
        else if(!strcmp(argv[i],"-iotrace") && i+1<argc){ extern int io_trace; io_trace = atoi(argv[++i]); }
        else if(!strcmp(argv[i],"-wav") && i+1<argc){ extern const char *wav_path; wav_path = argv[++i]; }
        else if(!strcmp(argv[i],"-snddbg")){ extern int sound_debug; sound_debug = 1; }
        else if(!strcmp(argv[i],"-nopatch")){ extern int dos_no_patch; dos_no_patch = 1; }
        else if(!strcmp(argv[i],"-mem") && i+1<argc){ mem_lo = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-trapexit")){ extern int dos_trap_exit; extern int x_on; dos_trap_exit = 1; x_on = 1; }
        else if(!strcmp(argv[i],"-intwatch") && i+1<argc){ extern int int_watch; int_watch = (int)strtol(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-trap") && i+2<argc){ extern uint32_t x_trap_lo, x_trap_hi; extern int x_on;
            x_on = 1; x_trap_lo = strtoul(argv[++i],NULL,16); x_trap_hi = strtoul(argv[++i],NULL,16); }
        else if(!strcmp(argv[i],"-speed") && i+1<argc) speed = atof(argv[++i]);
    }

    ram = (uint8_t*)calloc(RAM_SIZE,1);
    if(!ram){ fprintf(stderr,"out of memory\n"); return 1; }

    cpu_reset();
    vga_init();
    dev_init();
    bios_init();
    dos_init(dir);
    plat_init("Pinball Fantasies - pfemu");

    /* Hand-built boot: park the CPU on a HLT in ROM, then EXEC the program. */
    ram[0xFFFF0] = 0xF4;
    set_sreg(S_CS,0xF000); cpu.eip = 0xFFF0;
    set_sreg(S_SS,0x0050); REG16(R_ESP) = 0x0100;
    set_sreg(S_DS,0x0000); set_sreg(S_ES,0x0000);
    cpu.iflag = 1;
    /* fabricate an IRET frame so a terminating child has something to return to */
    {
        uint32_t sp;
        REG16(R_ESP) -= 6;
        sp = cpu.sbase[S_SS] + REG16(R_ESP);
        mem_w16(sp+0, 0xFFF0);
        mem_w16(sp+2, 0xF000);
        mem_w16(sp+4, 0x0202);
    }
    if(dos_exec(prog, 0, 0, 0, 0) != 0){
        fprintf(stderr,"could not load %s from %s\n", prog, dir);
        return 1;
    }

    t0 = plat_time();
    while(plat_pump() && !cpu.shutdown){
        double wall = plat_time() - t0;
        double real = wall * speed;
        if(max_secs > 0 && wall > max_secs) break;
        if(keyscript) run_keyscript(keyscript, real);
        int guard = 0;
        while(emu_time < real && !cpu.shutdown && guard < 40000){
            int n;
            if(cpu.halted){
                /* idle: jump the clock forward to the next scheduled event */
                cpu.cycles += (uint64_t)(emu_ips / 10000.0);
                dev_tick();
            } else {
                /* Run up to 64 instructions, but never past the next timer
                 * deadline: IRQ0 has to land on the instruction it is due on,
                 * not up to 64 instructions later. */
                uint64_t dl = dev_next_deadline();
                int lim = 64;
                if(dl > cpu.cycles && dl - cpu.cycles < 64) lim = (int)(dl - cpu.cycles);
                if(lim < 1) lim = 1;
                for(n=0;n<lim;n++) cpu_step();
                dev_tick();
            }
            if(cpu.iflag){
                int v = pic_pending();
                if(v >= 0){ irq_count[v&31]++; cpu_interrupt(v, 0); }
            }
            guard++;
        }
        if(emu_time < real - 0.25*speed) { t0 = plat_time() - emu_time/speed; }  /* fell behind */

        if(plat_time() - last_present > 1.0/60.0){
            last_present = plat_time();
            vga_render(fb, &fbw, &fbh);
            plat_present(fb, fbw, fbh);
            if(shot_every > 0 && real >= next_shot){
                char nm[64];
                next_shot = real + shot_every;
                sprintf(nm, "seq%03d.ppm", shot_n++);
                save_ppm(nm, fb, fbw, fbh);
            }
        }
        plat_sleep_ms(1);
    }

    vga_render(fb,&fbw,&fbh);
    plat_present(fb,fbw,fbh);
    if(shotfile) save_ppm(shotfile, fb, fbw, fbh);
    { extern void vga_dump(void); vga_dump(); }
    { extern unsigned long st1_calls, st1_bit0, st1_bit3;
      printf("[pfemu] 3DA reads=%lu  bit0(blank)=%lu  bit3(vsync)=%lu\n",
             st1_calls, st1_bit0, st1_bit3); }
    { extern void st1_report(void); st1_report(); }
    { extern unsigned long vsync_edges;
      printf("[pfemu] vsync edges seen = %lu (%.1f/s)\n",
             vsync_edges, vsync_edges/(emu_time>0?emu_time:1)); }
    { extern double vga_frame_hz(void); printf("[pfemu] CRT refresh = %.2f Hz\n", vga_frame_hz()); }
    { extern unsigned long vga_startaddr_changes;
      printf("[pfemu] page flips=%lu (%.1f/s of emulated time)\n",
             vga_startaddr_changes, vga_startaddr_changes/(emu_time>0?emu_time:1)); }
    { extern unsigned long kbd_port60_reads; extern uint8_t pic_imr(void);
      printf("[pfemu] port60 reads=%lu  master IMR=%02X\n", kbd_port60_reads, pic_imr()); }
    printf("[pfemu] irqs: int8=%lu int9=%lu ticks=%u iflag=%d halted=%d cs:ip=%04X:%04X\n",
           irq_count[8], irq_count[9], *(unsigned short*)&ram[0x46C],
           (int)cpu.iflag, cpu.halted, cpu.sreg[S_CS], (unsigned)cpu.eip);
    if(mem_lo){ int k; printf("[mem] %05lX:\n", mem_lo);
        for(k=0;k<256;k++){ if((k&15)==0) printf("  %05lX:", mem_lo+k);
            printf(" %02X", ram[(mem_lo+k)&0xFFFFF]); if((k&15)==15) printf("\n"); } }
    { extern void dev_state_dump(void); dev_state_dump(); }
    { extern uint32_t x_ring[]; extern uint16_t x_cs[], x_ip[];
      extern unsigned x_pos; extern int x_on;
      if(x_on){
          /* print the ring oldest-first, collapsing straight-line runs so the
           * interesting thing - the jump that left real code - stands out */
          unsigned n = x_pos < 8192 ? x_pos : 8192, k;
          uint32_t prev = 0xFFFFFFFFu;
          printf("[xring] last %u instruction addresses (jumps only):\n", n);
          for(k=0;k<n;k++){
              unsigned idx = (x_pos - n + k) & 8191;
              uint32_t lin = x_ring[idx];
              if(prev == 0xFFFFFFFFu || lin < prev || lin > prev + 15)
                  printf("  %04X:%04X  lin=%05X\n", x_cs[idx], x_ip[idx], lin);
              prev = lin;
          }
      } }
    printf("[pfemu] stopped: emu_time=%.3fs instructions=%llu mode=%02Xh %dx%d\n",
           emu_time, (unsigned long long)cpu.cycles, vga_get_mode(), fbw, fbh);
    /* hex window around the final CS:IP - enough to disassemble whatever loop
     * the guest was spinning in when we stopped */
    { uint32_t lin = cpu.sbase[S_CS] + cpu.eip; uint32_t s = lin > 0x60 ? lin-0x60 : 0; int i;
      printf("[pfemu] code @ linear %05X (cs:ip %04X:%04X):\n", (unsigned)lin,
             cpu.sreg[S_CS], (unsigned)cpu.eip);
      for(i=0;i<0xC0;i++){
          if((i&15)==0) printf("  %05X:", (unsigned)(s+i));
          printf(" %02X", ram[(s+i)&0xFFFFF]);
          if((i&15)==15) printf("\n");
      } }
    { extern void wav_close(void); extern void plat_audio_close(void);
      wav_close(); plat_audio_close(); }
    if(trace_fp) fclose(trace_fp);
    return 0;
}
