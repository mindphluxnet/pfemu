# pfemu

pfemu is a small, purpose-built PC emulator that runs the original DOS release
of **Pinball Fantasies** on 64-bit Windows. It does not use DOSBox, NTVDM, or
code from another emulator.

It currently supports the complete launch sequence, intro, table selector, all
four tables, keyboard controls, and Sound Blaster music. A native launcher lets
you choose the game's options before it starts, and borderless fullscreen is
available from the launcher, the command line, or at any time with `Alt+Enter`.
pfemu also includes targeted fixes for several bugs in the original game.

## Quick start

You will need:

- 64-bit Windows
- the original `FANTASY.ONE` and `FANTASY.TWO` archives
- Python 3, if you need to extract those archives
- Visual Studio 2019 Build Tools with the C++ toolchain, if you want to build
  pfemu yourself

From the repository root:

```powershell
# Extract the original game files into FANTASY\
py tools\pfx.py C:\path\to\the\archives FANTASY

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

## Playing the game

Running `pfemu.exe` without arguments opens the launcher. It lets you configure
sound, balls, table angle, scrolling, in-game music, resolution, color mode,
trainer support, and whether the game starts in fullscreen.

The launcher remembers these choices in `FANTASY/PFEMU-STATE/`.

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
| `Scroll Lock` | Quit pfemu |

Fullscreen can also be enabled with the launcher's **Start in fullscreen**
option or the `-fullscreen` command-line flag. `Alt+Enter` returns to the
previous window size and position.

### Optional trainer

The launcher can enable two hotkeys recovered from the 1994 RAZOR DoX trainer:

| Key | Action |
| --- | --- |
| `1` | Toggle infinite balls |
| `2` | Toggle ball control mode |

In ball control mode, `Down Arrow` launches the ball from anywhere on the
table. Both features are disabled unless **Enable trainer** is selected in the
launcher, and an on-screen message confirms each change.

## Keeping the original files clean

pfemu never writes to the installed game files. High scores, configuration,
and the intro's persistent flag are redirected to `FANTASY/PFEMU-STATE/`.
Deleting that directory resets pfemu's saved settings and game state without
touching the original installation.

This overlay also avoids a timing issue that can occur when the game reads an
existing `PINBALL.CFG` during Sound Blaster calibration. Launcher options are
stored separately and applied in memory when the game starts.

## Command-line options

The launcher is the easiest way to play, but direct startup is useful for
custom installations and development.

| Option | Description |
| --- | --- |
| `-fullscreen` | Start in borderless fullscreen |
| `-nolauncher` | Start `PINBALL.EXE` without showing the launcher |
| `-d DIR` | Use a different game directory; default: `FANTASY` |
| `-p PROGRAM` | Run a specific DOS program and skip the launcher |
| `-setup` | Run `SETSOUND.EXE` and skip the launcher |
| `-nopatch` | Disable the Pinball Fantasies compatibility patches |
| `-speed X` | Run at `X` times normal speed |
| `-ips N` | Set the emulated instruction rate; default: 6,000,000 |

`-d` only affects direct runs. The launcher always uses
`FANTASY/PINBALL.EXE`.

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
| `-snddbg` | Enable additional sound diagnostics |
| `-flipdbg`, `-vscan N`, `-dmd` | Trace display timing and page changes |
| `-force256`, `-nodbl`, `-oldtiming`, `-nosmooth` | Disable rendering behaviors to isolate display problems |

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
| `src/main.c` | Window, input, presentation, fullscreen, and the main loop |

The project builds as a single native Win32 executable and has no runtime
dependencies beyond Windows and the original game data.

## Compatibility and known limits

Working:

- the complete `PINBALL.EXE` launch chain, including the intro and table
  selector
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
- pfemu targets Pinball Fantasies, not general DOS software

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
