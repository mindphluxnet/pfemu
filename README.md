# pfemu

pfemu is a small, purpose-built PC emulator that runs the original DOS release
of **Pinball Fantasies** on 64-bit Windows. No DOSBox, no NTVDM, no borrowed
emulator core.

Four releases are supported. pfemu identifies them by hashing the game files,
not by asking you:

| Release | Boot program | Programs |
|---|---|---|
| Original floppy release | `PINBALL.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| Deluxe CD-ROM (1995) | `PINBALL.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| Power Pack (1996) | `PF.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| 5 Min Demo (1993) | `PFDEMO.EXE` | `DEMO.PRG`, `PLAND.PRG` |

Everything else — intro, table selector, all four tables, keyboard, Sound
Blaster music — works. The demo boots and plays music but stops at its title
screen; see [Releases](docs/RELEASES.md) for why. A native launcher handles
game options, multiple installations, and fullscreen.

pfemu also fixes a few bugs in the original game in memory, without touching
your game files. See [Emulator](docs/EMULATOR.md).

## Quick start

You need 64-bit Windows, the original game files, Python 3 (only to extract
the floppy archives), and Visual Studio 2019 Build Tools with C++ (only to
build).

```powershell
# Extract the floppy archives into GAME\
py tools\pfx.py C:\path\to\the\archives GAME

# Build
.\build.bat

# Run
.\pfemu.exe
```

The extractor checks all 25 files against the CRCs in the archives. Game files
and compiled binaries are not in this repository.

> `build.bat` expects the default Build Tools location. If yours is elsewhere,
> update the `vcvars64.bat` path in that file.

## Where to put the game files

Put one installation's files **directly** in a folder — `GAME\` is the
convention, but any top-level folder works. Don't nest them one level down
(`GAME\FANTASY\INTRO.PRG` won't be found).

Any folder holding an `INTRO.PRG` (or `DEMO.PRG`, for the demo) is listed in
the launcher, so several releases can sit side by side. The folder name is
just a place to look; the release is decided by the files inside.
`pfemu.exe -releases` prints the detection report for each one.

- **Deluxe CD-ROM:** there is no extractor — copy the files yourself. The 1995
  install is split in two: `PINBALL.EXE`, `SETSOUND.EXE`, and the `.SDR`
  drivers went to the hard drive, while `INTRO.PRG`, `TABLE1-4.PRG`, and the
  `.MOD` music stayed on the CD. pfemu doesn't emulate a CD drive, so combine
  both halves into one flat folder.
- **Power Pack:** copy the directory as-is. pfemu runs its `PF.EXE` directly;
  the bundled `PINBALL.BAT` wrapper is ignored.
- **Demo:** run its `INSTALL.BAT` or copy the directory as-is. Its two
  programs are LZEXE-compressed; pfemu unpacks them while loading so the usual
  fixes apply. `-nolzexe` disables that for comparison.

See [Releases](docs/RELEASES.md) for the full per-release file lists and
hashes.

## Playing

Running `pfemu.exe` with no arguments opens the launcher: sound on/off and
quality, balls, table angle, scrolling, in-game music, resolution, color mode,
trainer, fullscreen, and — if several installations are present — which one to
run. **Details** shows the detection report; send that along if you find a
release pfemu doesn't recognise.

If an installation isn't recognised, **Launch** stays disabled. Each release
keeps its options at a different address in memory, so writing one release's
layout into another would corrupt it. pfemu refuses to guess.

Choices are remembered in `PFEMU-STATE/` inside the selected install's folder.
Deleting that folder resets saved settings and high scores without touching
the original files. pfemu never writes to the installed game files.

### Controls

Keys are passed through to the game, so these are the original controls:

| Key | Action |
|---|---|
| `F1`–`F4` | Choose a table from the selector |
| `F1` | Add a player at a table |
| `Down Arrow` | Pull and release the plunger |
| `Shift`, `Alt`, or `Ctrl` | Flippers (either side) |
| `Space` | Nudge |
| `F5` | Game options menu |
| `Alt+Enter` | Borderless fullscreen (also `-fullscreen` or launcher option) |
| `F11` | Save a PNG screenshot to `screenshots/` |
| `-` / `+` | Volume down / up in 5% steps |
| `Keypad *` | Mute / restore |
| `Scroll Lock` | Quit |

Print Screen won't work: Windows 11 intercepts it for Snipping Tool before it
reaches pfemu.

### Sound

The launcher has a **Volume** slider, and `-` / `+` / `*` work in the game
window. These only scale what pfemu hands to Windows — the emulated card is
untouched, and `-wav` captures are always written at full level. Default is
70%; the level is remembered per install. `-vol N` overrides it for one run.

**Quality** is the game's own setting (the five notches `SETSOUND.EXE`
offered). It sets the rate the Sound Blaster driver mixes at (12–21 kHz), so a
higher notch costs more emulated CPU per second of audio — the same tradeoff
it was in 1992. If a high notch starves the game loop, `-ips` models a faster
CPU. Notch 1 is the old default.

### Trainer (optional)

Recovered from two 1994 trainers. Enable **Enable trainer** in the launcher,
then:

| Key | Action |
|---|---|
| `1` | Infinite balls |
| `2` | Ball control mode (`Down Arrow` launches from anywhere, `Z` kicks upward) |
| `3` | Infinite tilts |

Each toggle shows an on-screen message. Targets are found by signature scan,
so they work across all releases.

### Sessions

The launcher's **Session** row (or `-record FILE` / `-replay FILE`) records
keypresses with emulated-time stamps into a `.pfr` file and replays them on
the same clock, so host stutter can't shift what the game sees. Replay checks
that the release and program hashes match, restores the recorded install,
freezes guest-visible clocks, and redirects writes to a throwaway copy.

Two rules: the trainer can't be on while recording or replaying, and volume is
never recorded (it stays live throughout). A small red `REC` / green `PLAY`
badge shows in the window corner; it's host-only and never reaches captures.
See [Replay](docs/REPLAY.md).

## Command line

The launcher covers normal play; flags are for alternate installs and
development.

| Option | What it does |
|---|---|
| `-fullscreen` | Start in borderless fullscreen |
| `-nolauncher` | Skip the launcher, boot the detected release |
| `-d DIR` | Use a specific game directory (default: first found) |
| `-releases` | Print the detection report, then exit |
| `-release ID` | Force a release (`floppy`, `power_pack`, `deluxe`, `demo`) |
| `-p PROGRAM` | Run a specific DOS program instead of the boot program |
| `-setup` | Run `SETSOUND.EXE` |
| `-nopatch` | Disable the game-specific fixes |
| `-nolzexe` | Don't unpack LZEXE programs at load |
| `-speed X` | Run at X times normal speed |
| `-ips N` | Emulated instructions per second (default 6,000,000) |
| `-vol N` | Volume 0–100 for this run only |
| `-record FILE` / `-replay FILE` | Record / replay a session |
| `-secs N` | Run headless for N wall-clock seconds |
| `-shot FILE` / `-shotevery N` | Save PPM screenshot(s) |
| `-keys "t:sc:state,..."` | Feed timed keyboard events to the guest |
| `-wav FILE` | Capture audio to WAV |

There are further tracing flags (`-t`, `-xring`, `-trap`, `-dosdbg`,
`-iotrace`, `-pll`, `-snddbg`, `-flipdbg`, `-vscan`, `-dmd`, `-balldbg`,
`-matdbg`, `-mem`, `-intwatch`, `-undefdump`, `-dumpseg`, `-prof`,
`-vgastate`, and several `-no*` rendering/timing overrides). They are
documented in [Emulator](docs/EMULATOR.md#debugging-and-tracing).

`-release` is an escape hatch for lightly modified copies of a known release.
Don't use it to force an unknown build to start.

## Limits

- Sound Blaster only. AdLib, GUS, PAS16, Sound Master II, internal speaker,
  and ThING drivers are not emulated.
- Party Land has had the most play-testing; the other tables load and run but
  have had less.
- Verification is manual (gameplay, screenshots, traces, captured audio).
  There is no automated test suite.
- pfemu targets Pinball Fantasies, not DOS software in general.

## Further reading

- [Emulator](docs/EMULATOR.md) — how pfemu works, the game-specific fixes,
  and the debug flags.
- [Releases](docs/RELEASES.md) — supported versions, detection, and the demo's
  title-screen stop.
- [Archive format](docs/ARCHIVE.md) — the floppy installer format and the
  extractor.
- [Replay](docs/REPLAY.md) — session recording, accuracy, and validation.
