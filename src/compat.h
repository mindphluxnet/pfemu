/* Platform compatibility: one include in place of <windows.h>.
 *
 * The emulation core (cpu, vga, dev, bios, fantasies, png) contains no
 * Win32 at all and does not need this.  Six files do, and only for small
 * things - directory enumeration, a few path and file operations, and the
 * waveOut types in sound.c.  On Windows this header is just the real headers
 * they always included.  Everywhere else it declares the same names, and
 * src/posix.c implements them on top of dirent/stat.
 *
 * This is deliberately a shim and not an abstraction.  Rewriting six files
 * around a new portable file API would touch far more code than it replaces,
 * and every line of that is a line where the Windows build could pick up a
 * behaviour change - which is the one thing docs/VERIFY.md's Spike B cannot
 * afford, because it is trying to measure whether the two platforms agree.
 * Keeping the callers byte-identical means any difference the spike finds is
 * a real difference, not one this header introduced.
 */
#ifndef PFEMU_COMPAT_H
#define PFEMU_COMPAT_H

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>
#include <direct.h>

/* NTFS already matches names without regard to case, so there is nothing to
 * resolve here. */
#define host_casefix(p) ((void)0)

#else  /* ------------------------------------------------------- POSIX --- */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>

typedef uint32_t DWORD;
typedef uint16_t WORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef char *LPSTR;
typedef void *HANDLE;

#define MAX_PATH 260
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INVALID_FILE_ATTRIBUTES ((DWORD)0xFFFFFFFF)

#define FILE_ATTRIBUTE_READONLY  0x00000001u
#define FILE_ATTRIBUTE_HIDDEN    0x00000002u
#define FILE_ATTRIBUTE_SYSTEM    0x00000004u
#define FILE_ATTRIBUTE_DIRECTORY 0x00000010u
#define FILE_ATTRIBUTE_ARCHIVE   0x00000020u
#define FILE_ATTRIBUTE_NORMAL    0x00000080u

typedef struct { DWORD dwLowDateTime, dwHighDateTime; } FILETIME;
typedef struct {
    WORD wYear, wMonth, wDayOfWeek, wDay, wHour, wMinute, wSecond, wMilliseconds;
} SYSTEMTIME;

typedef struct {
    DWORD    dwFileAttributes;
    FILETIME ftCreationTime, ftLastAccessTime, ftLastWriteTime;
    DWORD    nFileSizeHigh, nFileSizeLow;
    DWORD    dwReserved0, dwReserved1;
    char     cFileName[MAX_PATH];
    char     cAlternateFileName[14];
} WIN32_FIND_DATAA;

/* Directory enumeration.  The pattern is a host path whose last component may
 * contain DOS wildcards (star, star-dot-PRG); matching is case-insensitive,
 * as it is on Windows.  See src/posix.c on enumeration ORDER, which is
 * guest-visible through INT 21h AH=4Eh/4Fh and therefore part of what a
 * cross-platform replay depends on. */
HANDLE FindFirstFileA(const char *pattern, WIN32_FIND_DATAA *fd);
BOOL   FindNextFileA(HANDLE h, WIN32_FIND_DATAA *fd);
BOOL   FindClose(HANDLE h);

DWORD  GetFileAttributesA(const char *path);
BOOL   CreateDirectoryA(const char *path, void *sec);
BOOL   RemoveDirectoryA(const char *path);
BOOL   DeleteFileA(const char *path);
DWORD  GetTempPathA(DWORD n, char *buf);
DWORD  GetFullPathNameA(const char *name, DWORD n, char *out, char **part);
DWORD  GetTickCount(void);
DWORD  GetCurrentProcessId(void);
/* The running program's path, for the files kept beside it (beside_exe(),
 * src/cfg.c).  Only a NULL module is supported. */
DWORD  GetModuleFileNameA(void *module, char *out, DWORD n);

/* Resolve the last component of a host path against the directory, ignoring
 * case, when the exact name does not exist.  Rewrites in place; a
 * case-insensitive match is the same length, so nothing can grow.  See
 * src/posix.c for why the guest needs this and src/dos.c for where it is
 * applied. */
void   host_casefix(char *path);
void   GetLocalTime(SYSTEMTIME *st);
BOOL   FileTimeToLocalFileTime(const FILETIME *in, FILETIME *out);
BOOL   FileTimeToSystemTime(const FILETIME *in, SYSTEMTIME *out);

/* --- waveOut, for src/sound.c ---------------------------------------------
 * Declared so that the synthesis half of sound.c - which is platform-free and
 * is the half the guest can hear - compiles unchanged.  src/host_null.c
 * implements waveOutOpen as a failure, which is a path sound.c already
 * handles: hwo stays NULL and it runs silent.  Everything -wav captures is
 * tapped upstream of that, so the capture and its FNV hash are unaffected. */
typedef void *HWAVEOUT;
typedef struct { WORD wFormatTag, nChannels; DWORD nSamplesPerSec, nAvgBytesPerSec;
                 WORD nBlockAlign, wBitsPerSample, cbSize; } WAVEFORMATEX;
typedef struct WAVEHDR_ {
    LPSTR lpData; DWORD dwBufferLength, dwBytesRecorded;
    DWORD dwUser, dwFlags, dwLoops;
    struct WAVEHDR_ *lpNext; DWORD reserved;
} WAVEHDR;

#define WAVE_FORMAT_PCM   1
#define WAVE_MAPPER       ((UINT)-1)
#define CALLBACK_NULL     0
#define MMSYSERR_NOERROR  0
#define WHDR_DONE         0x00000001

UINT waveOutOpen(HWAVEOUT *h, UINT dev, const WAVEFORMATEX *fmt,
                 void *cb, void *inst, DWORD flags);
UINT waveOutPrepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n);
UINT waveOutUnprepareHeader(HWAVEOUT h, WAVEHDR *hdr, UINT n);
UINT waveOutWrite(HWAVEOUT h, WAVEHDR *hdr, UINT n);
UINT waveOutReset(HWAVEOUT h);
UINT waveOutClose(HWAVEOUT h);

#endif /* _WIN32 */
#endif /* PFEMU_COMPAT_H */
