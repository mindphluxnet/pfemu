/* POSIX implementations of the handful of Win32 calls the non-core files use.
 * Declared in src/compat.h; compiled only off Windows.
 *
 * ENUMERATION ORDER is the one thing in here that is not a formality.
 * FindFirstFileA/FindNextFileA reach the guest: INT 21h AH=4Eh/4Fh
 * (src/dos.c) hands the results straight to the program, so the order files
 * come back in is guest-visible, and anything guest-visible has to be
 * identical on both platforms or a replay cannot be compared across them.
 *
 * readdir() returns entries in whatever order the filesystem stores them -
 * on ext4 with dir_index that is a hash order, which is stable for a given
 * directory but has nothing to do with the order NTFS would produce.  So the
 * entries are read in full and sorted, case-insensitively by name, before
 * anything is returned.  That makes this side deterministic and independent
 * of the filesystem.
 *
 * It does NOT make it agree with Windows: FindFirstFileA there returns the
 * NTFS b-tree order, which this only approximates, and nothing sorts it.  If
 * a cross-platform replay ever diverges inside a directory scan, that is the
 * first place to look, and the fix is to sort on both sides rather than to
 * guess at NTFS collation here.  Recorded as an open question in
 * docs/VERIFY.md.
 */
#ifndef _WIN32

#include "compat.h"
#include <dirent.h>
#include <ctype.h>
#include <time.h>
#include <limits.h>

/* --------------------------------------------------------------- glob --- */
/* DOS wildcards, case-insensitive, as Windows matches them.  '*' spans any
 * run of characters, '?' exactly one. */
static int wild_match(const char *pat, const char *s){
    while(*pat){
        if(*pat == '*'){
            pat++;
            if(!*pat) return 1;
            for(; *s; s++) if(wild_match(pat, s)) return 1;
            return wild_match(pat, s);
        }
        if(!*s) return 0;
        if(*pat != '?' && tolower((unsigned char)*pat) != tolower((unsigned char)*s))
            return 0;
        pat++; s++;
    }
    return !*s;
}

/* ---------------------------------------------------------- find state --- */
typedef struct {
    char   dir[1024];
    char **names;
    int    n, i;
} Find;

static int name_cmp(const void *a, const void *b){
    const char *x = *(const char *const *)a, *y = *(const char *const *)b;
    int r = strcasecmp(x, y);
    return r ? r : strcmp(x, y);   /* total order even for case-only pairs */
}

static void find_free(Find *f){
    int i;
    if(!f) return;
    for(i=0;i<f->n;i++) free(f->names[i]);
    free(f->names);
    free(f);
}

/* Fill fd from dir/name.  A file that vanished between readdir and stat is
 * reported with zeroed metadata rather than skipped: Windows has the same
 * race and the callers all tolerate it. */
static void fill(Find *f, const char *name, WIN32_FIND_DATAA *fd){
    char path[2048];
    struct stat st;
    memset(fd, 0, sizeof(*fd));
    snprintf(fd->cFileName, sizeof(fd->cFileName), "%s", name);
    snprintf(path, sizeof(path), "%s/%s", f->dir, name);
    if(stat(path, &st) != 0){ fd->dwFileAttributes = FILE_ATTRIBUTE_NORMAL; return; }
    fd->dwFileAttributes = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY
                                               : FILE_ATTRIBUTE_ARCHIVE;
    if(!(st.st_mode & S_IWUSR)) fd->dwFileAttributes |= FILE_ATTRIBUTE_READONLY;
    fd->nFileSizeLow  = (DWORD)(st.st_size & 0xFFFFFFFFu);
    fd->nFileSizeHigh = (DWORD)((uint64_t)st.st_size >> 32);
    {   /* Unix epoch -> 100 ns ticks since 1601, which is what FILETIME is.
         * Only ever read when the DOS clock is NOT frozen; a replay freezes
         * it (src/dos.c fill_dta), so this never reaches a compared run. */
        uint64_t t = ((uint64_t)st.st_mtime + 11644473600ULL) * 10000000ULL;
        fd->ftLastWriteTime.dwLowDateTime  = (DWORD)t;
        fd->ftLastWriteTime.dwHighDateTime = (DWORD)(t >> 32);
        fd->ftCreationTime = fd->ftLastAccessTime = fd->ftLastWriteTime;
    }
}

HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd){
    Find *f;
    DIR *d;
    struct dirent *e;
    const char *slash, *pat;
    int cap = 0;

    f = (Find*)calloc(1, sizeof(Find));
    if(!f) return INVALID_HANDLE_VALUE;

    /* Split the pattern into a directory and a last component.  Accept both
     * separators: the callers build paths with '\\' and '/' interchangeably. */
    {   const char *a = strrchr(pattern, '/'), *b = strrchr(pattern, '\\');
        slash = (a && b) ? (a > b ? a : b) : (a ? a : b);
    }
    if(slash){
        size_t n = (size_t)(slash - pattern);
        if(n >= sizeof(f->dir)) n = sizeof(f->dir) - 1;
        memcpy(f->dir, pattern, n);
        f->dir[n] = 0;
        if(!f->dir[0]) snprintf(f->dir, sizeof(f->dir), "/");
        pat = slash + 1;
    } else {
        snprintf(f->dir, sizeof(f->dir), ".");
        pat = pattern;
    }
    if(!*pat) pat = "*";

    d = opendir(f->dir);
    if(!d){ find_free(f); return INVALID_HANDLE_VALUE; }
    while((e = readdir(d)) != NULL){
        if(!wild_match(pat, e->d_name)) continue;
        if(f->n == cap){
            char **grown;
            cap = cap ? cap * 2 : 32;
            grown = (char**)realloc(f->names, (size_t)cap * sizeof(char*));
            if(!grown) break;
            f->names = grown;
        }
        f->names[f->n] = strdup(e->d_name);
        if(!f->names[f->n]) break;
        f->n++;
    }
    closedir(d);

    if(f->n == 0){ find_free(f); return INVALID_HANDLE_VALUE; }
    qsort(f->names, (size_t)f->n, sizeof(char*), name_cmp);
    fill(f, f->names[0], fd);
    f->i = 1;
    return (HANDLE)f;
}

BOOL FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd){
    Find *f = (Find*)h;
    if(!f || f == INVALID_HANDLE_VALUE || f->i >= f->n) return 0;
    fill(f, f->names[f->i++], fd);
    return 1;
}

BOOL FindClose(HANDLE h){
    if(!h || h == INVALID_HANDLE_VALUE) return 0;
    find_free((Find*)h);
    return 1;
}

/* ------------------------------------------------------------ case fold ---
 * The guest names its files in whatever case the original program was written
 * with, and it is not consistent: Pinball Fantasies Deluxe opens its sound
 * configuration as "SoUnD.cFg" while the file on disk is SOUND.CFG.  DOS and
 * NTFS do not care.  ext4 does, so on Linux that open failed, the table never
 * got its sound driver, and the guest sat in text mode for the whole run
 * while the replay dutifully injected all 292 events into nothing.
 *
 * Only the last component is resolved.  This DOS layer has no subdirectories
 * at all - every guest file lands directly in gamedir or the write overlay
 * (see dos_path() in src/dos.c) - so there is no other component to get
 * wrong.
 *
 * Exact matches are never touched, which is what keeps a newly created file
 * the name the guest asked for.  When several names differ only in case the
 * lexicographically first is taken, so the choice does not depend on the
 * order the filesystem happens to return them in.
 */
void host_casefix(char *path){
    char dir[1024], best[256];
    const char *base;
    char *slash;
    DIR *d;
    struct dirent *e;
    int have = 0;

    if(!path || !*path) return;
    if(access(path, F_OK) == 0) return;          /* the name is already right */

    slash = strrchr(path, '/');
    if(!slash){
        snprintf(dir, sizeof(dir), ".");
        base = path;
    } else {
        size_t n = (size_t)(slash - path);
        if(n >= sizeof(dir)) return;
        memcpy(dir, path, n);
        dir[n] = 0;
        if(!dir[0]) snprintf(dir, sizeof(dir), "/");
        base = slash + 1;
    }
    if(!*base || strlen(base) >= sizeof(best)) return;

    d = opendir(dir);
    if(!d) return;
    while((e = readdir(d)) != NULL){
        if(strcasecmp(e->d_name, base) != 0) continue;
        if(!have || strcmp(e->d_name, best) < 0){
            snprintf(best, sizeof(best), "%s", e->d_name);
            have = 1;
        }
    }
    closedir(d);

    /* Same length by construction - only the case differs. */
    if(have) memcpy(slash ? slash + 1 : path, best, strlen(best));
}

/* --------------------------------------------------------- file system --- */
DWORD GetFileAttributesA(const char *path){
    struct stat st;
    DWORD a;
    if(stat(path, &st) != 0) return INVALID_FILE_ATTRIBUTES;
    a = S_ISDIR(st.st_mode) ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_ARCHIVE;
    if(!(st.st_mode & S_IWUSR)) a |= FILE_ATTRIBUTE_READONLY;
    return a;
}

BOOL CreateDirectoryA(const char *path, void *sec){
    (void)sec;
    return mkdir(path, 0777) == 0;
}

BOOL RemoveDirectoryA(const char *path){ return rmdir(path) == 0; }
BOOL DeleteFileA(const char *path){ return unlink(path) == 0; }

DWORD GetTempPathA(DWORD n, char *buf){
    const char *t = getenv("TMPDIR");
    int len;
    if(!t || !*t) t = "/tmp";
    len = snprintf(buf, n, "%s/", t);
    return (len < 0 || (DWORD)len >= n) ? 0 : (DWORD)len;
}

/* Only has to satisfy the one caller in src/run.c: make a relative install
 * directory absolute against the launch-time CWD.  No attempt at the rest of
 * GetFullPathName's contract (".." folding, short names, drive-relative
 * paths) - nothing here asks for it. */
DWORD GetFullPathNameA(const char *name, DWORD n, char *out, char **part){
    char cwd[PATH_MAX];
    int len;
    if(part) *part = NULL;
    if(!name || !out) return 0;
    if(name[0] == '/'){
        len = snprintf(out, n, "%s", name);
    } else {
        if(!getcwd(cwd, sizeof(cwd))) return 0;
        len = snprintf(out, n, "%s/%s", cwd, name);
    }
    return (len < 0 || (DWORD)len >= n) ? 0 : (DWORD)len;
}

/* /proc/self/exe, so Linux only.  0 on any failure, including a path that
 * does not fit: the caller then falls back to the CWD, as on Windows. */
DWORD GetModuleFileNameA(void *module, char *out, DWORD n){
    ssize_t len;
    if(module || !out || n == 0) return 0;
    len = readlink("/proc/self/exe", out, (size_t)n);
    if(len <= 0 || (DWORD)len >= n) return 0;
    out[len] = 0;
    return (DWORD)len;
}

/* ---------------------------------------------------------------- time --- */
/* Host wall clock.  Never guest-visible: the only host-clock reads the guest
 * can make are INT 21h AH=2Ah/2Ch, which a replay freezes (src/dos.c). */
DWORD GetTickCount(void){
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (DWORD)(ts.tv_sec * 1000ULL + (uint64_t)ts.tv_nsec / 1000000ULL);
}

DWORD GetCurrentProcessId(void){ return (DWORD)getpid(); }

void GetLocalTime(SYSTEMTIME *st){
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    memset(st, 0, sizeof(*st));
    st->wYear      = (WORD)(tm.tm_year + 1900);
    st->wMonth     = (WORD)(tm.tm_mon + 1);
    st->wDayOfWeek = (WORD)tm.tm_wday;
    st->wDay       = (WORD)tm.tm_mday;
    st->wHour      = (WORD)tm.tm_hour;
    st->wMinute    = (WORD)tm.tm_min;
    st->wSecond    = (WORD)tm.tm_sec;
}

BOOL FileTimeToLocalFileTime(const FILETIME *in, FILETIME *out){
    *out = *in;   /* the one caller converts to SYSTEMTIME next, below */
    return 1;
}

BOOL FileTimeToSystemTime(const FILETIME *in, SYSTEMTIME *out){
    uint64_t t = ((uint64_t)in->dwHighDateTime << 32) | in->dwLowDateTime;
    time_t secs;
    struct tm tm;
    if(t < 116444736000000000ULL) return 0;
    secs = (time_t)((t - 116444736000000000ULL) / 10000000ULL);
    localtime_r(&secs, &tm);
    memset(out, 0, sizeof(*out));
    out->wYear      = (WORD)(tm.tm_year + 1900);
    out->wMonth     = (WORD)(tm.tm_mon + 1);
    out->wDayOfWeek = (WORD)tm.tm_wday;
    out->wDay       = (WORD)tm.tm_mday;
    out->wHour      = (WORD)tm.tm_hour;
    out->wMinute    = (WORD)tm.tm_min;
    out->wSecond    = (WORD)tm.tm_sec;
    return 1;
}

#endif /* !_WIN32 */
