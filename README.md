# pfemu

pfemu is a small PC emulator with one job: running the original DOS version of
**Pinball Fantasies** on 64-bit Windows. It does not use DOSBox, NTVDM, or code
from another emulator.

Four releases of the game are supported. pfemu tells them apart by hashing the
game files, so you never have to identify your copy yourself:

| Release | Boot program | Programs |
|---|---|---|
| Original floppy release | `PINBALL.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| Deluxe CD-ROM (1995) | `PINBALL.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| Power Pack (1996) | `PF.EXE` | `INTRO.PRG`, `TABLE1-4.PRG` |
| 5 Min Demo (1993) | `PFDEMO.EXE` | `DEMO.PRG`, `PLAND.PRG` |

The intro, the table selector, all four tables, keyboard controls, and Sound
Blaster music all work. The 1993 demo is a partial exception: it boots and
plays music but stops at its title screen. The reason is documented in
[Releases](docs/RELEASES.md).

A built-in launcher takes care of game options, multiple installations, and
fullscreen. pfemu also fixes a few bugs in the original game while it runs,
without changing your game files. Those fixes are described in
[Emulator](docs/EMULATOR.md).

## Quick start

You will need:

- 64-bit Windows
- the original game files
- Python 3, only if you need to extract the floppy archives
- Visual Studio 2019 Build Tools with C++, only if you want to build pfemu
  yourself

From the repository root:

```powershell
# Extract the original game files into GAME\
py tools\pfx.py C:\path\to\the\archives GAME

# Build pfemu
.\build.bat

# Start it
.\pfemu.exe
```

The extractor checks all 25 files against the CRCs stored in the archives.
Game files and compiled binaries are not included in this repository.

> `build.bat` expects the default Build Tools install location. If yours lives
> elsewhere, update the `vcvars64.bat` path in that file.

## Where to put the game files

Put one installation's files directly in a folder. `GAME\` is the convention,
and any other top-level folder works the same way. Do not nest them an extra
level down: `GAME\FANTASY\INTRO.PRG` will not be found.

Every folder that holds an `INTRO.PRG` (or a `DEMO.PRG`, for the demo) shows up
in the launcher, so several releases can sit side by side. The folder name is
only a place to look. The release is always decided by the files inside it.
`pfemu.exe -releases` prints the detection report for each installation it
finds.

- **Deluxe CD-ROM:** there is no extractor, so copy the files yourself. The
  1995 release installed in two halves. `PINBALL.EXE`, `SETSOUND.EXE`, and the
  `.SDR` drivers went onto the hard drive, while `INTRO.PRG`, `TABLE1-4.PRG`,
  and the `.MOD` music stayed on the CD. pfemu does not emulate a CD drive, so
  combine both halves into one flat folder.
- **Power Pack:** copy the directory as it is. pfemu runs its `PF.EXE`
  directly and ignores the bundled `PINBALL.BAT` wrapper.
- **Demo:** run its `INSTALL.BAT` or copy the directory as it is. Its two
  programs are LZEXE-compressed, and pfemu unpacks them while loading so the
  usual fixes apply. `-nolzexe` turns that off for comparison.

[Releases](docs/RELEASES.md) lists the exact files and hashes for every
release.

## Playing

Starting `pfemu.exe` with no arguments opens the launcher. It offers sound and
sound quality, balls, table angle, scrolling, in-game music, resolution, color
mode, trainer support, and fullscreen. When several installations are present
it also lets you pick between them. The **Details** button shows the full
detection report, which is worth including if you ever report a release that
pfemu does not recognise.

If an installation is not recognised, **Launch** stays disabled. Each release
stores its options at a different address in memory, so applying one release's
layout to another would corrupt it. pfemu refuses to guess.

Your choices are saved in `PFEMU-STATE/` inside the selected install's folder,
along with high scores and game state. Deleting that folder resets everything
pfemu saved, while the original files stay untouched. pfemu never writes to
the installed game files.

### Controls

Keys are passed straight through to the game, so these are the original game
controls:

| Key | Action |
|---|---|
| `F1`-`F4` | Choose a table from the selector |
| `F1` | Add a player at a table |
| `Down Arrow` | Pull and release the plunger |
| `Shift`, `Alt`, or `Ctrl` | Flippers (either side) |
| `Space` | Nudge |
| `F5` | Game options menu |
| `Alt+Enter` | Borderless fullscreen (also `-fullscreen` or the launcher option) |
| `F11` | Save a PNG screenshot to `screenshots/` |
| `-` / `+` | Volume down / up in 5% steps |
| `Keypad *` | Mute / restore |
| `Scroll Lock` | Quit |

Print Screen never reaches pfemu, because Windows 11 intercepts it for Snipping
Tool. Use `F11` for screenshots instead.

### Sound

The launcher has a **Volume** slider, and `-` / `+` / `*` adjust the volume in
the game window. All three only change what pfemu hands to Windows. The
emulated sound card is unaffected, and `-wav` captures are always recorded at
full level. The default is 70%. The level is remembered separately for each
install, and `-vol N` overrides it for one run without saving.

**Quality** is the game's own setting: the five notches `SETSOUND.EXE` once
offered between Low and High. It sets the rate at which the Sound Blaster
driver mixes music (12-21 kHz). A higher notch costs more emulated CPU per
second of audio, which is the same tradeoff this setting offered in 1992. If a
high notch starves the game loop, `-ips` models a faster CPU. Notch 1 is what
pfemu used before this setting was exposed.

### Trainer (optional)

These hotkeys come from two 1994 trainers. Turn on **Enable trainer** in the
launcher first:

| Key | Action |
|---|---|
| `1` | Infinite balls |
| `2` | Ball control mode (`Down Arrow` launches from anywhere, `Z` kicks upward) |
| `3` | Infinite tilts |

Each toggle prints an on-screen confirmation. The targets are found by scanning
the loaded game, so they work across all releases.

### Sessions

The launcher's **Session** row (or `-record FILE` / `-replay FILE` on the
command line) records your keypresses with emulated-time stamps into a `.pfr`
file, and replays them later on the same clock. Host stutter cannot shift what
the game sees. On replay, pfemu checks that the release and program hashes
match, restores the recorded install, freezes the clocks the game can see, and
sends the game's writes to a throwaway copy.

Two rules keep replays honest. The trainer must stay off while recording or
replaying. Volume is never recorded and stays adjustable throughout. A small
red `REC` badge appears while recording (green `PLAY` while replaying). The
badge is drawn by the host and never reaches the game, the `.pfr` file, or any
capture. [Replay](docs/REPLAY.md) has the full details.

## Command line

The launcher covers normal play. Flags are for alternate installs and
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
| `-vol N` | Volume 0-100 for this run only |
| `-record FILE` / `-replay FILE` | Record / replay a session |
| `-secs N` | Run headless for N wall-clock seconds |
| `-shot FILE` / `-shotevery N` | Save PPM screenshot(s) |
| `-keys "t:sc:state,..."` | Feed timed keyboard events to the guest |
| `-wav FILE` | Capture audio to WAV |

The tracing flags (`-t`, `-xring`, `-trap`, `-dosdbg`, `-iotrace`, `-pll`,
`-snddbg`, `-flipdbg`, `-vscan`, `-dmd`, `-balldbg`, `-matdbg`, `-mem`,
`-intwatch`, `-undefdump`, `-dumpseg`, `-prof`, `-vgastate`, and several `-no*`
rendering and timing overrides) are documented in
[Emulator](docs/EMULATOR.md#debugging-and-tracing).

`-release` is an escape hatch for lightly modified copies of a known release.
Do not use it to force an unknown build to start.

## Limits

- Sound Blaster only. The AdLib, GUS, PAS16, Sound Master II, internal
  speaker, and ThING drivers are not emulated.
- Party Land is the best tested table. The other three load and run but have
  seen less play-through testing.
- Verification is manual (gameplay, screenshots, traces, captured audio).
  There is no automated test suite.
- pfemu targets Pinball Fantasies, not DOS software in general.

## Further reading

- [Emulator](docs/EMULATOR.md): how pfemu works, the game-specific fixes, and
  the debug flags.
- [Releases](docs/RELEASES.md): supported versions, detection, and the demo's
  title-screen stop.
- [Archive format](docs/ARCHIVE.md): the floppy installer format and the
  extractor.
- [Replay](docs/REPLAY.md): session recording, accuracy, and validation.
