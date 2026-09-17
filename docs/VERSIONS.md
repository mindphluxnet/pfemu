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
containing the anchor program of a known layout is detected as well and listed
in the launcher, so a collection of releases can sit side by side. This does
not reintroduce directory-name identity - the list is a list of *places*, each
one labelled by what its hashes say it holds, and a directory that fails
detection is shown with its failure and cannot be launched.

A fourth release, added later, forced one structural change: **the code vector
is not always five files**, and is not always named the same. See
"[The 1993 demo](#the-1993-demo-a-different-shape)" for what a `demo`
installation is and what had to move to accommodate it.

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
| `demo` | Pinball Fantasies 5 Min Demo (1993) | `FANTDEMO` | 22 | 1,333,785 | `PFDEMO.EXE` |

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
- `demo` is its own branch, and the earliest artifact collected so far (file
  dates 15-23 November 1993). It ships one intro and one table, both renamed,
  both LZEXE-compressed, and its own generation of every sound driver - but
  `TABLE1.MOD`, `INTRO.MOD` and `TIMER.BIN` are byte-identical to the full
  game's. Its own `INSTALL.BAT` calls it the "Pinball Fantasies 5 Min Demo".

This mixed lineage is why the version should describe a known **code vector**,
not be inferred from one driver, module, filename, date, or setup utility.

## Which artifacts are good identifiers?

| Artifact | Identification value | Reason |
|---|---|---|
| `INTRO.PRG` / `DEMO.PRG` | Excellent primary key | Different SHA-256 in every collected release; also selects the intro-specific memory layout. Which of the two names is the anchor is decided by the layout, below |
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

### `demo`

```text
DEMO.PRG    105826  434ed39e78bf9cf1025cbf1ab107bd88832a0284c6e95a8703785d1c3420e541
PLAND.PRG   196594  582467262ba26f1d58ae71e62dc00494fd42c2d420c5fd85a8f0449d784601c4
```

Collected boot artifact:

```text
PFDEMO.EXE     1740  162db89a771e7bc9987943db21fb974c83fae5b1296b1e4a041980677510975a
```

Both programs are LZEXE 0.91 containers, so these hashes are of the packed
files as distributed - which is the right thing to hash, and also the only
thing a user can hash without tooling. What is inside them is recorded under
"[The 1993 demo](#the-1993-demo-a-different-shape)".

### Minimum and preferred matches

Among the current samples, the anchor program's hash is a sufficient unique
key. The implementation should nevertheless use these levels:

1. **Candidate:** the anchor program matches a known release.
2. **Recognized:** every program hash in that release's vector matches.
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

| Stable ID | Layout | Boot basename | Options buffer | Old scrolling NOP | Fake `cd.nfo` |
|---|---|---|---:|---|---|
| `floppy` | `full` | `PINBALL.EXE` | `DS:49A3` | Required/matches | No |
| `power_pack` | `full` | `PF.EXE` | `DS:4846` | Not present and not needed | No |
| `deluxe` | `full` | `PINBALL.EXE` | `DS:48D7` | Old signature not present | Yes |
| `demo` | `demo` | `PFDEMO.EXE` | None - see below | Neither variant present | No |

A recorded options buffer of `0` means *this build has none*, not *not yet
known*: the demo's intro carries neither the F5 options menu nor the
`PINBALL.CFG` behind it, so a signature match there would be a contradiction
and `fantasies.c` reports it as one rather than poking.

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
2. Decide which *layout* the directory holds, by which layout's anchor program
   is present (`INTRO.PRG` for `full`, `DEMO.PRG` for `demo`). Holding both is
   `ambiguous`, not a preference to be resolved.
3. Compute the anchor's size/hash and look it up among the releases that use
   that layout.
4. Hash the rest of that layout's programs and compare them against the
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
| `unknown` | Anchor hash is not in the database | Disable patched launch; print the layout's sizes/hashes for collection |
| `ambiguous` | Duplicate case-insensitive names, or two layouts' anchors in one directory | Disable Launch; ask the user to clean the directory |

An expert-only unpatched override could be considered later, but detection
must never silently apply hardcoded memory writes to an unknown intro.

## Mutable and optional files

Whole-directory hashing would reject valid used installations. These paths
must not participate in release identity:

- `PFEMU-STATE/` and everything beneath it;
- `PINBALL.CFG`, `SOUND.CFG`, `*.HI`, and pfemu's own option files;
- wrapper/metadata files such as `PINBALL.BAT` and `21STINFO.DAT`;
- screenshots, logs, and unrelated readme files.

`INTRO.MOD` deserves special handling. All four collected copies are
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
| `demo` | `2D F9` | `f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613` |

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

These 14 top-level files are byte-identical in the three `full`-layout
releases. They have no version-discrimination value, but their hashes are
useful for post-detection integrity/completeness checks.

The demo shares only three files with them - `TABLE1.MOD` (identical),
`INTRO.MOD` (identical, `2D F9` tail) and `TIMER.BIN` (identical to
`floppy`'s). Every one of its sound drivers is a separate, earlier and smaller
build, and it ships no `SB16.SDR`, `PAS16.SDR`, `MOD2.MOD` or `TABLE2-4.MOD`
at all.

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

### `demo`

```text
INTRO.MOD      252870  f36beae00efec1dd9e1c4e977bea264b7ec41ab18f258528ab66577a9ec66613
TABLE1.MOD     210760  a0877e4372abe64b70d9e361bf257ea5a84c948771f0eace3433d5f6399060b5
TIMER.BIN         253  783f88891a760b3fab648a7ae64ad8998e1349737df07a1e765329f7a6787d0c
SETSOUND.EXE    41264  89ca8441b878057386cfd5b2e79ed5aa62b02e17b16949789459663b4ad0ca7b
SOUND.CFG          16  99d5d1c0e0c2fe2f8fa8a08029290d1b5b99ae9c553d61be8e6a870d56f16a55
ADLIB.SDR        8179  480af0f38882cc864a793af1de3ecb89147cd2f31f934a0cbce2c25dea7799cf
GUS.SDR          6904  425e90b18c31cf3e41c803d6a74c9b04a04901a80f5ff43dda22cd80b737204a
INTERNAL.SDR     7986  5f2101f3c1ad0f0729a67b9ef8a33a881283ee46548602cf96fa0a586b3308a7
NOSOUND.SDR      2066  c47eddfd81795c8d1f4aa9fff16b8be9db06d5cabc63e90b247692b8afdd83aa
SB20.SDR         8295  a63e7a6295fe5be7b11901093314cde0d9f918a9c5fc5c412298c1263d2ffd9d
SBLASTER.SDR     8086  ebbd783386f76a844f60c54e10962b7ff59ec69ef6a034c0fa1043253216e966
SBPRO.SDR        8296  993216fc97978bdf043c707bd113bffc89f26bfcac0519f8f79960d81beaa313
SM2.SDR          7966  2e6109ea9630bb9621e53b50ea089c2541a17e5ecb2a7287c13e1485db9a2577
THING.SDR        7602  4bb91faff0040db0ba2667c3b279010c86e870a498c43430d934c826da97d026
INSTALL.BAT      1379  8a1c8e7de4d49390e7afc0fdfdca428cccea56e7ec5de7c22e150767b9834371
BILLION.PCX    103185  62f508cb1894d3812095c9c93cec6421e977b9d0255f6f31c3d0ba97ab3188ea
PARTY.PCX      110445  f0ba8f55d48ef94d41cd5eac148ada6f356e0b1e688eb1f5b57e9dd3f087a696
SPEED.PCX      101382  52ac7c70bf75ef3b44d99b2799906f379ceae5c5628fb1a3bc1d537c3f3269fa
STONE.PCX      142691  0b01bd641a72b1ecf2c4e99c9a1f47308ffbac56e4b7d4e44886cc98eb1497c8
```

The four `.PCX` screens are distribution-media artwork: the demo's own
`INSTALL.BAT` copies `*.sdr`, `*.mod`, `*.prg`, `*.exe` and `*.bin` to the hard
disk and not them, and none of the four programs names a `.PCX` file. They are
recorded as `RF_META` and ignored.

The four lists above plus the shared-payload table, boot artifact, and
code vector account for every top-level file in each collected
directory.

## The 1993 demo: a different shape

The demo is the first collected release that does not fit the assumption the
rest of this document was written under - that a release is five programs
called `INTRO.PRG` and `TABLE1-4.PRG`. Three things about it are structural
rather than cosmetic.

### One intro and one table, renamed

`FANTDEMO` ships `DEMO.PRG` (the intro) and `PLAND.PRG` (Party Land, the one
playable table). `PFDEMO.EXE` is the same family of loader as `PINBALL.EXE`;
its embedded program-name table still has five entries - `Demo.Prg`,
`pland.prg`, `Table2.Prg`, `Table3.Prg`, `Table4.Prg` - but only the first two
exist on the disk.

So the identity vector became a property of the distribution, not a constant.
`src/release.c` now carries a `CodeLayout` per shape (`full`: five programs;
`demo`: two), a release names the layout it uses, and a directory is assigned
a layout by which anchor program is in it. Nothing outside `release.c` knows a
program filename any more: `release_prog_slot()` answers "intro, table *n*, or
neither" and `fantasies.c` asks it instead of parsing `TABLE<n>.PRG`. Party
Land is table 1, which is also what its own `TABLE1.MOD` and `table1.hi` say.

### Both programs are LZEXE 0.91 containers

`DEMO.PRG` and `PLAND.PRG` carry `LZ91` at offset 1Ch and unpack to 235,150 and
535,958 bytes. That matters because every Fantasies fix locates its target by
scanning the freshly loaded image for a byte signature, and against a packed
image all of them match nothing.

Unpacked, `PLAND.PRG` matches all six table signatures exactly once each, at
offsets within about 40 bytes of `TABLE1.PRG`'s:

| Signature | `PLAND.PRG` | `TABLE1.PRG` (floppy) |
|---|---:|---:|
| pause-race A (`checkpause` resume) | `0x3339` | `0x3308` |
| pause-race B (`LATE_RASTER` tail) | `0x57D6` | `0x5814` |
| balls counter | `0x0BDC` | `0x0BE3` |
| `SPRING_VALID` | `0x38D6` | `0x38E5` |
| ball-gap A (`PUTTHEBALL` entry) | `0x4220` | `0x423B` |
| ball-gap B (`PUTTHEBALL` tail) | `0x4295` | `0x42B0` |
| dot-matrix `TIME_LEFT` | `0x42C6` | `0x42E1` |

So `src/lzexe.c` unpacks LZEXE 0.91 in the loader, and the demo gets every
table fix the full game gets. The implementation is a transcription of the stub
in `DEMO.PRG` itself rather than a general LZEXE tool, and it was verified
against an independent reference implementation: byte-identical images and
identical relocation lists for `DEMO.PRG`, `PLAND.PRG`, and the floppy
release's `SETSOUND.EXE` (which turns out to be LZEXE 0.91 as well - the only
file outside the demo that this path touches).

Two independent checks say the reconstruction is right rather than merely
plausible. `PLAND.PRG`'s 101 relocations land at almost exactly `TABLE1.PRG`'s
103 linear addresses and carry almost exactly its target segments
(`0x00925/19AD` against `0x00926/19B4`, and so on down the list), and none
points outside the image. And the reconstructed `minalloc` - the paragraphs the
packed header demanded, minus the unpacked image - comes out at 93 and 90
paragraphs, small positive numbers, which a wrong output length could not
produce.

The unpacked program sees what it would have seen anyway: the loader asks DOS
for exactly the paragraph count the packed header asked for, so the memory
block is identical, and `maxalloc` is carried over as `FFFFh` because that is
what the packed load would have used (LZEXE overwrites the original and it is
simply not in the file). `-nolzexe` forces the packed path for A/B testing;
every failure inside `lzexe.c` takes that path too, so this can only cost the
image patches, never the boot.

The demo's *sound drivers* are PKLITE-compressed, and those are left alone.
pfemu has nothing to match inside a `.SDR` - `fantasies_patch_sdr` is disabled
- so their stubs run as the guest's own code, which is the more faithful thing
to do where there is no reason not to.

### No options menu at all

`DEMO.PRG` contains neither the string `PINBALL.CFG` nor the F5 menu's labels
(`BALLS:`, `ANGLE:`, `SCROLLING:`, `INGAME MUSIC:`, `RESOLUTION:`,
`COLOR MODE:`), and neither of the two six-byte transfer signatures that locate
the options buffer in the other three intros matches anywhere in it. The demo
simply has no options structure, so its descriptor records `cfg_buf = 0`, the
launcher greys out the six option combo boxes for it, and nothing is poked.

It does still carry the manual-protection screen's strings, and its
`INTRO.MOD` is the same file with the same sentinel bytes, so
`fantasies_filter_read()` covers it unchanged.

### The sound drivers, and what PKLITE cost

Every `.SDR` in the demo is PKLITE-packed and an earlier, smaller build than
the full game's. That raised a question about the Quality setting, since the
five-entry mixing-rate table `src/launch.c` documents at image `0x27D3` in the
full game's driver is not visible in the packed file - and it turned out to be
answerable without unpacking anything.

**Every `.SDR` carries an uncompressed descriptor at the end.** The last four
bytes are a word holding the descriptor's own file offset, followed by the
ASCII marker `SP`; `SETSOUND.EXE` reads it to build its menu. It survives
packing because it sits past the end of the load image, so it can be read out
of a PKLITE'd driver as easily as a plain one:

| | demo `SBLASTER.SDR` | floppy `SBLASTER.SDR` |
|---|---|---|
| file size | 8,086 | 11,421 |
| descriptor at | `0x1CCF` | `0x29AF` |
| card names | SoundBlaster; ThunderBoard | + Pro Audio Spectrum |
| parameters | Base port address, IRQ number, **Sound quality** | the same three |
| quality choices | MEDIUM, HIGH, VERY HIGH, MAXIMUM | the same four |

So the demo's driver does have the Quality setting, and pfemu's `SOUND.CFG` is
the right shape for it. (Both drivers' help text says "there are two parameters
that the driver needs to know" and both then declare three; the prose is stale
in the full game too.)

The demo's `SETSOUND.EXE` is not compressed, and disassembling its config
writer gives the layout directly: a 14-byte driver name, then 3 bytes per
declared parameter, then a 2-byte per-driver tail (`100` for `NOSOUND`, which
is exactly what the shipped 16-byte `SOUND.CFG` contains). pfemu writes `00 00`
for that tail. The full game accepts it, so it is not why the demo failed to
boot, but it is not what `SETSOUND` would have written either.

### What actually broke the demo: `REP MOVS` overlap

The demo booted to a black screen, wedged inside the resident `SBLASTER.SDR`
on undecodable bytes. The driver's image in memory was corrupt before its
first instruction ran, and the cause was not in anything release-specific: a
long-standing bulk fast path in `strop()` (`src/cpu.c`) handed *overlapping*
forward `REP MOVS` to `memmove`, which is specified to do the opposite of what
the instruction does. That is precisely the LZ77 run-expansion idiom PKLITE
emits, so every packed driver decompressed subtly wrong.

Nothing in the three full releases had ever exercised it, because none of them
ships a compressed file the loader executes. `docs/OPTIMIZATIONS.md` §31 has
the byte-level evidence and the fix; `tools/reptest.c` is the regression test.

The order of discovery is the lesson worth keeping. The first log of this was
118 MB, of which 3,279,652 lines were one repeated message carrying only an
opcode byte and a `CS:IP` - which cannot distinguish an instruction pfemu
lacks from a decoder that has drifted mid-instruction. Bounding the report to
one line per distinct site and printing sixteen *bytes* separated those two
cases at a glance, and `-undefdump` supplied the rest: the program arrived
compressed, so guest memory was the only disassemblable copy of it.

## Where the demo stops: the title screen

The demo is supported but does not finish. It is detected, both programs
unpack, the sound driver loads and calibrates, `INTRO.MOD` plays, and the
publisher and developer screens draw. The screen that follows them is black
and the program never leaves the loop waiting for it. Scroll Lock still
exits. Nothing else in the release is affected, and no other release shares
the path.

It is a race in the demo's own code, not an emulation error, and the section
below records why - so that the next person to look does not re-derive it.

### What the demo is waiting for

`DEMO.PRG` (segment `1D93` as loaded) builds its title screen like this:

```text
07B1  out 3C4, 0F02                  ; map mask = all four planes
07B8  mov es, A000
07BD  mov cx, 8000h                  ; 32768 words
07C4  nop / stosw / nop / loop 07C4  ; clear the whole 64 KB VGA window
07C9  call 0BB9                      ; sound_off: 100x driver fn 8, then INT 66h AX=0018 BL=00
07CC  int 10h AX=0012                ; mode 12h, then its own CRTC/SEQ/GC/AR programming
0850  loop: call 1E61                ; draw a chunk
0875    mov ax,8 / pushf / lcall [0198]   ; service the driver
087D    cmp byte es:[0076],0 / je 0875    ; wait for the frame flag
0895  loop 0850                      ; CL is 1: the body runs once
08B3  call 0B96                      ; sound_on
```

The frame flag is `0040:0076`, a scratch byte in the BIOS data area. Exactly
one instruction in the whole demo sets it - `1D93:26C8` - and that instruction
lives inside a callback the program hands to the *sound driver* (`INT 66h
AL=0Bh`, `ES:DX = 1D93:269E`). The driver calls it from its timer ISR once per
frame; the same callback chain performs the `AR14` page flip, which is why a
failed run reports `AR14 switches=0`. Roughly fifty-five wait sites in the
program use this one flag.

The driver's ISR gates on `cs:[0106]`, which `INT 66h AL=18h` sets (`BL` non-
zero) and clears (`BL` zero). With it clear the ISR returns *without*
reprogramming its channel-0 one-shot, and since that one-shot is the only
thing that re-arms the timer, the timer stops, the callbacks stop, and the
flag can never be set again.

So the demo needs exactly one callback to land between its last consume of the
flag and the disable. It does not get one.

### Why it does not get one

The program runs with interrupts disabled and relies on the driver's own `sti`
- issued inside every `INT 66h` dispatch - to let them in. The VRAM clear at
`07C4` makes no driver calls, so for its whole duration nothing is delivered
and a single IRQ0 sits pending. Measured at the default 6 MIPS:

```text
t=22.422766  0040:0076 <- FF  by 1D93:26C8     last time the flag is ever set
t=22.422776  0040:0076 <- 00  by 1D93:0794     consumed by a wait site
t=22.422755  ISR arms one-shot, reload 8732 -> due 22.4301
t=22.450226  ISR finally runs, 20 ms late. Its latency compensation
             (`mov bx,[si+2] / sub bx,[si-7] / latch / in al,40 x2 /
              add bx,cx / sub bx,0Ah`) underflows to reload 52573 = 44 ms
t=22.454552  INT 66h AX=0018 BL=00 from 1D93:0BD8 -> cs:[0106] = 0
t=22.494288  one-shot fires; the ISR returns at the gate. Nothing again.
```

The one ISR that does run dispatches the *other* callback of the pair, not the
one that sets the flag.

Two conditions have to hold together, and they move in opposite directions
with emulated speed:

1. the stall must be shorter than the inter-event delta (~11089 ticks,
   9.29 ms) or the compensation underflows and schedules 44 ms out;
2. the pending event must fire before `sound_off`'s hundred service calls
   finish and clear the gate.

6 MIPS fails (1). 24 MIPS fails (2). 14.4 MIPS fails (2) with a perfectly sane
6194-tick reload. The lateness is not a fixed fraction of the loop - it is
wherever in the loop the interrupt happens to fall - so there is no speed to
tune to, and a fix that depended on landing inside a few-millisecond window
would not be a fix. Changing `-ips` also detunes the mixer and the driver's
PLL, which are calibrated against the default.

### What was ruled out

Each of these was a plausible cause, and each was eliminated by measurement or
by an independent reference rather than by argument:

- **The PKLITE decompression.** The full releases ship the same driver family
  *uncompressed*, so the demo's unpacked image can be checked against it. The
  ISR prologue and gate are byte-identical apart from the data segment and one
  jump displacement:

  ```text
  demo (PKLITE'd)   FC 60 06 1E B0 20 E6 20 FB  B8 9B 3C  8E D8  2E 80 3E 06 01 00  75 03  E9 AE 00
  floppy (plain)    FC 60 06 1E B0 20 E6 20 FB  B8 E2 01  8E D8  2E 80 3E 06 01 00  75 03  E9 BA 00
  ```

- **`REP MOVS` again.** The driver inserts a callback into its event list with
  `std; rep movsb` at `DI = SI + 9` - a backwards overlapping copy, the mirror
  of the bug in the section above. `tools/reptest.c` now covers both
  directions including that exact shift; all cases pass.
- **pfemu's interrupt scheduling.** The `[pit]` latency split reports batch
  overshoot mean 5 / max 10346 ticks against a 24042-tick stall, and names the
  blocker: `worst wait: due at 1D93:3BBE with IF=0`. The guest is holding
  interrupts off; pfemu is not delivering late.
- **The vertical retrace pulse**, which the driver's fallback watchdog counts:
  `vrs = 490`, `vre = (490 & ~0Fh) | 0Ch = 492`, two lines of 527 = 0.379%,
  against 0.348% of `3DAh` reads observed.
- **CPU starvation and the quality notch.** Quality 1 changes nothing, and
  `-prof`'s region view puts 42% of the run in the driver's dispatcher and
  vsync watchdog - which is the wait loop calling function 8, not a shortage
  of cycles.

### Two diagnostics this corrected

Both had been reporting confidently and wrongly, and both are worth knowing
about before trusting a future measurement:

- `-memwatch` was implemented on `mem_w8/16/32` in `src/vga.c`, which looks
  like the memory interface but only serves BIOS- and DOS-side writes. The CPU
  stores through `cpu_st8/16/32` in `src/cpu.c`, straight into `ram[]`, so the
  watch saw no guest stores at all and reported "written 0 times" for a byte
  the guest wrote 1239 times. It now sits on the CPU store path, and
  `strop()`'s bulk path reports separately so a block store cannot slip past.
- The PIT and IRQ0 health counters printed from the dot-matrix report in
  `src/fantasies.c`, behind `if(!mat_dbg || !mat_ticks)`. They describe the
  timer, not the dot matrix, and that guard made them unavailable for exactly
  the releases that never load a table. They now print from `dev_state_dump()`
  for every release, with no flag.

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
11. Put `INTRO.PRG` and `DEMO.PRG` in one directory; result must be
    `ambiguous`, never a choice between the two layouts.
12. Unpack an LZEXE program with an independent implementation and compare the
    image and relocation list byte for byte against `src/lzexe.c`; then boot
    the same release with and without `-nolzexe` and confirm only the image
    patches differ.

## Adding future discoveries

For every newly collected version, preserve and record:

1. archive name, source URL/item identifier, and any external version text;
2. unmodified top-level filename, size, and SHA-256 manifest;
3. the code vector separately, and which layout it is - a new set of program
   names needs a new `CodeLayout` in `src/release.c` and `tools/mkreltable.py`
   before anything else can be recorded;
4. boot filename and boot hash;
5. `INTRO.MOD` full hash, final two bytes, and prefix hash;
6. intro options-buffer layout or a successful uniqueness-checked signature,
   or a positive finding that the build has no options structure at all;
7. results of every existing table patch signature scan, run against the
   *unpacked* image if the programs are compressed;
8. a temporary-copy boot trace.

A new release should be added as a new descriptor. Existing hashes must never
be broadened with fuzzy matching merely to make an unknown build launch.
