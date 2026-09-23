/* Import Pinball Fantasies Deluxe from an image of its CD.
 *
 * The one image this exists for is GOG's game.gog (docs/RELEASES.md, GOG
 * section): track 1 of the Deluxe CD as raw 2352-byte sectors, Mode 2 Form 1.
 * Plain 2048-byte ISOs and Mode 1 raw images read the same way, so an image
 * of the 1995 retail disc imports too - the directory layout is the disc's,
 * not GOG's.
 *
 * The disc keeps the game in two directories, \PFD\FANTASY (programs, music,
 * PINBALL.EXE) and \SOUNDSYS (drivers, SETSOUND.EXE).  pfemu wants one flat
 * folder, so both are copied into it.  Nothing is decided here about what
 * the files are: the import only copies, and release_detect() on the result
 * says which release it is, exactly as for a folder the user filled by hand.
 *
 * The copy is built in a hidden sibling directory and renamed into place at
 * the end, so a failed or interrupted import never leaves a half-filled
 * folder for release_scan() to find (it skips names starting with '.'). */
#include "pfemu.h"
#include <errno.h>
#ifdef _WIN32
#include <direct.h>
#define mkdir_1(p) _mkdir(p)
#define rmdir_1(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define mkdir_1(p) mkdir((p), 0777)
#define rmdir_1(p) rmdir(p)
#endif

#define CD_SYNC_LEN 12
static const uint8_t cd_sync[CD_SYNC_LEN] =
    { 0x00,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0xFF,0x00 };

/* The game's own files are at most ~540 KB and a directory on this disc is
 * one sector; these only stop a damaged image from asking for gigabytes. */
#define CD_MAX_FILE  (16u << 20)
#define CD_MAX_DIR   (1u << 20)
#define CD_MAX_FILES 64

typedef struct {
    FILE *f;
    long long sectors;
    int stride, offset;          /* bytes per sector, user data offset */
} CdImage;

typedef struct {
    char name[16];
    uint32_t lba, size;
} CdFile;

static int cd_open(CdImage *c, const char *path, char *err, size_t n){
    uint8_t head[16];
    long long size;
    memset(c, 0, sizeof(*c));
    c->f = fopen(path, "rb");
    if(!c->f){ snprintf(err, n, "cannot open %s", path); return -1; }
    if(fseek(c->f, 0, SEEK_END) != 0 || (size = ftell(c->f)) < 0){
        snprintf(err, n, "cannot read %s", path); return -1;
    }
    rewind(c->f);
    if(fread(head, 1, sizeof(head), c->f) != sizeof(head)){
        snprintf(err, n, "%s is too short to be a CD image", path); return -1;
    }
    if(!memcmp(head, cd_sync, CD_SYNC_LEN) && size % 2352 == 0){
        c->stride = 2352;
        /* Mode 1: sync + 4-byte header.  Mode 2 Form 1: 8 more bytes of
         * subheader.  ISO 9660 structures are never in Form 2 sectors. */
        if(head[15] == 1) c->offset = 16;
        else if(head[15] == 2) c->offset = 24;
        else { snprintf(err, n, "unsupported CD sector mode %d", head[15]); return -1; }
    } else {
        c->stride = 2048;
        c->offset = 0;
    }
    c->sectors = size / c->stride;
    return 0;
}

static int cd_read(CdImage *c, uint32_t lba, uint32_t len, uint8_t *out){
    uint32_t done = 0;
    while(done < len){
        uint32_t chunk = len - done > 2048 ? 2048 : len - done;
        if((long long)lba >= c->sectors) return -1;
        if(fseek(c->f, (long)lba * c->stride + c->offset, SEEK_SET) != 0) return -1;
        if(fread(out + done, 1, chunk, c->f) != chunk) return -1;
        done += chunk;
        lba++;
    }
    return 0;
}

/* One directory's entries.  With want != NULL: find that subdirectory and
 * return its extent in *lba/*len.  Otherwise: list its files into out[]. */
static int cd_dir(CdImage *c, uint32_t lba, uint32_t len, const char *want,
                  uint32_t *sub_lba, uint32_t *sub_len,
                  CdFile *out, int max, int *count){
    uint8_t *d;
    uint32_t pos = 0;
    int found = 0;
    if(len == 0 || len > CD_MAX_DIR) return -1;
    d = (uint8_t*)malloc(len);
    if(!d) return -1;
    if(cd_read(c, lba, len, d) != 0){ free(d); return -1; }
    while(pos < len){
        uint32_t rl = d[pos], nl, ext, size, k;
        char name[40];
        int is_dir;
        if(rl == 0){                   /* records never cross a sector */
            pos = (pos / 2048 + 1) * 2048;
            continue;
        }
        if(rl < 34 || pos + rl > len) break;
        ext  = d[pos+2] | d[pos+3]<<8 | d[pos+4]<<16 | (uint32_t)d[pos+5]<<24;
        size = d[pos+10] | d[pos+11]<<8 | d[pos+12]<<16 | (uint32_t)d[pos+13]<<24;
        is_dir = (d[pos+25] & 2) != 0;
        nl = d[pos+32];
        if(33 + nl > rl){ pos += rl; continue; }
        if(nl == 1 && d[pos+33] <= 1){ pos += rl; continue; }  /* "." and ".." */
        for(k = 0; k < nl && k < sizeof(name) - 1 && d[pos+33+k] != ';'; k++)
            name[k] = (char)d[pos+33+k];
        name[k] = 0;
        if(k && name[k-1] == '.') name[--k] = 0;   /* "NAME." for no extension */
        pos += rl;
        if(want){
            if(is_dir && !_stricmp(name, want)){
                *sub_lba = ext; *sub_len = size; found = 1;
                break;
            }
        } else if(!is_dir){
            if(*count >= max || k >= sizeof(out[0].name)){ free(d); return -1; }
            snprintf(out[*count].name, sizeof(out[0].name), "%s", name);
            out[*count].lba = ext;
            out[*count].size = size;
            (*count)++;
        }
    }
    free(d);
    return want ? (found ? 0 : -1) : 0;
}

static int cd_find_dir(CdImage *c, uint32_t lba, uint32_t len, const char *path,
                       uint32_t *out_lba, uint32_t *out_len){
    char part[40];
    const char *p = path;
    while(*p){
        size_t k = 0;
        while(*p && *p != '/' && k < sizeof(part) - 1) part[k++] = *p++;
        part[k] = 0;
        if(*p == '/') p++;
        if(cd_dir(c, lba, len, part, &lba, &len, NULL, 0, NULL) != 0) return -1;
    }
    *out_lba = lba; *out_len = len;
    return 0;
}

static void remove_tree_flat(const char *dir, const CdFile *files, int n){
    char path[700];
    int i;
    for(i = 0; i < n; i++){
        snprintf(path, sizeof(path), "%s/%s", dir, files[i].name);
        remove(path);
    }
    rmdir_1(dir);
}

int cdimage_import(const char *image, const char *dest, char *err, size_t n){
    static const char *const from[] = { "PFD/FANTASY", "SOUNDSYS" };
    CdImage c;
    CdFile files[CD_MAX_FILES];
    int nfiles = 0, i, j, have_intro = 0;
    uint8_t pvd[2048], *buf;
    uint32_t root_lba, root_len;
    char tmp[600], path[700];

    if(cd_open(&c, image, err, n) != 0){ if(c.f) fclose(c.f); return -1; }
    if(cd_read(&c, 16, 2048, pvd) != 0 || pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5)){
        snprintf(err, n, "%s is not an ISO 9660 CD image", image);
        fclose(c.f); return -1;
    }
    root_lba = pvd[158] | pvd[159]<<8 | pvd[160]<<16 | (uint32_t)pvd[161]<<24;
    root_len = pvd[166] | pvd[167]<<8 | pvd[168]<<16 | (uint32_t)pvd[169]<<24;
    for(i = 0; i < 2; i++){
        uint32_t lba, len;
        int before = nfiles;
        if(cd_find_dir(&c, root_lba, root_len, from[i], &lba, &len) != 0 ||
           cd_dir(&c, lba, len, NULL, NULL, NULL, files, CD_MAX_FILES, &nfiles) != 0){
            snprintf(err, n, "no %s directory on the disc - this is not the"
                     " Pinball Fantasies Deluxe CD", from[i]);
            fclose(c.f); return -1;
        }
        for(j = before; j < nfiles; j++)
            if(!_stricmp(files[j].name, "INTRO.PRG")) have_intro = 1;
    }
    if(!have_intro){
        snprintf(err, n, "no INTRO.PRG on the disc - this is not the"
                 " Pinball Fantasies Deluxe CD");
        fclose(c.f); return -1;
    }
    /* Two directories into one: a name in both would lose a file. */
    for(i = 0; i < nfiles; i++)
        for(j = i + 1; j < nfiles; j++)
            if(!_stricmp(files[i].name, files[j].name)){
                snprintf(err, n, "%s is on the disc twice", files[i].name);
                fclose(c.f); return -1;
            }

    /* Hidden work directory next to dest, cleared of any earlier attempt. */
    {
        const char *sep = strrchr(dest, '/'), *sep2 = strrchr(dest, '\\');
        size_t dl;
        if(sep2 && (!sep || sep2 > sep)) sep = sep2;
        dl = sep ? (size_t)(sep - dest + 1) : 0;
        snprintf(tmp, sizeof(tmp), "%.*s.pfemu-import", (int)dl, dest);
    }
    remove_tree_flat(tmp, files, nfiles);
    if(mkdir_1(tmp) != 0){
        snprintf(err, n, "cannot create %s", tmp);
        fclose(c.f); return -1;
    }
    buf = (uint8_t*)malloc(CD_MAX_FILE);
    if(!buf){ snprintf(err, n, "out of memory"); fclose(c.f); rmdir_1(tmp); return -1; }
    for(i = 0; i < nfiles; i++){
        FILE *o;
        if(files[i].size > CD_MAX_FILE || cd_read(&c, files[i].lba, files[i].size, buf) != 0){
            snprintf(err, n, "cannot read %s from the image", files[i].name);
            goto fail;
        }
        snprintf(path, sizeof(path), "%s/%s", tmp, files[i].name);
        o = fopen(path, "wb");
        if(!o || fwrite(buf, 1, files[i].size, o) != files[i].size){
            if(o) fclose(o);
            snprintf(err, n, "cannot write %s", path);
            goto fail;
        }
        if(fclose(o) != 0){ snprintf(err, n, "cannot write %s", path); goto fail; }
    }
    free(buf);
    fclose(c.f);
    if(rename(tmp, dest) != 0){
        snprintf(err, n, "cannot rename %s to %s (%s)", tmp, dest, strerror(errno));
        remove_tree_flat(tmp, files, nfiles);
        return -1;
    }
    snprintf(err, n, "%d files", nfiles);
    return 0;
fail:
    free(buf);
    fclose(c.f);
    remove_tree_flat(tmp, files, nfiles);
    return -1;
}
