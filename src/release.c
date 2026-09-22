/* Release identity for Pinball Fantasies installations.
 *
 * Four releases of the game are known (docs/RELEASES.md
 * has the full inventory and the reasoning behind this design), and they do
 * not agree on the things pfemu has to know before it boots anything:
 *
 *   floppy      PINBALL.EXE   intro options at DS:49A3
 *   power_pack  PF.EXE        intro options at DS:4846
 *   deluxe      PINBALL.EXE   intro options at DS:48D7, plus a cd.nfo check
 *
 * The launcher used to infer all of that from two hardcoded directory names
 * and one hardcoded boot filename.  That cannot work: Power Pack renamed the
 * launcher, and its PF.EXE is byte-identical to the floppy PINBALL.EXE, so
 * neither the directory name nor the boot filename tells you which build's
 * memory layout you are about to poke.  Get it wrong and the six option
 * bytes land in the middle of the credits strings.
 *
 * So identity comes from content: SHA-256 over the programs a distribution
 * ships, matched as a whole vector against the table below.  The intro alone
 * is already unique across all four, but checking every program is what
 * separates a coherent installation from one assembled out of parts - and a
 * mixture is reported as a mixture, never resolved by picking whichever
 * release matches the most files.  Guessing there would mean applying one
 * intro's memory layout to a different intro's code.
 *
 * Which programs those are is not a constant either.  The full game ships
 * INTRO.PRG and TABLE1-4.PRG; the 1993 five-minute demo ships DEMO.PRG and
 * PLAND.PRG - an intro and the one table it came with, both renamed.  So a
 * release names the CodeLayout it uses, a directory is assigned a layout by
 * which anchor program is in it, and a directory holding two anchors is
 * ambiguous rather than resolved in favour of either.
 *
 * What is deliberately NOT part of identity:
 *   - the directory name (the whole point of this file);
 *   - the boot filename (renamed in Power Pack);
 *   - timestamps (rewritten by every extraction and repack);
 *   - INTRO.MOD's full hash - the game writes its manual-check sentinel over
 *     the last two bytes, so a played installation legitimately differs.
 *     Its length and its first 252,868 bytes are checked instead;
 *   - SOUND.CFG, PINBALL.CFG, *.HI, PFEMU-STATE/ and anything else the game
 *     or this emulator writes.
 *
 * Unknown extra files (PINBALL.BAT, 21STINFO.DAT, readmes, logs) never make a
 * known installation fail; they are listed in the report and ignored.
 */
#include "compat.h"
#include <stdarg.h>
#include "pfemu.h"

/* ---------------------------------------------------------------- SHA-256 */
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; int n; } Sha256;

static const uint32_t sha_k[64] = {
0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2 };

#define ROR32(x,n) (((x)>>(n)) | ((x)<<(32-(n))))

static void sha_block(Sha256 *s, const uint8_t *p){
    uint32_t w[64], a,b,c,d,e,f,g,h, t1, t2;
    int i;
    for(i=0;i<16;i++)
        w[i] = ((uint32_t)p[i*4]<<24)|((uint32_t)p[i*4+1]<<16)|
               ((uint32_t)p[i*4+2]<<8)|(uint32_t)p[i*4+3];
    for(i=16;i<64;i++){
        uint32_t s0 = ROR32(w[i-15],7) ^ ROR32(w[i-15],18) ^ (w[i-15]>>3);
        uint32_t s1 = ROR32(w[i-2],17) ^ ROR32(w[i-2],19) ^ (w[i-2]>>10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    a=s->h[0]; b=s->h[1]; c=s->h[2]; d=s->h[3];
    e=s->h[4]; f=s->h[5]; g=s->h[6]; h=s->h[7];
    for(i=0;i<64;i++){
        uint32_t S1 = ROR32(e,6) ^ ROR32(e,11) ^ ROR32(e,25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t S0 = ROR32(a,2) ^ ROR32(a,13) ^ ROR32(a,22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        t1 = h + S1 + ch + sha_k[i] + w[i];
        t2 = S0 + mj;
        h=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
    }
    s->h[0]+=a; s->h[1]+=b; s->h[2]+=c; s->h[3]+=d;
    s->h[4]+=e; s->h[5]+=f; s->h[6]+=g; s->h[7]+=h;
}

static void sha_init(Sha256 *s){
    s->h[0]=0x6a09e667; s->h[1]=0xbb67ae85; s->h[2]=0x3c6ef372; s->h[3]=0xa54ff53a;
    s->h[4]=0x510e527f; s->h[5]=0x9b05688c; s->h[6]=0x1f83d9ab; s->h[7]=0x5be0cd19;
    s->len = 0; s->n = 0;
}

static void sha_update(Sha256 *s, const uint8_t *p, size_t n){
    s->len += n;
    while(n){
        size_t take = 64 - (size_t)s->n;
        if(take > n) take = n;
        memcpy(s->buf + s->n, p, take);
        s->n += (int)take; p += take; n -= take;
        if(s->n == 64){ sha_block(s, s->buf); s->n = 0; }
    }
}

static void sha_final(Sha256 *s, uint8_t out[32]){
    uint64_t bits = s->len * 8;
    uint8_t pad[72];
    int i, padn;
    pad[0] = 0x80;
    padn = 1;
    while(((s->n + padn) & 63) != 56) pad[padn++] = 0;
    for(i=0;i<8;i++) pad[padn+i] = (uint8_t)(bits >> (56 - i*8));
    sha_update(s, pad, (size_t)padn + 8);
    for(i=0;i<8;i++){
        out[i*4]   = (uint8_t)(s->h[i]>>24); out[i*4+1] = (uint8_t)(s->h[i]>>16);
        out[i*4+2] = (uint8_t)(s->h[i]>>8);  out[i*4+3] = (uint8_t)s->h[i];
    }
}

/* Hash a file, optionally only its first prefix_len bytes (INTRO.MOD).
 * Returns 0 on success, and always reports the file's true full size. */
static int hash_file(const char *path, uint32_t prefix_len,
                     uint8_t out[32], uint32_t *size_out){
    static uint8_t buf[65536];
    Sha256 s;
    FILE *f = fopen(path, "rb");
    uint32_t total = 0, want;
    size_t got;
    if(!f) return -1;
    sha_init(&s);
    want = prefix_len ? prefix_len : 0xFFFFFFFFu;
    while((got = fread(buf, 1, sizeof(buf), f)) > 0){
        uint32_t use = (uint32_t)got;
        if(total < want){
            if(use > want - total) use = want - total;
            sha_update(&s, buf, use);
        }
        total += (uint32_t)got;
    }
    fclose(f);
    sha_final(&s, out);
    if(size_out) *size_out = total;
    return 0;
}

static void hex32(const uint8_t d[32], char out[65]){
    static const char *x = "0123456789abcdef";
    int i;
    for(i=0;i<32;i++){ out[i*2] = x[d[i]>>4]; out[i*2+1] = x[d[i]&15]; }
    out[64] = 0;
}

/* ------------------------------------------------------- release database */
/* Generated from the three collected installations and cross-checked against
 * every hash printed in docs/RELEASES.md.  Sizes are decimal bytes; prefix is
 * nonzero only where the hash deliberately covers less than the whole file. */
#include "reltable.h"

/* The two program layouts collected so far.  Order matters only for the
 * report; a directory is assigned whichever layout's anchor it actually
 * holds, and holding both anchors is an error rather than a preference. */
static const CodeLayout layout_full = {
    "full", { "INTRO.PRG", "TABLE1.PRG", "TABLE2.PRG", "TABLE3.PRG", "TABLE4.PRG" }, 5
};
static const CodeLayout layout_demo = {
    "demo", { "DEMO.PRG", "PLAND.PRG" }, 2
};
static const CodeLayout *const layouts[] = { &layout_full, &layout_demo };
#define NLAYOUTS ((int)(sizeof(layouts)/sizeof(layouts[0])))

/* Per-release runtime metadata.  cfg_buf is the intro's six-byte options
 * structure (see fantasies.c); it is normally re-derived from the loaded
 * image by signature, and this value is the cross-check that the derivation
 * found the right thing.  Zero is not "unknown" but "this build has none":
 * the demo's intro carries neither the F5 options menu nor the PINBALL.CFG
 * that feeds it, so a signature match there would be a contradiction, and
 * fantasies.c treats it as one.  scroll_clobber / opt_validate say which of
 * the two known "no config file on disk" fallbacks that intro build uses, so
 * a patch that does not match is a real warning in one release and an
 * expected absence in another; a build with no options structure has neither.
 * cd_marker is Deluxe's boot-time CD-presence check. */
static const Release releases[] = {
  { "floppy", "Pinball Fantasies (floppy release)", "PINBALL.EXE", &layout_full,
    0x49A3, 1, 0, 0, files_floppy, (int)(sizeof(files_floppy)/sizeof(RelFile)) },
  { "power_pack", "Pinball Power Pack (1996)", "PF.EXE", &layout_full,
    0x4846, 0, 1, 0, files_power_pack, (int)(sizeof(files_power_pack)/sizeof(RelFile)) },
  { "deluxe", "Pinball Fantasies Deluxe CD-ROM (1995)", "PINBALL.EXE", &layout_full,
    0x48D7, 0, 1, 1, files_deluxe, (int)(sizeof(files_deluxe)/sizeof(RelFile)) },
  { "demo", "Pinball Fantasies 5 Min Demo (1993)", "PFDEMO.EXE", &layout_demo,
    0, 0, 0, 0, files_demo, (int)(sizeof(files_demo)/sizeof(RelFile)) },
};
#define NRELEASES ((int)(sizeof(releases)/sizeof(releases[0])))

int release_prog_slot(const Release *r, const char *base){
    int i;
    if(!r || !r->layout) return -1;
    for(i=0;i<r->layout->n;i++)
        if(!_stricmp(r->layout->names[i], base)) return i;
    return -1;
}

const Release *release_by_id(const char *id){
    int i;
    for(i=0;i<NRELEASES;i++)
        if(!_stricmp(releases[i].id, id)) return &releases[i];
    return NULL;
}

int release_count(void){ return NRELEASES; }

const Release *release_at(int i){
    return (i>=0 && i<NRELEASES) ? &releases[i] : NULL;
}

static const RelFile *rel_find(const Release *r, const char *name){
    int i;
    for(i=0;i<r->nfiles;i++)
        if(!_stricmp(r->files[i].name, name)) return &r->files[i];
    return NULL;
}

/* ----------------------------------------------------------- directory I/O */
#define MAX_ENTRIES 512

typedef struct {
    char names[MAX_ENTRIES][64];
    int n;
    int dup;                       /* two names differing only by case */
    char dupname[64];
} DirList;

static int list_dir(const char *dir, DirList *dl){
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[600];
    dl->n = 0; dl->dup = 0; dl->dupname[0] = 0;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if(h == INVALID_HANDLE_VALUE) return -1;
    do {
        int i;
        if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if(strlen(fd.cFileName) >= sizeof(dl->names[0])) continue;
        for(i=0;i<dl->n;i++)
            if(!_stricmp(dl->names[i], fd.cFileName)){
                dl->dup = 1;
                snprintf(dl->dupname, sizeof(dl->dupname), "%s", fd.cFileName);
                break;
            }
        if(i < dl->n) continue;
        if(dl->n < MAX_ENTRIES)
            snprintf(dl->names[dl->n++], sizeof(dl->names[0]), "%s", fd.cFileName);
    } while(FindNextFileA(h, &fd));
    FindClose(h);
    return 0;
}

/* Case-insensitive lookup; returns the name as it is actually spelled on disk
 * so every later open uses the real filename. */
static const char *dir_find(const DirList *dl, const char *want){
    int i;
    for(i=0;i<dl->n;i++) if(!_stricmp(dl->names[i], want)) return dl->names[i];
    return NULL;
}

/* Does this directory hold the anchor program of any known layout? */
static const CodeLayout *anchor_in(const char *dir){
    char probe[700];
    int i;
    for(i=0;i<NLAYOUTS;i++){
        snprintf(probe, sizeof(probe), "%s\\%s", dir, layouts[i]->names[0]);
        if(GetFileAttributesA(probe) != INVALID_FILE_ATTRIBUTES) return layouts[i];
    }
    return NULL;
}

/* One level down only, and only to explain the mistake - never to recurse
 * into it and pick a winner.  GAME/FANTASY/INTRO.PRG is a common unpacking
 * result, and "game not found" is a useless thing to say about it. */
static int nested_install(const char *dir, char *sub, size_t subn){
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char pat[600], probe[700];
    int found = 0;
    snprintf(pat, sizeof(pat), "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if(h == INVALID_HANDLE_VALUE) return 0;
    do {
        if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        if(fd.cFileName[0] == '.') continue;
        snprintf(probe, sizeof(probe), "%s\\%s", dir, fd.cFileName);
        if(anchor_in(probe)){
            snprintf(sub, subn, "%s", fd.cFileName);
            found = 1;
            break;
        }
    } while(FindNextFileA(h, &fd));
    FindClose(h);
    return found;
}

/* ---------------------------------------------------------------- detector */
static void addf(RelResult *r, const char *fmt, ...){
    va_list ap;
    size_t n = strlen(r->detail);
    if(n + 1 >= sizeof(r->detail)) return;
    va_start(ap, fmt);
    vsnprintf(r->detail + n, sizeof(r->detail) - n, fmt, ap);
    va_end(ap);
}

static void addlist(char *dst, size_t n, const char *item){
    snprintf(dst + strlen(dst), n - strlen(dst), "%s%s", dst[0] ? ", " : "", item);
}

const char *release_state_name(RelState s){
    switch(s){
    case REL_RECOGNIZED: return "recognized";
    case REL_INCOMPLETE: return "incomplete";
    case REL_MODIFIED:   return "modified";
    case REL_MIXED:      return "mixed";
    case REL_UNKNOWN:    return "unknown";
    case REL_AMBIGUOUS:  return "ambiguous";
    case REL_ABSENT:     return "absent";
    default:             return "none";
    }
}

int release_runnable(const RelResult *r){
    return r && r->state == REL_RECOGNIZED && r->rel != NULL;
}

static int hash_in_dir(const char *dir, const DirList *dl, const char *want,
                       uint32_t prefix, uint8_t out[32], uint32_t *size){
    const char *real = dir_find(dl, want);
    char path[700];
    if(!real) return -1;
    snprintf(path, sizeof(path), "%s\\%s", dir, real);
    return hash_file(path, prefix, out, size);
}

int release_detect(const char *dir, RelResult *out){
    DirList dl;
    uint8_t code_sha[5][32];
    uint32_t code_size[5];
    int code_have[5];
    const Release *origin[5];
    const Release *cand;
    const CodeLayout *lay = NULL;
    int i, j, ncode, wrong = 0, foreign = 0;
    char hx[65];

    memset(out, 0, sizeof(*out));
    snprintf(out->dir, sizeof(out->dir), "%s", dir ? dir : "");
    if(!GetFullPathNameA(out->dir, (DWORD)sizeof(out->full), out->full, NULL))
        snprintf(out->full, sizeof(out->full), "%s", out->dir);
    addf(out, "scanned: %s\n", out->full);

    if(list_dir(out->dir, &dl) != 0){
        out->state = REL_ABSENT;
        snprintf(out->summary, sizeof(out->summary), "No such directory.");
        addf(out, "state: absent\nthe directory could not be opened.\n");
        return 0;
    }
    if(dl.dup){
        out->state = REL_AMBIGUOUS;
        snprintf(out->summary, sizeof(out->summary),
                 "Two files named '%s' differ only by case.", dl.dupname);
        addf(out, "state: ambiguous\nduplicate name (case only): %s\n"
                  "clean the directory so each DOS name appears once.\n", dl.dupname);
        return 0;
    }
    /* Which shape of distribution is this?  The anchor program answers it,
     * and two anchors in one directory means two distributions have been
     * poured into the same folder - which is not a preference to resolve,
     * since the programs of one release do not run under the other's boot. */
    for(i=0;i<NLAYOUTS;i++){
        if(!dir_find(&dl, layouts[i]->names[0])) continue;
        if(lay){
            out->state = REL_AMBIGUOUS;
            snprintf(out->summary, sizeof(out->summary),
                     "Two installations mixed in one directory (%s and %s).",
                     lay->names[0], layouts[i]->names[0]);
            addf(out, "state: ambiguous\nboth %s and %s are here, so this holds "
                      "more than one\ndistribution.  Give each its own directory.\n",
                 lay->names[0], layouts[i]->names[0]);
            return 0;
        }
        lay = layouts[i];
    }
    if(!lay){
        char sub[64], anchors[128];
        out->state = REL_ABSENT;
        anchors[0] = 0;
        for(i=0;i<NLAYOUTS;i++) addlist(anchors, sizeof(anchors), layouts[i]->names[0]);
        addf(out, "state: absent\nno game program is in this directory "
                  "(looked for %s).\n", anchors);
        if(nested_install(out->dir, sub, sizeof(sub))){
            snprintf(out->summary, sizeof(out->summary),
                     "Game files are one level down, in '%s'.", sub);
            addf(out, "'%s' does hold one - the files look like they are\n"
                      "one directory too deep.  Move the contents of '%s' up.\n",
                 sub, sub);
        } else {
            snprintf(out->summary, sizeof(out->summary),
                     "Not a Pinball Fantasies installation.");
        }
        return 0;
    }
    ncode = lay->n;
    addf(out, "programs: %s layout (%s)\n", lay->id, lay->names[0]);

    for(i=0;i<ncode;i++){
        code_have[i] = hash_in_dir(out->dir, &dl, lay->names[i], 0,
                                   code_sha[i], &code_size[i]) == 0;
        /* The identity vector replay records (docs/REPLAY.md section 3.1):
         * layout names in order with what is actually on disk, whatever the
         * verdict below turns out to be. */
        out->code_have[i] = code_have[i];
        snprintf(out->code_names[i], sizeof(out->code_names[i]), "%s", lay->names[i]);
        if(code_have[i]){
            memcpy(out->code_sha[i], code_sha[i], 32);
            out->code_size[i] = code_size[i];
        } else {
            memset(out->code_sha[i], 0, 32);
            out->code_size[i] = 0;
        }
        out->ncode = ncode;
        origin[i] = NULL;
        if(!code_have[i]) continue;
        for(j=0;j<NRELEASES;j++){
            const RelFile *rf;
            if(releases[j].layout != lay) continue;
            rf = rel_find(&releases[j], lay->names[i]);
            if(rf && rf->size == code_size[i] && !memcmp(rf->sha, code_sha[i], 32)){
                origin[i] = &releases[j];
                break;
            }
        }
    }
    cand = origin[0];

    if(!cand){
        out->state = REL_UNKNOWN;
        snprintf(out->summary, sizeof(out->summary),
                 "Unrecognised build - %s is not a known release.", lay->names[0]);
        addf(out, "state: unknown\n"
                  "%s does not match any release in this build's database.\n"
                  "Code vector, for adding this release (see docs/RELEASES.md):\n",
             lay->names[0]);
        for(i=0;i<ncode;i++){
            if(!code_have[i]){ addf(out, "  %-11s MISSING\n", lay->names[i]); continue; }
            hex32(code_sha[i], hx);
            addf(out, "  %-11s %8u  %s\n", lay->names[i], code_size[i], hx);
        }
        return 0;
    }

    out->rel = cand;
    for(i=1;i<ncode;i++){
        const RelFile *rf;
        if(!code_have[i]) continue;
        rf = rel_find(cand, lay->names[i]);
        if(rf && rf->size == code_size[i] && !memcmp(rf->sha, code_sha[i], 32)) continue;
        wrong++;
        if(origin[i]) foreign++;
    }

    /* Deliberately not "whichever release matches the most files": a code
     * vector that spans releases is a mixed installation, and there is no
     * intro layout that is safe to apply to it. */
    if(foreign){
        out->state = REL_MIXED;
        snprintf(out->summary, sizeof(out->summary),
                 "Mixed installation - programs come from different releases.");
        addf(out, "state: mixed\nthe programs do not all come from one release:\n");
        for(i=0;i<ncode;i++){
            if(!code_have[i]) addf(out, "  %-11s MISSING\n", lay->names[i]);
            else if(origin[i]) addf(out, "  %-11s %s\n", lay->names[i], origin[i]->id);
            else {
                hex32(code_sha[i], hx);
                addf(out, "  %-11s unknown  %8u  %s\n",
                     lay->names[i], code_size[i], hx);
            }
        }
        addf(out, "no single release's memory layout fits this mixture.\n");
        return 0;
    }
    if(wrong){
        out->state = REL_MODIFIED;
        snprintf(out->summary, sizeof(out->summary),
                 "Modified install - a program does not match %s.", cand->id);
        addf(out, "state: modified\nidentified as '%s' from %s, but:\n",
             cand->id, lay->names[0]);
        for(i=1;i<ncode;i++){
            const RelFile *rf = rel_find(cand, lay->names[i]);
            if(!code_have[i] || !rf) continue;
            if(rf->size == code_size[i] && !memcmp(rf->sha, code_sha[i], 32)) continue;
            hex32(rf->sha, hx);
            addf(out, "  %-11s expected %8u  %s\n", lay->names[i], rf->size, hx);
            hex32(code_sha[i], hx);
            addf(out, "  %-11s actual   %8u  %s\n", "", code_size[i], hx);
        }
        return 0;
    }

    /* The code vector is coherent.  Everything from here is about whether the
     * installation is complete, not about which release it is - so it is
     * reported separately, exactly as docs/RELEASES.md asks. */
    {
        int npresent = 0;
        for(i=0;i<ncode;i++) npresent += code_have[i];
        addf(out, "release: %s (%s)\ncode vector: %s\n", cand->id, cand->label,
             npresent == ncode ? "every program matches."
                               : "every program present matches, but some are missing.");
    }
    {
        int nmiss = 0, nodd = 0;
        char missnames[512];
        missnames[0] = 0;
        for(i=0;i<ncode;i++)
            if(!code_have[i]){ nmiss++; addlist(missnames, sizeof(missnames), lay->names[i]); }
        for(i=0;i<cand->nfiles;i++){
            const RelFile *rf = &cand->files[i];
            uint8_t sha[32];
            uint32_t sz;
            /* RF_BOOT is checked on its own below - it needs its hash
             * verified, not just its presence, and listing it in both places
             * would name it twice in the same "missing:" line. */
            if(rf->flags & (RF_MUTABLE|RF_META|RF_CODE|RF_BOOT)) continue;
            if(!(rf->flags & RF_REQ)) continue;
            if(hash_in_dir(out->dir, &dl, rf->name, rf->prefix, sha, &sz) != 0){
                nmiss++;
                addlist(missnames, sizeof(missnames), rf->name);
                continue;
            }
            /* Data, not identity: say so and carry on.  A replaced music
             * module is somebody's choice, not a reason to refuse to boot -
             * but it is counted into the summary line so it is not a
             * discovery the user only makes behind a Details button. */
            if(sz != rf->size || memcmp(sha, rf->sha, 32)){
                nodd++;
                addf(out, "warning: %s is not this release's copy"
                          " (%u bytes here, %u expected)\n", rf->name, sz, rf->size);
            }
        }
        {
            const char *boot = dir_find(&dl, cand->boot);
            const RelFile *rf = rel_find(cand, cand->boot);
            uint8_t sha[32];
            uint32_t sz;
            if(!boot){
                nmiss++;
                addlist(missnames, sizeof(missnames), cand->boot);
            } else {
                snprintf(out->boot, sizeof(out->boot), "%s", boot);
                /* Hashed so an unrelated file that happens to carry the
                 * release's boot name is never the thing we execute. */
                if(rf && hash_in_dir(out->dir, &dl, cand->boot, 0, sha, &sz) == 0 &&
                   (sz != rf->size || memcmp(sha, rf->sha, 32))){
                    out->state = REL_MODIFIED;
                    snprintf(out->summary, sizeof(out->summary),
                             "%s is not this release's boot program.", cand->boot);
                    hex32(rf->sha, hx);
                    addf(out, "state: modified\n  %-11s expected %8u  %s\n",
                         cand->boot, rf->size, hx);
                    hex32(sha, hx);
                    addf(out, "  %-11s actual   %8u  %s\n", "", sz, hx);
                    return 0;
                }
            }
        }
        if(nmiss){
            out->state = REL_INCOMPLETE;
            snprintf(out->summary, sizeof(out->summary),
                     "%s: %d required file%s missing.",
                     cand->label, nmiss, nmiss==1 ? "" : "s");
            addf(out, "state: incomplete\nmissing: %s\n", missnames);
            return 0;
        }
        out->odd = nodd;
    }

    /* Everything required is present and correct.  Anything else here is
     * noted and ignored: a distribution's own extras, a readme, pfemu's own
     * state files, a log the user dropped in. */
    {
        char extras[768];
        int nex = 0;
        extras[0] = 0;
        for(i=0;i<dl.n;i++){
            if(rel_find(cand, dl.names[i])) continue;
            nex++;
            if(strlen(extras) + strlen(dl.names[i]) + 3 < sizeof(extras))
                addlist(extras, sizeof(extras), dl.names[i]);
        }
        if(nex) addf(out, "ignored extra files (%d): %s\n", nex, extras);
    }
    out->state = REL_RECOGNIZED;
    if(out->odd)
        snprintf(out->summary, sizeof(out->summary),
                 "%s (%d data file%s differ%s)", cand->label,
                 out->odd, out->odd==1 ? "" : "s", out->odd==1 ? "s" : "");
    else
        snprintf(out->summary, sizeof(out->summary), "%s", cand->label);
    addf(out, "state: recognized\nboot program: %s\n", out->boot);
    if(cand->cfg_buf)
        addf(out, "intro options buffer: DS:%04X\n", cand->cfg_buf);
    else
        addf(out, "intro options buffer: none - this build has no options menu\n");
    return 0;
}

/* --------------------------------------------------------- install search */
/* GAME is the documented place to put one installation (docs/RELEASES.md).
 * Any other top-level directory holding the anchor program of a known layout
 * is offered as well, so a collection of releases sitting side by side keeps
 * working - the directory is then only a location to look in, never the
 * identity of what is found. */
static const char *skip_dirs[] = {
    "src", "docs", "tools", "trainer", "screenshots", "PFEMU-STATE", NULL
};

static int is_skipped(const char *name){
    int i;
    for(i=0;skip_dirs[i];i++) if(!_stricmp(skip_dirs[i], name)) return 1;
    return 0;
}

int release_scan(RelResult *out, int max){
    WIN32_FIND_DATAA fd;
    HANDLE h;
    char names[64][64];
    int n = 0, i, k = 0, first;

    if(anchor_in("GAME"))
        snprintf(names[n++], sizeof(names[0]), "GAME");
    first = n;

    h = FindFirstFileA("*", &fd);
    if(h != INVALID_HANDLE_VALUE){
        do {
            if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
            if(fd.cFileName[0] == '.') continue;
            if(is_skipped(fd.cFileName) || !_stricmp(fd.cFileName, "GAME")) continue;
            if(strlen(fd.cFileName) >= sizeof(names[0])) continue;
            if(!anchor_in(fd.cFileName)) continue;
            if(n < 64) snprintf(names[n++], sizeof(names[0]), "%s", fd.cFileName);
        } while(FindNextFileA(h, &fd));
        FindClose(h);
    }
    /* Alphabetical after GAME, so "the first one found" is a stable choice
     * rather than whatever order the filesystem happened to hand back. */
    for(i = first; i < n; i++){
        int j;
        for(j=i+1;j<n;j++)
            if(_stricmp(names[j], names[i]) < 0){
                char t[64];
                memcpy(t, names[i], sizeof(t));
                memcpy(names[i], names[j], sizeof(t));
                memcpy(names[j], t, sizeof(t));
            }
    }
    for(i=0;i<n && k<max;i++) if(release_detect(names[i], &out[k]) == 0) k++;
    return k;
}
