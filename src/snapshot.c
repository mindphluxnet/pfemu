/* Mid-table savestates (single slot per install, Play mode only).
 *
 * A .pfs file is one session frozen mid-run: the release identity, CPU,
 * low RAM, and every subsystem's guest-visible state, sealed with FNV-1a.
 * Saving or loading while recording/replaying is refused (like the
 * trainer): loading rewinds cpu.cycles, which would unsort the .pfr event
 * stream, and saving one would checkpoint a clock the replay owns.
 *
 * Layout: magic "PFEMU-SNAP 1\n", then sections of tag[8] + u32le len +
 * payload, in fixed order, ending with a HASH section whose 8-byte payload
 * is FNV-1a over every file byte before the HASH tag.  Unknown sections
 * are skipped (forward tolerance); missing ones fail the load.  Same-build
 * only: raw struct dumps are guarded by the magic, never by version
 * negotiation.
 *
 * Two honest limitations, both warned about, neither silent:
 *  - PFEMU-STATE/ lives outside the snapshot (host files).  The overlay
 *    hash travels in RELEASE and a drift only warns, like replay.
 *  - an in-flight FindFirst iteration is reset on load (boot-time ops in
 *    practice); open handles are re-resolved by name and seeked back.
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <direct.h>
#include "pfemu.h"

extern double emu_time;
extern double emu_ips;
extern double emu_inv_ips;

static char snap_err[512] = "";
const char *snapshot_error(void){ return snap_err[0] ? snap_err : NULL; }

/* ------------------------------------------------------- section buffers */
/* Raised when a section buffer could not grow.  snapshot_save() checks it
 * before sealing: a short section would otherwise be written out, hashed,
 * and pass its own integrity check on load. */
static int snap_oom = 0;
static void wgrow(SnapW *w, size_t n){
    if(w->len + n > w->cap){
        size_t nc = w->cap ? w->cap * 2 : 1024;
        uint8_t *nb;
        while(nc < w->len + n) nc *= 2;
        nb = (uint8_t*)realloc(w->buf, nc);
        /* Drop the buffer rather than keeping it with cap = 0: wput()'s
         * guard tests w->buf, so a non-NULL pointer with no capacity would
         * let the next memcpy run off the end of the old allocation. */
        if(!nb){ free(w->buf); w->buf = NULL; w->len = 0; w->cap = 0; snap_oom = 1; return; }
        w->buf = nb; w->cap = nc;
    }
}
static void wput(SnapW *w, const void *p, size_t n){
    wgrow(w, n);
    if(!w->buf){ w->len = 0; return; }
    memcpy(w->buf + w->len, p, n);
    w->len += n;
}
void snap_w_u8(SnapW *w, uint8_t v){ wput(w, &v, 1); }
void snap_w_u16(SnapW *w, uint16_t v){
    uint8_t b[2]; b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8); wput(w, b, 2);
}
void snap_w_u32(SnapW *w, uint32_t v){
    uint8_t b[4]; b[0]=(uint8_t)v; b[1]=(uint8_t)(v>>8);
    b[2]=(uint8_t)(v>>16); b[3]=(uint8_t)(v>>24); wput(w, b, 4);
}
void snap_w_u64(SnapW *w, uint64_t v){
    uint8_t b[8]; int i;
    for(i=0;i<8;i++) b[i] = (uint8_t)(v >> (i*8));
    wput(w, b, 8);
}
void snap_w_dbl(SnapW *w, double v){ uint64_t u; memcpy(&u, &v, 8); snap_w_u64(w, u); }
void snap_w_bytes(SnapW *w, const void *p, size_t n){ if(n) wput(w, p, n); }

static uint8_t rget(SnapR *r){
    if(r->pos >= r->n){ r->err = 1; return 0; }
    return r->p[r->pos++];
}
uint8_t snap_r_u8(SnapR *r){ return rget(r); }
uint16_t snap_r_u16(SnapR *r){ uint8_t a = rget(r), b = rget(r); return (uint16_t)(a | (b << 8)); }
uint32_t snap_r_u32(SnapR *r){
    uint32_t v = rget(r); v |= (uint32_t)rget(r) << 8;
    v |= (uint32_t)rget(r) << 16; v |= (uint32_t)rget(r) << 24; return v;
}
uint64_t snap_r_u64(SnapR *r){
    uint64_t v = 0; int i;
    for(i=0;i<8;i++) v |= (uint64_t)rget(r) << (i*8);
    return v;
}
double snap_r_dbl(SnapR *r){ uint64_t u = snap_r_u64(r); double v; memcpy(&v, &u, 8); return v; }
void snap_r_bytes(SnapR *r, void *dst, size_t n){
    if(r->pos + n > r->n){ r->err = 1; return; }
    memcpy(dst, r->p + r->pos, n);
    r->pos += n;
}

/* ------------------------------------------------------------- slot path */
static uint64_t fnv1a(const void *p, size_t n, uint64_t h);
void snapshot_slot_path(const char *dir, char *dst, size_t n){
    char safe[64];
    size_t i;
    uint64_t h;
    for(i=0;i<sizeof(safe)-1 && dir && dir[i];i++){
        char c = dir[i];
        safe[i] = (c=='\\'||c=='/'||c==':'||c==' ') ? '_' : c;
    }
    safe[i] = 0;
    if(!safe[0]) snprintf(safe, sizeof(safe), "GAME");
    /* The readable part is truncated to keep the name short, so two
     * installs sharing a long prefix would land on one slot and
     * silently overwrite each other.  A short hash of the full path
     * separates them; the identity check on load would catch a
     * collision anyway, but as a refusal rather than two good slots. */
    h = fnv1a(dir ? dir : "", dir ? strlen(dir) : 0, 1469598103934665603ULL);
    snprintf(dst, n, "savestates\\%s-%08lx.pfs", safe,
             (unsigned long)(h & 0xFFFFFFFFu));
}

static void mkdir_parents(const char *path){
    char tmp[600];
    size_t i, n;
    snprintf(tmp, sizeof(tmp), "%s", path);
    n = strlen(tmp);
    for(i=0;i<n;i++){
        if(tmp[i]=='/' || tmp[i]=='\\'){
            char save = tmp[i];
            tmp[i] = 0;
            if(i > 0 && tmp[i-1] != ':') _mkdir(tmp);
            tmp[i] = save;
        }
    }
}

static uint64_t fnv1a(const void *p, size_t n, uint64_t h){
    const uint8_t *b = (const uint8_t*)p;
    size_t i;
    for(i=0;i<n;i++){ h ^= b[i]; h *= 1099511628211ULL; }
    return h;
}

/* ------------------------------------------------------------------ save */
int snapshot_save(const char *path, const RelResult *rel){
    FILE *f;
    SnapW s;
    int i;
    uint64_t h = 1469598103934665603ULL;
    snap_err[0] = 0;
    snap_oom = 0;
    if(replay_is_recording() || replay_is_replaying()){
        snprintf(snap_err, sizeof(snap_err),
                 "[snapshot] refused: cannot snapshot while recording or replaying.");
        return -1;
    }
    if(fantasies_trainer_enabled()){
        snprintf(snap_err, sizeof(snap_err),
                 "[snapshot] refused: the trainer is on (PFEMU-STATE/pfemu.cfg).");
        return -1;
    }
    if(!rel || !release_runnable(rel)){
        snprintf(snap_err, sizeof(snap_err),
                 "[snapshot] refused: '%s' is not a recognised release.",
                 rel ? rel->dir : "?");
        return -1;
    }
    if(dos_done || cpu.shutdown){
        snprintf(snap_err, sizeof(snap_err),
                 "[snapshot] refused: the session already ended.");
        return -1;
    }
    mkdir_parents(path);
    f = fopen(path, "wb");
    if(!f){
        snprintf(snap_err, sizeof(snap_err), "[snapshot] cannot write '%s'.", path);
        return -1;
    }
    {
        static const char magic[] = "PFEMU-SNAP 1\n";
        fwrite(magic, 1, sizeof(magic)-1, f);
        h = fnv1a(magic, sizeof(magic)-1, h);
    }
#define SECTION(tag, body) do { \
        char stag[8]; uint8_t lb[4]; uint32_t llen; \
        memset(&s, 0, sizeof(s)); \
        do body while(0); \
        memset(stag, 0, sizeof(stag)); \
        memcpy(stag, tag, strlen(tag) > 8 ? 8 : strlen(tag)); \
        fwrite(stag, 1, 8, f); h = fnv1a(stag, 8, h); \
        llen = (uint32_t)s.len; \
        lb[0]=(uint8_t)llen; lb[1]=(uint8_t)(llen>>8); \
        lb[2]=(uint8_t)(llen>>16); lb[3]=(uint8_t)(llen>>24); \
        fwrite(lb, 1, 4, f); h = fnv1a(lb, 4, h); \
        if(s.len){ fwrite(s.buf, 1, s.len, f); h = fnv1a(s.buf, s.len, h); } \
        free(s.buf); \
    } while(0)
    /* RELEASE: identity is release_id + code vector, like replay. */
    SECTION("RELEASE ", {
        char ov[32] = {0};
        char rid[32] = {0};
        snprintf(rid, sizeof(rid), "%s", rel->rel->id);
        snap_w_bytes(&s, rid, 32);
        snap_w_u32(&s, (uint32_t)rel->ncode);
        for(i=0;i<rel->ncode;i++){
            snap_w_bytes(&s, rel->code_names[i], 16);
            snap_w_u8(&s, rel->code_have[i] ? 1 : 0);
            snap_w_u32(&s, rel->code_size[i]);
            snap_w_bytes(&s, rel->code_sha[i], 32);
        }
        replay_overlay_hash(rel->dir, ov);
        snap_w_bytes(&s, ov, 32);
    });
    /* CPU: the struct holds no pointers (regs/sregs/bases/eip/unpacked
     * flags/halted/cycles/shutdown), so a raw dump is exact same-build. */
    SECTION("CPU     ", {
        snap_w_bytes(&s, &cpu, sizeof(cpu));
        snap_w_u32(&s, a20_mask);
    });
    /* RAMLOW: linear 0x00000-0xFFFFF covers IVT/BDA/MCB/PSPs/programs and
     * the ROM stubs; everything above is untouched (cpu.c/vga.c ignore
     * writes at 0xC0000+, nothing allocates past 0x9FC00). */
    SECTION("RAMLOW  ", { snap_w_bytes(&s, ram, 0x100000); });
    SECTION("DEV     ", { dev_save_state(&s); });
    SECTION("VGAREGS ", { vga_save_state(&s); });
    SECTION("VRAM    ", { snap_w_bytes(&s, vga_vram, sizeof(vga_vram)); });
    SECTION("DOS     ", { dos_save_state(&s); });
    SECTION("FILES   ", { dos_save_handles(&s); });
    SECTION("SOUND   ", { sound_save_state(&s); });
    SECTION("FANT    ", { fantasies_save_state(&s); });
    if(snap_oom){
        fclose(f);
        remove(path);
        snprintf(snap_err, sizeof(snap_err),
                 "[snapshot] out of memory: '%s' not written.", path);
        return -1;
    }
    {
        /* HASH over every byte before this section's tag. */
        uint8_t hb[8]; int k;
        char stag[8]; uint8_t lb[4];
        memset(stag, 0, sizeof(stag)); memcpy(stag, "HASH    ", 8);
        fwrite(stag, 1, 8, f);
        lb[0]=8; lb[1]=lb[2]=lb[3]=0;
        fwrite(lb, 1, 4, f);
        /* The length prefix is part of the file but was never hashed for
         * other sections either (only tags+payloads are) - hash the tag
         * here the same way, then the payload closes it. */
        h = fnv1a(stag, 8, h);
        h = fnv1a(lb, 4, h);
        for(k=0;k<8;k++) hb[k] = (uint8_t)(h >> (k*8));
        fwrite(hb, 1, 8, f);
    }
    fclose(f);
    fprintf(stderr, "[snapshot] saved '%s' (%.3fs / %llu cycles)\n",
            path, emu_now(), (unsigned long long)cpu.cycles);
    return 0;
#undef SECTION
}

/* ------------------------------------------------------------------ load */
/* Two passes.  The first validates only - magic, section bounds, the FNV
 * seal, completeness, the fixed-size sections and the release identity -
 * and writes no emulator state at all, so a corrupt, truncated, foreign or
 * wrong-install file leaves the running session exactly as it was.  That
 * matters most for F8: a refusal used to be reported over a machine that
 * had already been half-overwritten by the sections parsed before the bad
 * one.  The second pass applies.  By then the seal has verified, so the
 * bytes are what this build wrote; an apply failure means a struct moved
 * under a file still carrying the same magic, which is unrecoverable and
 * now says so instead of posing as a clean refusal. */
enum { SEC_RELEASE = 0, SEC_CPU, SEC_RAMLOW, SEC_DEV, SEC_VGAREGS,
       SEC_VRAM, SEC_DOS, SEC_FILES, SEC_SOUND, SEC_FANT, SEC_N };
static const char sec_tags[SEC_N][9] = {
    "RELEASE ", "CPU     ", "RAMLOW  ", "DEV     ", "VGAREGS ",
    "VRAM    ", "DOS     ", "FILES   ", "SOUND   ", "FANT    "
};

int snapshot_load(const char *path, const RelResult *rel, char *why, size_t nwhy){
    static const char magic[] = "PFEMU-SNAP 1\n";
    FILE *f;
    long len = 0;
    uint8_t *data = NULL;
    size_t pos = 0, hash_off = 0;
    size_t soff[SEC_N];
    uint32_t ssz[SEC_N];
    int have[SEC_N];
    char file_rel[32] = "";
    uint64_t hh;
    SnapR r;
    int i, k;
#define LOAD_REFUSE(...) do { if(why) snprintf(why, nwhy, __VA_ARGS__); \
        snprintf(snap_err, sizeof(snap_err), "%s", why ? why : "refused"); \
        free(data); return -1; } while(0)
#define APPLY_FAIL(what) LOAD_REFUSE("[snapshot] '%s': cannot apply the %s section." \
        "  The file passed its seal, so this build no longer matches the one that" \
        " wrote it - the session is inconsistent and should be restarted.", path, what)
#define SEC_OPEN(ix) do { r.p = data + soff[ix]; r.n = ssz[ix]; \
                          r.pos = 0; r.err = 0; } while(0)
    snap_err[0] = 0;
    for(i=0;i<SEC_N;i++){ soff[i] = 0; ssz[i] = 0; have[i] = 0; }
    if(replay_is_recording() || replay_is_replaying())
        LOAD_REFUSE("[snapshot] refused: cannot load while recording or replaying.");
    if(fantasies_trainer_enabled())
        LOAD_REFUSE("[snapshot] refused: the trainer is on.");
    f = fopen(path, "rb");
    if(!f) LOAD_REFUSE("[snapshot] cannot open '%s'.", path);
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    if(len <= 0 || len > (64<<20)){ fclose(f); LOAD_REFUSE("[snapshot] bad size '%s'.", path); }
    data = (uint8_t*)malloc((size_t)len);
    if(!data){ fclose(f); LOAD_REFUSE("[snapshot] out of memory."); }
    if(fread(data, 1, (size_t)len, f) != (size_t)len){
        fclose(f); LOAD_REFUSE("[snapshot] cannot read '%s'.", path);
    }
    fclose(f);
    if((size_t)len < sizeof(magic)-1 || memcmp(data, magic, sizeof(magic)-1))
        LOAD_REFUSE("[snapshot] '%s': bad magic (not a pfemu snapshot).", path);

    /* ---- pass 1: validate ---- */
    pos = sizeof(magic)-1;
    while(pos + 12 <= (size_t)len){
        char tag[9];
        uint32_t slen;
        memcpy(tag, data+pos, 8); tag[8] = 0;
        slen = (uint32_t)data[pos+8] | ((uint32_t)data[pos+9] << 8) |
               ((uint32_t)data[pos+10] << 16) | ((uint32_t)data[pos+11] << 24);
        pos += 12;
        if(pos + slen > (size_t)len)
            LOAD_REFUSE("[snapshot] '%s': truncated section '%.8s'.", path, tag);
        if(!memcmp(tag, "HASH    ", 8)){
            if(slen != 8) LOAD_REFUSE("[snapshot] '%s': bad hash section.", path);
            hash_off = pos - 12;
            break;
        }
        for(i=0;i<SEC_N;i++)
            if(!memcmp(tag, sec_tags[i], 8)){
                soff[i] = pos; ssz[i] = slen; have[i] = 1; break;
            }
        /* Unknown sections are skipped: forward tolerance. */
        pos += slen;
    }
    if(!hash_off) LOAD_REFUSE("[snapshot] '%s': no integrity seal.", path);
    hh = fnv1a(data, hash_off + 12, 1469598103934665603ULL);
    {
        uint64_t want = 0;
        for(i=0;i<8;i++) want |= (uint64_t)data[hash_off+12+i] << (i*8);
        if(want != hh) LOAD_REFUSE("[snapshot] '%s': integrity check failed.", path);
    }
    for(i=0;i<SEC_N;i++)
        if(!have[i])
            LOAD_REFUSE("[snapshot] '%s': incomplete file (no '%s' section).",
                        path, sec_tags[i]);
    if(ssz[SEC_RAMLOW] != 0x100000)
        LOAD_REFUSE("[snapshot] '%s': bad RAM section.", path);
    if(ssz[SEC_VRAM] != sizeof(vga_vram))
        LOAD_REFUSE("[snapshot] '%s': bad VRAM section.", path);
    /* Identity, still read-only: a file from another release or another
     * build of the programs is refused before anything is applied. */
    {
        char ov[32] = {0}, cur[32] = {0};
        uint32_t ncode;
        SEC_OPEN(SEC_RELEASE);
        snap_r_bytes(&r, file_rel, 32);
        file_rel[31] = 0;
        ncode = snap_r_u32(&r);
        if(r.err || ncode > 5) LOAD_REFUSE("[snapshot] '%s': bad identity.", path);
        if(!rel || !release_runnable(rel))
            LOAD_REFUSE("[snapshot] refused: no runnable install for '%s'.", path);
        if(_stricmp(file_rel, rel->rel->id))
            LOAD_REFUSE("[snapshot] refused: file is '%s', install is '%s'.",
                        file_rel, rel->rel->id);
        if((int)ncode != rel->ncode)
            LOAD_REFUSE("[snapshot] refused: code vector length differs.");
        for(k=0;k<(int)ncode;k++){
            char nm[17];
            int hv; uint32_t sz; uint8_t sha[32];
            snap_r_bytes(&r, nm, 16);
            nm[16] = 0;              /* the field is not NUL-terminated when full */
            hv = snap_r_u8(&r); sz = snap_r_u32(&r);
            snap_r_bytes(&r, sha, 32);
            if(r.err) LOAD_REFUSE("[snapshot] '%s': bad identity.", path);
            if(_stricmp(nm, rel->code_names[k]) || hv != rel->code_have[k] ||
               (hv && (sz != rel->code_size[k] || memcmp(sha, rel->code_sha[k], 32))))
                LOAD_REFUSE("[snapshot] refused: program '%s' differs.", nm);
        }
        snap_r_bytes(&r, ov, 32);
        if(r.err) LOAD_REFUSE("[snapshot] '%s': bad identity.", path);
        replay_overlay_hash(rel->dir, cur);
        if(memcmp(cur, ov, 32))
            fprintf(stderr, "[snapshot] warning: PFEMU-STATE/ differs from the save"
                            " (same inputs + different overlay can diverge).\n");
    }

    /* ---- pass 2: apply.  DOS before FILES: reopening the handles needs
     * the restored writedir. ---- */
    SEC_OPEN(SEC_CPU);
    {
        CPU c; uint32_t a20;
        snap_r_bytes(&r, &c, sizeof(c));
        a20 = snap_r_u32(&r);
        if(r.err) APPLY_FAIL("CPU");
        c.shutdown = 0;
        cpu = c; a20_mask = a20;
    }
    SEC_OPEN(SEC_RAMLOW);
    snap_r_bytes(&r, ram, 0x100000);
    if(r.err) APPLY_FAIL("RAM");
    SEC_OPEN(SEC_DEV);
    if(dev_load_state(&r)) APPLY_FAIL("DEV");
    SEC_OPEN(SEC_VGAREGS);
    if(vga_load_state(&r)) APPLY_FAIL("VGA");
    SEC_OPEN(SEC_VRAM);
    snap_r_bytes(&r, vga_vram, sizeof(vga_vram));
    if(r.err) APPLY_FAIL("VRAM");
    SEC_OPEN(SEC_DOS);
    if(dos_load_state(&r)) APPLY_FAIL("DOS");
    SEC_OPEN(SEC_FILES);
    if(dos_load_handles(&r)) APPLY_FAIL("FILES");
    SEC_OPEN(SEC_SOUND);
    if(sound_load_state(&r)) APPLY_FAIL("SOUND");
    SEC_OPEN(SEC_FANT);
    if(fantasies_load_state(&r)) APPLY_FAIL("FANT");

    free(data);
    fprintf(stderr, "[snapshot] loaded '%s' (%s, %.3fs / %llu cycles)\n",
            path, file_rel, emu_now(), (unsigned long long)cpu.cycles);
    return 0;
#undef SEC_OPEN
#undef APPLY_FAIL
#undef LOAD_REFUSE
}
