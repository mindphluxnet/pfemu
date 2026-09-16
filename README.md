# pfemu

pfemu is a small, purpose-built PC emulator that runs the original DOS release
of **Pinball Fantasies** on 64-bit Windows. It does not use DOSBox, NTVDM, or
code from another emulator.

Three releases of the game are supported, and pfemu works out which one it is
looking at by hashing the installation rather than by asking:

| Release | Boot program |
|---|---|
| Pinball Fantasies, the original floppy release | `PINBALL.EXE` |
| Pinball Fantasies Deluxe CD-ROM (1995) | `PINBALL.EXE` |
| Pinball Power Pack (1996) | `PF.EXE` |

It currently supports the complete launch sequence, intro, table selector, all
four tables, keyboard controls, and Sound Blaster music. A native launcher lets
you choose the game's options before it starts, pick between installations when
several are present, and borderless fullscreen is available from the launcher,
the command line, or at any time with `Alt+Enter`. pfemu also includes targeted
fixes for several bugs in the original game.

## Quick start

You will need:

- 64-bit Windows
- the original `FANTASY.ONE` and `FANTASY.TWO` archives
- Python 3, if you need to extract those archives
- Visual Studio 2019 Build Tools with the C++ toolchain, if you want to build
  pfemu yourself

From the repository root:

```powershell
# Extract the original game files into GAME\
py tools\pfx.py C:\path\to\the\archives GAME

# Build pfemu
.\build.bat

# Start it
.\pfemu.exe
```

The extractor checks all 25 files against the CRCs in the original archives.
Game files and compiled binaries are deliberately not included in this
repository.

> `build.bat` expects the default Visual Studio 2019 Build Tools location. If
> Visual Studio is installed elsewhere, update the `vcvars64.bat` path in that
> file.

### Where to put the game files

Put one installation's files **directly** in a directory named `GAME\` - not in
the enclosing `FANTASY\` or `PFD\` folder the archive unpacks into. Any other
top-level directory holding an `INTRO.PRG` is offered as well, so several
releases can sit side by side (`FANTASY\`, `FANTASYDX\`, ...) and the launcher
will list them. The directory name is only a place to look; which release it
holds is decided by the files inside it.

`pfemu.exe -releases` prints what was found in each installation, including the
sizes and hashes of anything it did not recognise.

### Pinball Fantasies Deluxe (CD-ROM)

There's no extraction tool for the Deluxe release; copy its files in yourself.
The 1995 CD-ROM release installs as two halves - `INSTALL.COM` only copies
`PINBALL.EXE` and the sound drivers to the hard drive, and expects
`INTRO.PRG`/`TABLE1-4.PRG`/the `.MOD` music to keep loading off the CD-ROM.
pfemu doesn't emulate a CD-ROM drive, so combine both halves into one flat
folder: `PINBALL.EXE`, `SETSOUND.EXE`, and the `.SDR` drivers from the
hard-drive install, alongside `INTRO.PRG`, `TABLE1.PRG`-`TABLE4.PRG`,
`INTRO.MOD`, `TABLE1.MOD`-`TABLE4.MOD`, and `MOD2.MOD` from the CD-ROM's
`PFD\FANTASY\` directory.

### Pinball Power Pack (1996)

A later repackaging of the floppy-family release, with its own intro and its
own Table 1 and Table 2 programs. Its launcher is called `PF.EXE`; pfemu runs
that directly, so the `PINBALL.BAT` wrapper it ships with is not needed and is
ignored. Copy the directory in as it is.

## Playing the game

Running `pfemu.exe` without arguments opens the launcher. It lets you configure
sound, volume, sound quality, balls, table angle, scrolling, in-game music,
resolution, color mode, trainer support, and whether the game starts in
fullscreen. At the top it shows which release it detected; if it finds more
than one installation, it also shows a list to pick between them. **Details**
prints the full detection report - which is also what to send along when a
release pfemu doesn't know about turns up.

If an installation isn't recognised, **Launch** stays disabled rather than
guessing. Each release keeps its options in a different place in memory, and
writing one release's layout into another release's intro corrupts it.

The launcher remembers these choices in `PFEMU-STATE/` under whichever
install's directory is selected.

### Fixes for the original game

pfemu does more than emulate the hardware. It applies narrowly scoped fixes to
the loaded game while it runs, including:

- clearing an interrupt-handshake race after unpausing that could leave the
  ball stuck—a bug noted in the original developers' own source comments
- correcting the game's shared flipper state when multiple flipper keys
  overlap, and recovering when Windows loses a modifier-key release
- making the original, otherwise ignored **Ingame Music** setting work through
  the launcher

These fixes are applied in memory and do not modify the game files. The
game-specific patches can be disabled with `-nopatch`.

### Controls

Most keys are passed directly to Pinball Fantasies, so these are the original
game controls rather than emulator-specific bindings.

| Key | Action |
| --- | --- |
| `F1`–`F4` | Choose a table from the table selector |
| `F1` | Add a player while a table is running |
| `Down Arrow` | Pull and release the plunger |
| `Shift`, `Alt`, or `Ctrl` | Operate the flippers; either side works |
| `Space` | Nudge the table |
| `F5` | Open the game's options menu |
| `Alt+Enter` | Toggle borderless fullscreen |
| `F11` | Save a PNG screenshot |
| `-` / `+` | Turn the volume down / up in 5% steps; shown on screen |
| `Keypad *` | Mute, and restore the previous level; shown on screen |
| `Scroll Lock` | Quit pfemu |

Fullscreen can also be enabled with the launcher's **Start in fullscreen**
option or the `-fullscreen` command-line flag. `Alt+Enter` returns to the
previous window size and position.

`F11` saves the current frame as a timestamped PNG under a `screenshots/`
subdirectory (created next to `pfemu.exe` if it doesn't already exist), e.g.
`screenshots/pfemu_20260916_143005.png`. Not Print Screen: Windows 11
intercepts that key itself and pops up Snipping Tool instead of reaching
pfemu.

### Sound

The 1992 original expected a volume wheel on the sound card and another on the
speakers. Since neither exists here, the launcher has a **Volume** slider and
the game window takes `-` / `+` and keypad `*`, all of which only scale what
pfemu hands to Windows â€” nothing emulated changes, and `-wav` captures are
written at the card's own level regardless. 100% is the level pfemu played at
before the slider existed, and the default is 70%.

The level is remembered per install, whether it was set with the slider or
with `-` / `+` in the game window â€” quit and the next session starts where you
left it. Muting is the exception: it is a momentary thing, so quitting while
muted saves the level the mute is hiding rather than silence. Riding `-` all
the way down to 0 does save 0. `-vol N` overrides the saved level for one run
without replacing it.

**Quality** is the game's own setting, the five notches `SETSOUND.EXE` offered
between Low and High. It picks the rate at which `SBLASTER.SDR` mixes the
music, which is also the rate the emulated card plays at:

| Notch | Mixing rate |
| --- | --- |
| 1 | 12000 Hz |
| 2 | 16000 Hz |
| 3 | 20000 Hz |
| 4 | 21000 Hz |
| 5 | 21000 Hz, plus a longer per-voice mixing routine |

The mixer is guest code, so a higher notch spends more of the emulated 386's
budget per second of audio â€” exactly the trade the setting existed to offer in
1992. If a high notch starves the game loop, `-ips` models a faster CPU.
Notch 1 is what pfemu used before this setting was exposed.

### Optional trainer

The launcher can enable two hotkeys recovered from the 1994 RAZOR DoX trainer:

| Key | Action |
| --- | --- |
| `1` | Toggle infinite balls |
| `2` | Toggle ball control mode |

In ball control mode, `Down Arrow` launches the ball from anywhere on the
table. Both features are disabled unless **Enable trainer** is selected in the
launcher, and an on-screen message confirms each change. Both hotkeys locate
their targets by signature scan, so they work on every supported
releases alike even though the two ship differently laid-out table programs.

## Keeping the original files clean

pfemu never writes to the installed game files. High scores, configuration,
and the intro's persistent flag are redirected to `PFEMU-STATE/` inside
whichever game directory you're running (`GAME/PFEMU-STATE/`, and the same
under any other installation directory). Deleting it resets pfemu's saved
settings and game state without touching the original installation.

This overlay also avoids a timing issue that can occur when the game reads an
existing `PINBALL.CFG` during Sound Blaster calibration. Launcher options are
stored separately and applied in memory when the game starts.

## Command-line options

The launcher is the easiest way to play, but direct startup is useful for
custom installations and development.

| Option | Description |
| --- | --- |
| `-fullscreen` | Start in borderless fullscreen |
| `-nolauncher` | Start the detected release's boot program without the launcher |
| `-d DIR` | Use a specific game directory; default: the first one found |
| `-releases` | Print the detection report for every installation, then exit |
| `-release ID` | Force a release (`floppy`, `power_pack`, `deluxe`) whatever the hashes say |
| `-p PROGRAM` | Run a specific DOS program and skip the launcher |
| `-setup` | Run `SETSOUND.EXE` and skip the launcher |
| `-nopatch` | Disable the Pinball Fantasies compatibility patches |
| `-speed X` | Run at `X` times normal speed |
| `-ips N` | Set the emulated instruction rate; default: 6,000,000 |
| `-vol N` | Output volume, 0â€“100, for this run only; overrides the saved level |

`-d` only affects direct runs; pass `-d FANTASYDX` to boot a particular
installation this way. Without it, pfemu uses the first one it finds (`GAME\`
first, then the rest alphabetically). The boot program comes from whichever
release that directory turns out to hold, so `-p` is only needed to run
something other than the game itself.

`-release` is a development escape hatch. Detection deliberately refuses to
apply a known release's memory layout to code it does not recognise, and this
overrides that refusal - so use it on a build you know is a lightly modified
copy of the release you name, not to make an unknown one start.

### Diagnostics

These options were used to reverse-engineer and verify the emulator:

| Option | Description |
| --- | --- |
| `-secs N` | Run for `N` wall-clock seconds and skip the launcher |
| `-shot FILE` | Save the final frame as a PPM screenshot |
| `-shotevery N` | Save a numbered PPM screenshot every `N` emulated seconds |
| `-keys "t:scancode:state,..."` | Feed timed keyboard events to the guest |
| `-wav FILE` | Capture audio to a WAV file |
| `-t` | Write the general trace to `pfemu.log` |
| `-xring` | Print the last 8,192 executed addresses at exit |
| `-trap LOW HIGH` | Stop when execution enters a linear address range |
| `-trapexit` | Stop when a child process exits |
| `-mem ADDRESS` | Dump 256 bytes of guest memory at exit |
| `-intwatch NN` | Trace calls to interrupt `NN` |
| `-dosdbg` | Trace DOS `INT 21h` calls |
| `-iotrace N` | Trace the first `N` DMA, Sound Blaster, and OPL I/O accesses |
| `-pll N` | Trace `N` timer reloads during audio calibration |
| `-snddbg` | Log each DSP transfer with its DMA buffer and the gap since the last one |
| `-flipdbg`, `-vscan N`, `-dmd` | Trace display timing and page changes |
| `-balldbg` | Time the ball erase/redraw gap and how often a frame lands in it |
| `-matdbg` | Count dot-matrix updates, and how many the sound driver's "no time left" flag dropped |
| `-nopitm0` | Read PIT channel 0 in mode 0 as a free-running rate generator, as builds before the one-shot fix did |
| `-nophaselock` | Present on the old wall timer instead of a fixed frame phase |
| `-dmairq` | Interrupt on each DMA buffer wrap instead of when the DSP's transfer length runs out |
| `-noballsync` | Stop pairing the displayed camera with the ball; show the raw camera |
| `-nolatch` | Read the CRTC start address live instead of latching it at retrace |
| `-force256`, `-nodbl`, `-oldtiming` | Disable rendering behaviors to isolate display problems |

Numeric addresses and interrupt numbers are hexadecimal unless stated
otherwise. Keyboard scripts use comma-separated `time:scancode:state` entries,
where `state` is `1` for key down and `0` for key up.

## What pfemu emulates

The emulator is intentionally narrow: it implements the parts of a 386-era PC
that Pinball Fantasies actually uses.

| Source file | Responsibility |
| --- | --- |
| `src/cpu.c` | 386 real-mode CPU interpreter |
| `src/vga.c` | Text, planar VGA, chain-4, Mode X, DAC, and display timing |
| `src/dev.c` | PIC, PIT, keyboard controller, and I/O ports |
| `src/bios.c` | BIOS interrupts and interrupt-vector setup |
| `src/dos.c` | DOS processes, memory, files, program loading, and the write overlay |
| `src/sound.c` | DMA, Sound Blaster DSP, Windows audio, and WAV capture |
| `src/fantasies.c` | Game-specific compatibility fixes and trainer support |
| `src/launch.c` | Native Windows launcher and saved options |
| `src/release.c` | Release identification, by SHA-256 of the game's programs |
| `src/main.c` | Window, input, presentation, fullscreen, and the main loop |

The project builds as a single native Win32 executable and has no runtime
dependencies beyond Windows and the original game data.

## Compatibility and known limits

Working:

- the complete launch chain, including the intro and table selector, for all
  three supported releases
- Party Land, Speed Devils, Billion Dollar Gameshow, and Stones 'N Bones
- Sound Blaster music in the intro, menus, and all four tables
- the manual-lookup protection bypass, without modifying `INTRO.PRG` or
  `INTRO.MOD`
- sustained operation above the original game's real-time speed

Known limits:

- only the Sound Blaster driver is emulated; the AdLib, GUS, PAS16, Sound
  Master II, internal-speaker, and ThING drivers are not supported
- Party Land has received the most play-testing; the other tables load and run
  but have had less extensive play-through testing
- verification is currently manual, using gameplay, screenshots, traces, and
  captured audio rather than an automated test suite
- pfemu targets Pinball Fantasies, not general
  DOS software

## Technical write-ups

- [Installing Pinball Fantasies without the DOS installer](docs/WRITEUP.md)
  explains the archive format, decompressor, CRC layer, and extraction tool.
- [Running Pinball Fantasies without DOSBox](docs/WRITEUP-PHASE2.md) covers the
  emulator, hardware behavior, launch chain, copy protection, and debugging
  process.
- [Performance optimizations and later fixes](docs/OPTIMIZATIONS.md) records
  timing, rendering, launcher, and performance work completed after the main
  implementation.

No external reference material about the game's file formats or copy
protection was used during the original reverse-engineering work; those details
were derived from the shipped binaries and their behavior.
