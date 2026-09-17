# FANTASYA / Pinball Power Pack build analysis

Date: 2026-09-16

## Executive summary

`FANTASYA` is a later repackaging of the floppy-family DOS release, not the
1995 Deluxe CD-ROM build. Its real-mode launcher, renamed `PF.EXE`, is
byte-for-byte identical to `FANTASY/PINBALL.EXE`; its table music and most
drivers are also the known floppy files. The package mixes those files with a
different `INTRO.PRG`, rebuilt Table 1 and Table 2 programs, two different
sound-driver builds, and the newer setup utility from the Deluxe release.

The current emulator can execute the alternate binaries, but it does **not**
support this directory as a complete drop-in installation:

1. The GUI launcher only discovers `FANTASY` and `FANTASYDX`, and always uses
   `PINBALL.EXE`. `FANTASYA` contains `PF.EXE` instead.
2. A direct `-d FANTASYA` boot consequently fails to find `PINBALL.EXE`.
3. `-d FANTASYA -p PF.EXE` starts the game, but the Fantasies session is not
   armed: neither the directory basename `FANTASYA` nor program basename
   `PF.EXE` is recognized by `fantasies_begin_session()`. All game-specific
   fixes, read interception, and trainer behavior are therefore disabled.
4. If a Fantasies session is forced by directly booting `INTRO.PRG`, the
   options interceptor writes the six option bytes to the wrong offset. The
   alternate intro uses `DS:4846`, while pfemu unconditionally uses
   `DS:49A3`.

The table-side patch design itself holds up well. All four alternate tables
have unique matches for the pause-race, infinite-balls, spring-control, and
ball-gap signatures. Runtime smoke loads armed the pause, spring, balls, and
flipper fixes for every table. Tables 1 and 2 have moved code/data, so this is
also a useful confirmation that the signature scans are doing their job.

No existing file was changed during this investigation. Runtime tests used a
copy under the system temporary directory.

## Release identification and provenance

The ZIP's `21STINFO.DAT`, one directory above the `FANTASY` subdirectory, was
not included in `FANTASYA`. The supplied identifying line was:

> Pinball Power Pack. (c) 21st Century Entertainment. Ltd. 1996

No runtime file in `FANTASYA` contains `21STINFO`, `Power Pack`, or `1996`, and
none references `21STINFO.DAT`. It is therefore packaging metadata, not a
runtime dependency. The common 1996-12-24 timestamps likewise describe the
archive/repackaging; they do not mean that every binary was rebuilt in 1996.

The strongest family identification is the launcher:

- `FANTASYA/PF.EXE` and `FANTASY/PINBALL.EXE` have the same 1,742 bytes and
  SHA-256 `e7acf53b1af353bd44805579f49a594bf33d243cb8c50a23f0d5c50bb75765ba`.
- `FANTASYDX/PINBALL.EXE` is a different 2,595-byte program with the Deluxe
  CD-presence/path behavior.
- `FANTASYA/PINBALL.BAT` runs `SETSOUND` only if `SOUND.CFG` is absent, then
  runs `PF.EXE`. pfemu has no command interpreter for this wrapper; it must
  execute the underlying EXE directly.

## File-level comparison

Compared with the extracted `FANTASY` release, 16 common files are identical:
nine sound drivers (`ADLIB`, `GUS`, `INTERNAL`, `NOSOUND`, `SB20`, `SBLASTER`,
`SBPRO`, `SM2`, and `THING`), all five table/music modules (`MOD2.MOD` and
`TABLE1.MOD` through `TABLE4.MOD`), and `TABLE3.PRG`/`TABLE4.PRG`.

File-set changes:

| Change | Details |
|---|---|
| Renamed | `PINBALL.EXE` -> `PF.EXE`; content is identical |
| Added | `PINBALL.BAT` (82 bytes) |
| Added | `SOUND.CFG` (20 bytes), selecting `SB16.SDR` |
| Missing | `TIMER.BIN`; no plain-text reference to it was found in the alternate binaries |

Changed common files:

| File | FANTASY | FANTASYA | Finding |
|---|---:|---:|---|
| `INTRO.MOD` | 252,870 | 252,870 | Only the final two bytes differ: `2B 3F` -> `2D F9`; alternate is identical to Deluxe |
| `INTRO.PRG` | 345,678 | 345,038 | Distinct intro build; see below |
| `PAS16.SDR` | 10,712 | 10,728 | Different/relinked driver build, one paragraph larger |
| `SB16.SDR` | 11,349 | 11,349 | Different driver build; 1,227 byte positions differ |
| `SETSOUND.EXE` | 5,652 | 27,347 | Replaced by the exact Deluxe copy, identifying itself as FLD setup V2.01 (1995) rather than V1.00 (1993) |
| `TABLE1.PRG` | 536,822 | 536,838 | Rebuilt executable, 16 bytes larger |
| `TABLE2.PRG` | 517,974 | 517,990 | Rebuilt executable, 16 bytes larger |

SHA-256 values for the distinct alternate files:

| File | SHA-256 |
|---|---|
| `INTRO.MOD` | `f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613` |
| `INTRO.PRG` | `2f6addcbf410e60abf585a3af1bbad0f6cb5a85c84af01a56a046c72eca6f98a` |
| `PAS16.SDR` | `0fa6033eecfdcd2f675af779f370ea8dd74b50747b8f6beb83b3bb7dedaa5589` |
| `SB16.SDR` | `3fb857a95c077fe91e621fde95090a2538823e5c6bbc1873aa983bf9103138ad` |
| `SETSOUND.EXE` | `344831fc055b790518d5cbfedd635aa0d86d12a51e615b017604ed9c1d847d94` |
| `TABLE1.PRG` | `3b897533f11163934b8e4da038143e8f7339224421803ab4108c9d10f0a7bb4a` |
| `TABLE2.PRG` | `37019f7bd41d896a8f5a6383a2dffc3b3e4fc63fdf1aec1cc15db99110e581eb` |

### What changed in the intro

This is a genuine alternate intro build, not the old file with a footer or
copyright string appended.

- The MZ load image is 640 bytes smaller and has a different entry point
  (`3620:0001` instead of `364C:0005`).
- The memory-error text was rewritten. The known floppy intro asks for 560K
  with sound or 530K for GUS/no sound; this intro reports 555 kbytes or 520
  kbytes respectively.
- The old embedded anti-cracking message (`HI HACKER!...`) is absent.
- The options-loading startup code was changed substantially. The known
  floppy build falls back by setting only the Scrolling byte. The alternate
  build validates all six fields and resets all six to defaults if the load
  fails or a value is out of range.
- That six-byte options structure moved from `DS:49A3` to `DS:4846`.
- The manual-check mechanism still reads the last two bytes of `INTRO.MOD` at
  offset 252,868. A runtime test observed the read and pfemu's existing
  `20 01` substitution, so the changed original sample-tail bytes do not
  matter to the patch.

The current `fantasies_patch_intro()` pattern (`JNC +5; MOV [x],1`) is absent,
so its scrolling-clobber NOP is not applied. That is harmless for this build:
the alternate startup code has replaced the one-field clobber with the
six-field validation/default block. The separate options interceptor is not
harmless, however. It forces the open to fail and writes to `DS:49A3`; the
game then defaults the real structure at `DS:4846`. Launcher choices are lost,
and the erroneous write lands in the in-memory credits strings, overwriting
the tail of `DESIGN` and the beginning of `PROGRAMMING`. It did not crash the
short smoke run, but it is still an invalid memory write.

For reference, binary inspection finds three intro layouts already present in
the tree:

| Intro build | Options structure |
|---|---:|
| Known floppy (`FANTASY`) | `DS:49A3` |
| Power Pack alternate (`FANTASYA`) | `DS:4846` |
| Deluxe (`FANTASYDX`) | `DS:48D7` |

This shows that `PINBALL_CFG_BUF_OFFSET` is not release-independent.

### What changed in Tables 1 and 2

`TABLE1.PRG` and `TABLE2.PRG` are each one paragraph (16 bytes) larger. Their
entry IPs move from `2F96` to `2F9B` and from `277C` to `279A`, respectively,
and references/code in the early executable image are relinked. The changed
area contains executable code, including VGA-register I/O; this is not merely
trailing padding. A complete semantic disassembly was not needed for the
compatibility result and no gameplay-visible purpose is assigned here.

The large resource/data tails remain byte-identical after the 16-byte shift:

- Table 1: old file offset `0x1E33A` equals alternate offset `0x1E34A` through
  EOF (413,116 bytes).
- Table 2: old file offset `0x1D3A4` equals alternate offset `0x1D3B4` through
  EOF (398,258 bytes).

`TABLE3.PRG` and `TABLE4.PRG` are entirely byte-identical to the known floppy
files.

## Patch compatibility

The following results combine a post-relocation static scan with short runtime
loads using the current `pfemu.exe` and a temporary copy of the directory.

| Behavior/patch | Result | Evidence |
|---|---|---|
| Fantasies session gating | **Fail for normal boot** | `PF.EXE` logs `session not armed`; direct table/intro filenames do arm it |
| Manual-check read filter | **Pass when armed** | Runtime log: `manual check flag forced to 'answered' (Intro.Mod+252868)` |
| Launcher-options memory poke | **Fail** | Current `DS:49A3`; alternate uses `DS:4846` |
| Scrolling-default NOP | **Not matched, not needed** | Old signature is absent; alternate uses a different all-fields validation/default block |
| Pause-race fix | **Pass, all 4 tables** | Both unique signatures found; runtime addresses armed |
| Infinite-balls locator/toggle site | **Pass, all 4 tables** | One unique signature per table; runtime site located |
| Spring-control locator | **Pass, all 4 tables** | One unique signature and strong DATA-segment vote per table; runtime site located |
| Flipper lost-release repair | **Pass when armed** | Runtime direct table loads log `flipper fix on` |
| Ball-gap diagnostic locator | **Pass, all 4 tables** | Unique prologue/epilogue pair within the required routine window |
| SDR PLL patch | **N/A** | This unsafe optimization is disabled globally in `dos.c` |
| Deluxe `cd.nfo` interception | **N/A** | This is a flat floppy-family layout, not the Deluxe hard-disk/CD split |

Runtime locator results from direct table loads were:

| Table | Pause flag | `LAST_WAS_VB` | `SPRING_VALID` | Balls instruction |
|---:|---:|---:|---:|---:|
| 1 | `1C56F` | `04C5B` | `1D5EA` | `012FD` |
| 2 | `1B51E` | `0445A` | `1C752` | `01217` |
| 3 | `1AE40` | `03EE9` | `1BC8C` | `011A4` |
| 4 | `1937A` | `05407` | `1A482` | `012E1` |

These are run-specific linear addresses from identical direct-load process
layouts, included only to make the runtime result auditable. The patch code
does not hardcode them.

The supplied `SOUND.CFG` selects the alternate `SB16.SDR`. During the armed
intro smoke run, that driver loaded and executed without an immediate loader
or unsupported-opcode failure. The newer `SETSOUND.EXE` is identical to the
one already in `FANTASYDX`; a two-second direct smoke run entered its loader
and memory-allocation path, but no full interactive setup test was performed,
so this report does not claim that the setup utility is fully functional. It
is not on the normal path while the supplied `SOUND.CFG` exists.

## Boot tests

All tests were time-limited and used a temporary copy, so high-score/config
writes could not alter the supplied files.

| Invocation shape | Result |
|---|---|
| `-d <temp>/FANTASYA -nolauncher -secs 0.1` | Exit 1: `could not load PINBALL.EXE` |
| `-d <temp>/FANTASYA -p PF.EXE -secs 2 -t` | Launcher and intro execute, but log says `session not armed` |
| `-d <temp>/FANTASYA -p INTRO.PRG -secs 2 -t` | Session armed; options interception, alternate SB16 load, and manual-check filtering observed |
| `-d <temp>/FANTASYA -p TABLEn.PRG -secs 0.25 -t` | All four load; pause/spring/balls locators and flipper fix arm |

Directly booting `INTRO.PRG` or a table is useful for testing, but is not a
playable substitute for `PF.EXE`: the resident launcher normally sequences
the intro and selected table.

## Changes needed for first-class support

No implementation was made, but the required changes are small and localized:

1. Add `FANTASYA` as a launcher-visible installation whose boot program is
   `PF.EXE`.
2. Arm Fantasies behavior for that selected installation. Avoid accepting any
   arbitrary `PF.EXE` globally; tie the recognition to the known game choice,
   directory layout, and/or launcher hash.
3. Make the options-buffer location release-aware. Prefer signature-derived
   discovery from each intro's config load/validation code, with a uniqueness
   check, rather than adding another unconditional fixed offset.
4. Treat the alternate intro's missing scrolling-NOP signature as an expected
   layout, not an error. Its own validation/default logic supersedes that old
   patch.
5. Re-run an end-to-end launch through `PF.EXE`, choose each table, exercise
   pause/resume and all trainer toggles, and verify launcher options after the
   startup/session fixes are implemented.

Until items 1-3 are addressed, the accurate support status is: **the CPU/DOS/
VGA emulation and table patch signatures are compatible, but the Power Pack
installation is not yet supported as a normal patched game selection.**
