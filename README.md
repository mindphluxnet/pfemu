# pfemu

A from-scratch 386 real-mode PC emulator that runs the original 1992 DOS
release of **Pinball Fantasies** natively on 64-bit Windows — no DOSBox, no
NTVDM (which 64-bit Windows doesn't ship anyway), no other emulator involved.

```
cpu.c   386 real-mode interpreter: eager flags, 16/32-bit op/addr size, full 0F group, REP string ops
vga.c   memory dispatch + VGA: text, planar write modes 0-3, latches, chain-4, unchained mode X, DAC
dev.c   8259 PIC pair, 8253 PIT, 8042-style keyboard, port map, CRT timing
bios.c  INT 10h/11h/12h/13h/15h/16h/1Ah, IRQ0/IRQ1, IVT set-up
dos.c   MCB chain, PSPs, file handles, FindFirst/Next, EXEC, TSR, .COM loading
sound.c 8237 DMA, Sound Blaster DSP, waveOut sink, WAV capture
main.c  Win32 window, presentation, keyboard, main loop, headless test harness
```

~3,600 lines of C, built with a single `cl /O2` invocation. It boots the
game's own launcher, plays the intro and table-select menu, and runs all four
tables, with music: an emulated 8237 DMA channel and Sound Blaster DSP carry
the game's own `.MOD` mixer output all the way through.

The two write-ups in [docs/](docs/) are the actual documentation:

- [`docs/WRITEUP.md`](docs/WRITEUP.md) — recovering the original installer's
  25 files from `FANTASY.ONE`/`FANTASY.TWO` by static analysis and emulation
  of its 8086 decompressor, with no reference material beyond the binaries
  themselves.
- [`docs/WRITEUP-PHASE2.md`](docs/WRITEUP-PHASE2.md) — building the emulator
  itself: the launch chain, every hardware quirk the game depends on, the
  bugs that came from each wrong assumption, and the debugging tools that
  found them.
- [`docs/OPTIMIZATIONS.md`](docs/OPTIMIZATIONS.md) — later rendering/timing
  fixes (e.g. the table-select palette flash).

No external references about the game, its file formats, or its copy
protection were consulted for any of this — everything was established by
reading the binaries or watching them execute.

## Requirements

- The original game files (not included here — see [Getting the game
  files](#getting-the-game-files) below), installed into a `FANTASY/`
  directory next to `pfemu.exe`.
- Windows, since the host side is a plain Win32 app (window, `waveOut`
  audio, no other dependencies).
- MSVC (the VS2019 Build Tools or later) to build from source.

## Building

```
build.bat
```

This calls `vcvars64.bat` and runs one `cl /O2` command over `src/*.c`,
producing `pfemu.exe` in the repo root.

## Getting the game files

`FANTASY/` isn't checked in. If you own the original disks/installer, extract
it with the recovery tool from phase one:

```
tools/pfx.py
```

which unpacks `FANTASY.ONE`/`FANTASY.TWO` into a `FANTASY/` directory and
verifies all 25 files against the CRCs stored in the archive. See
[`docs/WRITEUP.md`](docs/WRITEUP.md) for how the format was reverse-engineered
and what the extractor is doing.

## Running

```
pfemu.exe
```

With no arguments, a small Win32 launcher dialog appears first (see
[The launcher](#the-launcher) below); picking **Launch** starts the game from
`FANTASY/PINBALL.EXE`. Passing any of `-p`, `-setup`, or `-secs` skips the
dialog and launches directly — see [Command-line flags](#command-line-flags).

### Controls

Keys pass straight through to the game's own keyboard handler (XT scancodes
off the raw Win32 message, `E0`-prefixed for extended keys) — this is
whatever the game itself listens for, not a pfemu-specific binding, with one
exception:

| Key | Effect |
|---|---|
| **Scroll Lock** | Quit the emulator (moved off F12, which the game itself uses) |
| **F1**–**F4** | Table-select menu: pick a table |
| **F1** (in a table) | Add a player |
| **Down arrow** | Pull and release the plunger |
| **Shift / Alt / Ctrl** (either side) | Flippers (left/right) |
| **Space** | Nudge |
| **F5** (in a table) | The game's own options menu |

### The launcher

The startup dialog reproduces the game's own **F5** in-game options menu —
Balls (3/5), Angle, Scrolling, Ingame Music, Resolution, and Color Mode — so
every one of those can be set once before playing instead of dug out of a
menu after the game boots. It also has a plain on/off switch for sound
(SoundBlaster at 220h/IRQ 7, or silent), which writes the same `SOUND.CFG`
`SETSOUND.EXE` would.

None of this touches the installed game files: options are staged in
`FANTASY/PFEMU-STATE/pfemu_options.cfg` and poked into memory at boot rather
than written into `PINBALL.CFG` (an existing `PINBALL.CFG` at boot can wedge
the sound driver's PLL calibration — a pfemu timing quirk, not a bug in the
values; see `src/launch.c` for the full story). Likewise, any file the game
opens for writing (`INTRO.MOD`'s two-byte flag, the tables' `.hi` files) is
redirected to a copy in `FANTASY/PFEMU-STATE/` on first use, so the originals
stay byte-identical to what shipped no matter how much you play.

### Trainer hotkeys

The launcher's **"Enable trainer"** checkbox arms two hotkeys ported from a
1994 RAZOR DoX trainer (`trainer/PINTRN.COM`), reverse-engineered from its
packed TSR image — see the comment above `fantasies_key_event()` in
[`src/fantasies.c`](src/fantasies.c) for the full derivation:

| Key | Effect |
|---|---|
| **1** | Toggle infinite balls |
| **2** | Toggle ball control mode (down-arrow launches the ball from anywhere on the table, not just the spring) |

Both are off by default and completely inert unless the checkbox is ticked.
A small on-screen notification confirms each toggle.

## Command-line flags

Everyday:

| Flag | Effect |
|---|---|
| `-d DIR` | Game directory (default `FANTASY`) |
| `-p PROG` | Launch a specific program directly, skipping the launcher dialog |
| `-setup` | Run `SETSOUND.EXE` directly |
| `-nolauncher` | Skip the startup dialog, launch `PINBALL.EXE` immediately |
| `-nopatch` | Disable pfemu's Fantasies-specific patches (manual-lookup bypass, boot shortcuts) and run byte-for-byte as shipped |
| `-speed X` | Run at X times real time |
| `-ips N` | Emulated instruction rate (default 6 MIPS) |

Debugging (used throughout the write-ups to isolate specific faults):

| Flag | Effect |
|---|---|
| `-secs N -speed X` | Run headless for N wall seconds at X× real time |
| `-shot FILE` / `-shotevery N` | Write PPM screenshots, once or every N frames |
| `-keys "t:sc:updown,…"` | Drive the keyboard from a script at emulated times |
| `-xring` | Print a ring buffer of the last 8,192 executed addresses at exit |
| `-trap LO HI` | Stop when execution enters a linear address range |
| `-trapexit` | Stop when a child process terminates |
| `-pll N` | Trace N timer-0 reloads (sound driver's phase-locked loop) |
| `-mem LIN` | Hex-dump 256 bytes of guest memory at exit |
| `-intwatch NN` | Log every `INT NN` with AX, BX, and the calling address |
| `-dosdbg` | Log every INT 21h call with the caller's address |
| `-iotrace N` | Log the first N accesses to DMA/page/Sound Blaster/OPL ports |
| `-wav FILE` | Capture host audio output to a WAV file |
| `-force256`, `-nodbl`, `-oldtiming` | Force renderer/timing variants, to bisect display faults |

## What works

- The full launch chain: `PINBALL.EXE` → intro (with logos and palette
  fades) → table-select menu → all four tables.
- Party Land (Table 1), played through as a real game: adding a player,
  launching the ball, both flippers, nudging, the amber dot-matrix score
  panel.
- Music, throughout — intro, menu, and in-game on all four tables, each
  loading its own module, through an emulated Sound Blaster.
- The manual-lookup copy protection, bypassed cleanly (see §5.13.1 of
  [`docs/WRITEUP-PHASE2.md`](docs/WRITEUP-PHASE2.md)) without ever modifying
  `INTRO.PRG` or `INTRO.MOD`.
- Sustained well above real time (~3× on one core with headroom to spare).

## What doesn't

- Any sound driver but Sound Blaster (`ADLIB.SDR`, `INTERNAL.SDR`, `GUS.SDR`,
  `PAS16.SDR`, `SM2.SDR`, `THING.SDR` all want hardware that isn't modelled).
- An automated test suite — verification has been by screenshot diffing, WAV
  analysis, and playing it.
- Tables 2–4 load and run but haven't been played through as extensively as
  Table 1.

See §8–10 of [`docs/WRITEUP-PHASE2.md`](docs/WRITEUP-PHASE2.md) for the full,
itemized list of deviations from real hardware and known gaps.
