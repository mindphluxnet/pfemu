# Pinball Fantasies version detection research

Date: 2026-09-16

## Implementation status

Implemented in `src/release.c` (detector and release database), `src/reltable.h`
(generated manifests), `tools/mkreltable.py` (the generator), and the launcher,
`main()` and `src/fantasies.c` changes this document asks for. `pfemu.exe
-releases` prints the report described under "Launcher UX after detection".

Two things were settled during implementation that this research left open, and
one correction:

- **The options-buffer offsets are derived, not stored as the primary source.**
  Both `LOAD_TOGGLAREN` and `SAVE_TOGGLAREN` set up the same six-byte transfer,
  `MOV CX,6 / MOV DX,<buf> / MOV AX,3F00h (or MOV AH,40h) / INT 21h`. Scanning
  the loaded image for those two shapes finds exactly two sites in each
  collected release, and both agree on the address: `49A3` (floppy), `4846`
  (Power Pack), `48D7` (Deluxe). The detected release's recorded offset is kept
  as a cross-check; disagreement, ambiguity, or no match at all leaves the
  buffer unknown and nothing is poked. No relocation-table entry in any of the
  three intros lands inside these signatures, so a scan over the relocated
  image sees the same bytes the file does.

- **Correction: Deluxe has the six-field validation block too.** The table
  under "Release-specific runtime metadata" records only that Deluxe lacks the
  old scrolling-NOP signature. It does have the same all-fields
  validate-or-default block that Power Pack has, at `INTRO.PRG+0x366DD`
  (Power Pack: `+0x36609`); the only difference is that Deluxe spells the
  Scrolling range check `JAE` where Power Pack spells it `JA`.

- **That block has to be defused, or the options poke is pointless on two of
  the three releases.** Because the interception deliberately makes the
  boot-time `PINBALL.CFG` open fail, the `JC` at the top of that block is
  always taken, and it re-defaults all six fields - overwriting everything the
  launcher just poked. NOPing that leading `JC` fixes it: execution falls into
  the range checks instead, which the launcher's values pass (it never writes
  an out-of-range value), and the block's own `JMP` then skips the defaults.
  Values that somehow are out of range still get defaulted, which is the
  wanted behaviour anyway. The floppy build keeps the existing narrower fix,
  its one-field `MOV S_SCROLLING,1` clobber NOPed. Which of the two a release
  uses is recorded in its descriptor, so a missing signature is a warning where
  one was expected and an expected silence where it was not; both scans run
  regardless, so a release not yet in the database still gets whichever of the
  two it actually contains.

One deviation from the recommendation below: `GAME` is the documented place to
put an installation, but it is not the only one. Any other top-level directory
containing an `INTRO.PRG` is detected as well and listed in the launcher, so a
collection of releases can sit side by side. This does not reintroduce
directory-name identity - the list is a list of *places*, each one labelled by
what its hashes say it holds, and a directory that fails detection is shown
with its failure and cannot be launched.

## Purpose

pfemu currently identifies a release indirectly from fixed directory names
(`FANTASY` and `FANTASYDX`) and the selected boot filename. That is no longer
adequate: the repository now contains three materially different releases,
one of which renames the launcher and mixes components from the other two.

This document inventories the three collected releases and specifies a
checksum-based detection approach for a later implementation. It makes no code
changes.

## Recommendation in one paragraph

Ask the user to place one installation's files directly in a single directory,
preferably `GAME`. At launcher startup, hash `INTRO.PRG` and
`TABLE1.PRG`-`TABLE4.PRG` with SHA-256 and match the resulting five-file vector
against a compiled-in release database. `INTRO.PRG` is already unique across
all three collected releases, but checking all five programs confirms that the
installation is coherent rather than a partial or accidental mixture. The
matched release record supplies the boot filename, intro options-buffer
layout, and release flags; game-specific behavior must no longer be armed from
the directory basename. Unknown extra files are allowed, mutable files are not
part of identity, and an unknown or mixed code vector produces a diagnostic
rather than a guessed version.

## Collected releases

| Stable ID | User-facing label | Current directory | Files | Bytes | Boot file |
|---|---|---|---:|---:|---|
| `floppy` | Original floppy archive build | `FANTASY` | 25 | 3,716,060 | `PINBALL.EXE` |
| `power_pack` | Pinball Power Pack (1996) | `FANTASYA` | 26 | 3,737,012 | `PF.EXE` via `PINBALL.BAT` |
| `deluxe` | Pinball Fantasies Deluxe CD-ROM (1995) | `FANTASYDX` | 24 | 3,710,787 | `PINBALL.EXE` |

The Power Pack label and year come from the archive's external
`21STINFO.DAT`, which was one directory above its `FANTASY` directory:

> Pinball Power Pack. (c) 21st Century Entertainment. Ltd. 1996

That metadata file is not present in `FANTASYA`, is not referenced by its
runtime files, and is not needed for detection. See [FANTASYA.md](FANTASYA.md)
for the detailed binary and patch analysis.

### Lineage summary

- `power_pack` is a floppy-family release. Its `PF.EXE` is byte-identical to
  `floppy`'s `PINBALL.EXE` and is not the Deluxe launcher.
- `power_pack` has its own `INTRO.PRG`, Table 1, Table 2, PAS16 driver, and
  SB16 driver. Its Table 3 and Table 4 programs are identical to `floppy`.
- `power_pack` takes `INTRO.MOD` and `SETSOUND.EXE` byte-for-byte from the
  same variants collected with `deluxe`.
- All table music, `MOD2.MOD`, and most sound drivers are identical across all
  three releases.
- `deluxe` has a distinct launcher, intro, and all four table programs.

This mixed lineage is why the version should describe a known **code vector**,
not be inferred from one driver, module, filename, date, or setup utility.

## Which artifacts are good identifiers?

| Artifact | Identification value | Reason |
|---|---|---|
| `INTRO.PRG` | Excellent primary key | Different SHA-256 in all three collected releases; also selects the intro-specific memory layout |
| `TABLE1.PRG` | Excellent confirmation | Different in all three releases |
| `TABLE2.PRG` | Excellent confirmation | Different in all three releases |
| `TABLE3.PRG` / `TABLE4.PRG` | Good coherence check | `floppy` and `power_pack` share them; Deluxe differs |
| Boot EXE contents | Secondary check | Separates Deluxe from the floppy family, but `floppy` and `power_pack` share the same bytes under different names |
| Boot filename | Poor identifier | `PINBALL.EXE` was renamed to `PF.EXE` in Power Pack |
| `INTRO.MOD` | Poor identifier | Deluxe and Power Pack share it; only two sample-tail bytes differ from floppy, and the game writes those bytes |
| Other `.MOD` files | Integrity only | Identical across all collected releases and therefore carry no version information |
| Sound drivers | Poor identifier | Mostly shared; Power Pack is a mixture rather than a single driver generation |
| `SETSOUND.EXE` | Poor identifier | Power Pack and Deluxe share it, and it is not needed during an ordinary configured launch |
| File size alone | Prefilter only | Sizes distinguish the current samples but are not collision-resistant |
| Timestamps | Never use | Extraction/repackaging rewrites them; current directories contain 1995, 1996, and 2026 dates |
| Directory name | Never use for identity | It is exactly the user knowledge the proposed design should eliminate |

## Exact code fingerprints

All hashes below are SHA-256 over the complete file and are shown in lowercase.
Sizes are decimal bytes.

### `floppy`

```text
INTRO.PRG   345678  619723e39acc003c64ae5f10159ae9da6192a28642c348f455bac447a1184967
TABLE1.PRG  536822  4d7a69e7dc95260ad2541c6981a11ab842e2f1f20e45447e5613688b86e38414
TABLE2.PRG  517974  6689dcef5fd051998bab990b5d243614c7dae2dcdcab9bffbe3c1936a76504b5
TABLE3.PRG  504758  da83ef5a7a471e6a6ad759126907076c81e92ffde6dec8e3de8e6052c6a98858
TABLE4.PRG  522198  88f63edd4c7b50bd057397016d7aa962f0ed1c858f4a746f1ccf976f67494ebf
```

Collected boot artifact:

```text
PINBALL.EXE    1742  e7acf53b1af353bd44805579f49a594bf33d243cb8c50a23f0d5c50bb75765ba
```

### `power_pack`

```text
INTRO.PRG   345038  2f6addcbf410e60abf585a3af1bbad0f6cb5a85c84af01a56a046c72eca6f98a
TABLE1.PRG  536838  3b897533f11163934b8e4da038143e8f7339224421803ab4108c9d10f0a7bb4a
TABLE2.PRG  517990  37019f7bd41d896a8f5a6383a2dffc3b3e4fc63fdf1aec1cc15db99110e581eb
TABLE3.PRG  504758  da83ef5a7a471e6a6ad759126907076c81e92ffde6dec8e3de8e6052c6a98858
TABLE4.PRG  522198  88f63edd4c7b50bd057397016d7aa962f0ed1c858f4a746f1ccf976f67494ebf
```

Collected boot artifact (same content as `floppy`, different name):

```text
PF.EXE          1742  e7acf53b1af353bd44805579f49a594bf33d243cb8c50a23f0d5c50bb75765ba
```

### `deluxe`

```text
INTRO.PRG   317262  143e17b3515fb2d47259d19fd8053d99a61f07fae6c2f3dcf9bd6ffd7e5d53a1
TABLE1.PRG  537030  bf6057d58f22ac31526007ee8669fce210023ccc43f64c8659596b5cbf7daa85
TABLE2.PRG  518198  47efd95bdaae6a410cdda3e032ffff447cb62428f06e3b6f94ec5034d1411e5c
TABLE3.PRG  504950  413e6c70d6a22629f903eaec6c1af10366b68beaedd93a4b6df489044359e0fd
TABLE4.PRG  522422  805953261e89b3e22360e7e9867c41f6a73dec4902700e723f919fc142670f60
```

Collected boot artifact:

```text
PINBALL.EXE    2595  1e8c77504828eebad8452e89e8ea41142a192ca908e5600bde2ea9ce10ad1f6c
```

### Minimum and preferred matches

Among the current samples, the `INTRO.PRG` hash is a sufficient unique key.
The implementation should nevertheless use these levels:

1. **Candidate:** `INTRO.PRG` matches a known release.
2. **Recognized:** all five program hashes match that release's vector.
3. **Runnable:** the expected boot artifact and required common runtime payload
   are also present.
4. **Exact collected manifest:** every distributed immutable file matches.
   This is useful diagnostic information, but should not be required to play.

This distinction lets the launcher say, for example, “Power Pack code
recognized, but `TABLE2.PRG` is from the original floppy release” rather than
the unhelpful “game not found.”

## Release-specific runtime metadata

Detection should return a release descriptor, not just a display string.
Known per-release behavior is:

| Stable ID | Boot basename | Options buffer | Old scrolling NOP | Fake `cd.nfo` |
|---|---|---:|---|---|
| `floppy` | `PINBALL.EXE` | `DS:49A3` | Required/matches | No |
| `power_pack` | `PF.EXE` | `DS:4846` | Not present and not needed | No |
| `deluxe` | `PINBALL.EXE` | `DS:48D7` | Old signature not present | Yes |

The current unconditional `DS:49A3` options write is already known to be
wrong for the latter two layouts. The future implementation may derive the
buffer by a uniqueness-checked signature instead of storing these offsets,
but the detected release should still be available as a guard and diagnostic.

Session arming should become an explicit consequence of a recognized release:

```text
recognized Pinball Fantasies release
    -> session armed
    -> release metadata available to intro-specific behavior
    -> signature-based table patches remain self-checking
```

It should no longer depend on `dir == "FANTASY"` or a small list of program
basenames.

## Proposed detector behavior

### 1. Directory policy

- Use one documented directory, preferably `GAME`, for interactive launcher
  operation.
- Ask users to put the game files directly in it, not the archive's enclosing
  `FANTASY`/`PFD` directory.
- Keep `-d DIR` as a development/advanced override, but run the same detector
  in that directory.
- Scan only the selected directory's top level. Ignore `PFEMU-STATE` and all
  other subdirectories.
- Match DOS filenames case-insensitively.
- Extra top-level files such as `PINBALL.BAT` or `21STINFO.DAT` must not make a
  known installation fail.

A useful error nicety would detect the common mistake `GAME/FANTASY/INTRO.PRG`
and tell the user that the files appear to be one directory too deep. It
should not silently recurse and choose among multiple candidates.

### 2. Hash algorithm

Use SHA-256 plus an exact file-size check.

- SHA-256 makes the release database unambiguous and produces hashes users can
  report when a fourth version appears.
- File size is a cheap prefilter, not a substitute for the hash.
- Do not use the installer's CRC-16 values as identities; 16 bits are too weak
  for a growing release database.
- Hashing the five code files reads about 2.4 MB, which is negligible at
  launcher startup. Hashing the entire approximately 3.7 MB installation for
  optional integrity diagnostics is also cheap.

The implementation choice—Windows CNG/BCrypt or a small local SHA-256
implementation—is left to the coding stage. Store and compare the 32-byte
digest in binary; use lowercase hex only for logs and UI diagnostics.

### 3. Matching sequence

1. Build a case-insensitive map of top-level basenames. Reject duplicate names
   that differ only by case.
2. Require `INTRO.PRG` and compute its size/hash.
3. Look up the intro hash in the known release table.
4. Hash `TABLE1.PRG` through `TABLE4.PRG` and compare all four against the
   candidate record.
5. Find and validate the expected boot artifact. The release record can name
   the collected basename; checking its hash prevents an unrelated file with
   that name from being executed.
6. Validate the runtime payload and report missing/corrupt files separately
   from version identity.
7. Return a release descriptor to the launcher and game-specific layer.

Do not select whichever release has the highest number of matching files. A
conflicting vector is a mixed/modified installation and should be reported as
such. Silent best-fit selection risks applying an intro memory layout to the
wrong executable.

### 4. Result states

The detector should distinguish at least:

| State | Meaning | Launcher action |
|---|---|---|
| `recognized` | Known five-file code vector and valid boot file | Show detected label and allow Launch |
| `incomplete` | Known intro/version, but required file is missing | Disable Launch; name the missing files |
| `modified` | Known intro, but one or more expected code hashes differ | Disable Launch by default; show expected and actual hashes |
| `mixed` | Code files independently match different known releases | Disable Launch; list each file's detected origin |
| `unknown` | Intro hash is not in the database | Disable patched launch; print the five sizes/hashes for collection |
| `ambiguous` | Duplicate case-insensitive names or multiple boot candidates | Disable Launch; ask the user to clean the directory |

An expert-only unpatched override could be considered later, but detection
must never silently apply hardcoded memory writes to an unknown intro.

## Mutable and optional files

Whole-directory hashing would reject valid used installations. These paths
must not participate in release identity:

- `PFEMU-STATE/` and everything beneath it;
- `PINBALL.CFG`, `SOUND.CFG`, `*.HI`, and pfemu's own option files;
- wrapper/metadata files such as `PINBALL.BAT` and `21STINFO.DAT`;
- screenshots, logs, and unrelated readme files.

`INTRO.MOD` deserves special handling. All three collected copies are
252,870 bytes. Their first 252,868 bytes are identical:

```text
SHA-256(first 252868 bytes) =
0051a695e1c91806d24198a7258ff038ddc69e8bd57886e30004985376348009
```

Only the last two bytes differ in the pristine collected files:

| Release | Tail | Full-file SHA-256 |
|---|---|---|
| `floppy` | `2B 3F` | `e871ab7cc8f406c329c5b1842c564c439ddb8c1232b2e50720648a477e5ca399` |
| `power_pack` | `2D F9` | `f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613` |
| `deluxe` | `2D F9` | `f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613` |

The game writes its manual-check sentinel to those final bytes. Therefore:

- never use the full `INTRO.MOD` hash for release identity;
- validate its length and, if desired, hash only the first 252,868 bytes;
- continue using the write overlay so a pristine top-level copy stays stable.

`SETSOUND.EXE`, `TIMER.BIN`, the non-selected sound drivers, `PINBALL.BAT`, and
an installed `SOUND.CFG` are distribution/utility details. Record them in an
“exact manifest” diagnostic, but do not let them override a coherent code
vector. The ordinary pfemu launcher only needs the drivers it will select
(currently `SBLASTER.SDR` for sound and `NOSOUND.SDR` for silence).

## Shared immutable payload

These 14 top-level files are byte-identical in all three collected releases.
They have no version-discrimination value, but their hashes are useful for
post-detection integrity/completeness checks.

| File | Bytes | SHA-256 |
|---|---:|---|
| `ADLIB.SDR` | 10,966 | `19d2e5e7f6b4b0b11d9b6ae8070dc558a0303d56647142966ec2713b4a805724` |
| `GUS.SDR` | 10,502 | `b32e0306634bb9285ff402e2985eeebe66b2eff09a0c313b7fe5c9960e1d42ac` |
| `INTERNAL.SDR` | 10,711 | `281e62186567dbf3bc670d397ccf7fbfba37eee5844440796856c0e1d39a6fe8` |
| `MOD2.MOD` | 55,394 | `aa5003c275b494062f37f44e8c77105b8a420555f4bd6ff53d7698f89c540f21` |
| `NOSOUND.SDR` | 2,883 | `223fdd845fa8541e2801c3371f2f97a61244e4ea54bcd81f2a51d312c005366d` |
| `SB20.SDR` | 11,814 | `4cba5dff50926c9c2485d5242e935a4bc4d134b1d8b8fce4db0d5faa0e044e9c` |
| `SBLASTER.SDR` | 11,421 | `c6c016f94f985c0f70d6bba7acb7e4572db8bbf88d99b2714aa150313be4b21f` |
| `SBPRO.SDR` | 11,886 | `3dfdea95cd3cd7c2096a4bf59fcf0af4802e20ca1879e149c9f1f39f57e27f20` |
| `SM2.SDR` | 11,348 | `d305e3401899af6d4938c09f85d0a1dc0aec0361b6ce8cb434933bdd298208b8` |
| `TABLE1.MOD` | 210,760 | `a0877e4372abe64b70d9e361bf257ea5a84c948771f0eace3433d5f6399060b5` |
| `TABLE2.MOD` | 211,912 | `728629c54311386781271308e181ac0435f0582e90870accff0a42270d467529` |
| `TABLE3.MOD` | 219,668 | `fb7bfd1c96a462cb03999d2e6f843a20d3de69ba05fcbd384a9f1c131b9a563a` |
| `TABLE4.MOD` | 216,418 | `31ad7e671ae77c07c3d075e2f1fecd3d918fd921fa23acd9a1b0b6fc07fbbcea` |
| `THING.SDR` | 10,369 | `c9cd962aae321ce1e2637287a559a7d2aa6032c468ae6d794c3a7f4e3e491836` |

## Other version-varying artifacts

These files complete the collected manifests but should not be primary keys.

### `floppy`

```text
INTRO.MOD      252870  e871ab7cc8f406c329c5b1842c564c439ddb8c1232b2e50720648a477e5ca399
PAS16.SDR       10712  e863cb16e9e30cc2506638b7f91a2a31e26d19c242429af065ad455554352fd2
SB16.SDR        11349  81a7c4d41a14ae042a63899b38e88bc4d33a547053f44b17b3bd324ebe7bab17
SETSOUND.EXE      5652  21f3e5477299f4bcedce7dea2d9a7d7c940dcade9e183b4b38ee5ad2e1c37a34
TIMER.BIN          253  783f88891a760b3fab648a7ae64ad8998e1349737df07a1e765329f7a6787d0c
```

### `power_pack`

```text
INTRO.MOD      252870  f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613
PAS16.SDR       10728  0fa6033eecfdcd2f675af779f370ea8dd74b50747b8f6beb83b3bb7dedaa5589
SB16.SDR        11349  3fb857a95c077fe91e621fde95090a2538823e5c6bbc1873aa983bf9103138ad
SETSOUND.EXE     27347  344831fc055b790518d5cbfedd635aa0d86d12a51e615b017604ed9c1d847d94
PINBALL.BAT         82  1d5d8ea1e82c74190b97e6c98f73dc6ebbd1488c2b880f9871380147f0bf50e4
SOUND.CFG           20  9a85e58c455bc250474b5555f55edd926dfa522233378afef84eea6de65155d4
```

### `deluxe`

```text
INTRO.MOD      252870  f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613
PAS16.SDR       10712  e863cb16e9e30cc2506638b7f91a2a31e26d19c242429af065ad455554352fd2
SB16.SDR        11349  81a7c4d41a14ae042a63899b38e88bc4d33a547053f44b17b3bd324ebe7bab17
SETSOUND.EXE     27347  344831fc055b790518d5cbfedd635aa0d86d12a51e615b017604ed9c1d847d94
```

The three lists above plus the shared-payload table, boot artifact, and
five-file code vector account for every top-level file in each collected
directory.

## Launcher UX after detection

There should be no release radio buttons. The launcher can show one read-only
line, for example:

```text
Detected game: Pinball Power Pack (1996)
```

If detection fails, replace that line with a specific result and disable the
Launch button. A copyable details view or trace should include:

- scanned absolute directory;
- detector state;
- detected origin of each of the five code files, if any;
- actual size and SHA-256 of every unknown/mismatched code file;
- missing required filenames;
- ignored extra files.

This output is also the intake format for newly discovered releases: the user
can provide five hashes and sizes before any binary is run or any patch is
considered.

## Suggested implementation tests

1. Copy each collected release, unchanged, into `GAME`; assert its stable ID,
   label, boot basename, and runtime metadata.
2. Rename the directory itself; detection must not change.
3. Change filename case; detection must still work.
4. Add `21STINFO.DAT`, readmes, logs, and a populated `PFEMU-STATE`; detection
   must still work.
5. Replace one table program with the corresponding file from each other
   release; result must be `mixed`, never best-fit `recognized`.
6. Flip one byte in `INTRO.PRG`; result must be `unknown` or `modified`, and no
   intro-specific memory poke may run.
7. Remove every required file in turn; result must name that file and keep
   Launch disabled.
8. Give `INTRO.MOD` each collected tail and the game's `20 01` sentinel;
   identity must remain unchanged and prefix validation must pass.
9. Add two top-level names differing only by case; result must be `ambiguous`.
10. Put a valid install one directory too deep; the diagnostic should explain
    the likely layout error without silently recursing.

## Adding future discoveries

For every newly collected version, preserve and record:

1. archive name, source URL/item identifier, and any external version text;
2. unmodified top-level filename, size, and SHA-256 manifest;
3. the five-file code vector separately;
4. boot filename and boot hash;
5. `INTRO.MOD` full hash, final two bytes, and prefix hash;
6. intro options-buffer layout or a successful uniqueness-checked signature;
7. results of every existing table patch signature scan;
8. a temporary-copy boot trace.

A new release should be added as a new descriptor. Existing hashes must never
be broadened with fuzzy matching merely to make an unknown build launch.
