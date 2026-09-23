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

For full disclosure: this emulator was developed with the help of AI models.
Roughly who did what:

- **Claude Opus 5** (Anthropic): initial reverse engineering and the MVP
  emulator. Later, release detection by file hash, the mode-0 PIT counter fix
  that got the engine running at its real frame rate, the table-select palette
  and ball-flicker fixes, volume and quality controls, 1993 demo support,
  savestates, direct-to-table, and replay verification hashes.
- **Claude Sonnet 5** (Anthropic): Deluxe (CD-ROM) support, fullscreen, the
  first trainer hotkeys (infinite balls, ball control), game options in the
  launcher, and PNG screenshots.
- **ChatGPT Sol 5.6** (OpenAI): core performance work, planar rendering fixes,
  the DOS-layer manual check bypass, session record/replay, the megatrainer
  hotkeys (infinite tilts, ball jump), the on-screen session badges, the
  launcher UI cleanup, the documentation rewrites, and the release build.
- **Muse Spark 1.3** (Meta AI): the original Win32 launcher, Pinball Dreams
  support (since dropped), performance benchmarking, and the ball-flicker,
  scrolling and dot-matrix cadence investigations.

The original source code, published at
https://github.com/historicalsource/pinballfantasies, was used as reference
during development.

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

- **Deluxe from GOG.com:** nothing to do. When the GOG version is installed
  and no Deluxe is present yet, the launcher offers once to copy the game out
  of GOG's `game.gog` CD image into `GOG\`. The GOG installation is not
  changed. If you said No, delete `pfemu-gog.cfg` next to `pfemu.exe` to be
  asked again.
- **Deluxe CD-ROM:** copy the files yourself. The 1995 release installed in
  two halves. `PINBALL.EXE`, `SETSOUND.EXE`, and the
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

**Use unmodified game files**. pfemu checks the programs against known-good hashes, so a cracked copy will not be recognised and will not launch. No crack is needed: pfemu answers the manual lookup itself, as described under game-specific fixes.

## Playing

Starting `pfemu.exe` with no arguments opens the launcher. It offers sound and
sound quality, balls, table angle, scrolling, in-game music, resolution, color
mode, trainer support, and fullscreen. When several installations are present
it also lets you pick between them. The **Details** button opens a scrollable
report window with a **Copy** button; that report is worth including if you
ever report a release that pfemu does not recognise.

If an installation is not recognised, **Launch** stays disabled. Each release
stores its options at a different address in memory, so applying one release's
layout to another would corrupt it. pfemu refuses to guess.

Your choices are saved in `PFEMU-STATE/pfemu.cfg` inside the selected
install's folder - one plain-text file per install, next to the high scores
and game state. Deleting that folder resets everything pfemu saved, while
the original files stay untouched. pfemu never writes to the installed game
files.

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
| `F11` | Save a PNG screenshot to `screenshots/`, at the size the picture is on screen |
| `F6` / `F8` | Save / load the snapshot slot (`savestates/`) |
| `-` / `+` | Volume down / up in 5% steps |
| `Keypad *` | Mute / restore |
| `Keypad /` | Enhancement bypass on/off (A/B the old sound) |
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

**Audio enhancement** (launcher, per install) shapes only what reaches your
speakers: **Bass** and **Treble** (±12 dB shelves), **Oomph** (extra low-bass
up to +12 dB with a limiter so it never clips), and **Headphone mode** (a
narrow pseudo-stereo image with crossfeed, since the game's Sound Blaster
music is mono). Flat/Off is the old sound. Like volume, these are host-only:
they are never recorded, never affect replays, and `-wav` captures always
stay dry so two runs remain comparable.

**Resolution High** needs about twice the emulated CPU of Normal (360x350 at
~71 Hz against 320x240 at 60 Hz), so pfemu automatically models a faster CPU
there: 12 MIPS instead of the default 6, unless `-ips` or a replay says
otherwise. Without it the game runs slow and the music crackles, even on the
lowest sound notch. The switch is picked up from the launcher setting at boot
and, for an F5 change mid-session, from the first hi-res frame itself (play
mode only; `-ips` always wins).

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

The launcher stays open while the game runs. It starts each game as a
process of its own and comes back to the front when you quit, so the next
game, or submitting the one you just recorded, is one click away.

### Leaderboard

**Ranked** (next to Record, on by default) records against a fixed
starting state instead of your own high-score tables, which is what the
leaderboard needs to verify a game: the tables change how the game plays.
Your own table is not shown during a ranked game and is not updated by it.
Unticked, recording works as before but the file cannot be submitted.

The **Leaderboard** group logs in to the leaderboard server, or registers an
account (the email is optional, but without one a forgotten password cannot
be recovered). After a ranked recording ends, **Submit** uploads it; in
Replay mode it uploads the picked ranked file. **Replays...** (next to the
file field, except in Record mode) lists your recordings with their best
games, their length and where each stands on the leaderboard, with
**Submit** and **Delete** on every row. Delete moves a recording to the
Recycle Bin. **Replay** there switches the launcher to replaying the one
you picked. The server replays the game
itself and the launcher shows the result when it has one, usually a minute
or two for a five-minute game. **Submissions** lists everything you sent,
one row each, and **Copy** puts the table on the clipboard. **Leaderboards**
under the buttons opens the website with the boards, and **My account**
opens your account page there (the website asks you to log in separately).
Only a finished one-player 3-ball game that ran until the table was back in
attract mode counts. Each result names the pfemu build that produced it.
Sending the same recording again does not verify it again: the server
verifies every kept recording again by itself when it moves to a new build,
and the launcher picks up the new result when you come back to it.

The login is kept in `pfemu-online.cfg` next to `pfemu.exe`, encrypted for
your Windows account. The file also holds the server address (`server=`,
default `https://pf.dark-secrets.eu`).


### Starting at a table

The launcher's **Start at** box boots straight into a table, skipping the
intro and the menu. `-table 1`-`-table 4` does the same from the command
line. Quitting the table returns to the menu exactly as it normally would,
and your game options still apply.

This is not the same as `-p TABLE1.PRG`. The table programs cannot run on
their own - they depend on services `PINBALL.EXE` installs and on its
program loop being there when they exit. **Start at** leaves all of that
running and only skips the intro, so nothing the table relies on is
missing. Recording a session that began at a table replays correctly; the
`.pfr` remembers where it started.

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
| `-unthrottle` | Drop the wall-clock pacer entirely; host pacing only, so it overrides nothing a replay carries |
| `-ranked` | With `-record`: record against the canonical state the leaderboard verifies, instead of this install's `PFEMU-STATE/` |
| `-strict` | Verification-grade input policy: refuse a replay with no integrity line, with pre-cycle events, or recorded without `-ranked` |
| `-launched` | Set by the launcher on the games it starts: refusals show a message box |
| `-verify FILE` | Write the machine-readable verdict (one JSON object) for a replay: status, footer/capture comparison, every scored attempt, and the best **rankable** score. Implies `-scoredbg`, and sets the exit code from the verdict. This is the output a verification service reads - see [Verification](docs/VERIFY.md) |
| `-ips N` | Emulated instructions per second (default 6,000,000; 12,000,000 when Resolution is High unless `-ips` or a replay overrides) |
| `-vol N` | Volume 0-100 for this run only |
| `-record FILE` / `-replay FILE` | Record / replay a session |
| `-load FILE` / `-snapsave FILE` | Boot from / write a snapshot |
| `-untilemu SEC` | Stop at an emulated-time point (validation) |
| `-table N` | Start at table N (1-4) instead of the intro |
| `-res normal|high` | Resolution for this run only, without saving (like `-vol`) |
| `-freezetime` | Pin the guest DOS clock, so two runs are comparable (validation) |
| `-secs N` | Run headless for N wall-clock seconds |
| `-shot FILE` / `-shotevery N` | Save PPM screenshot(s) |
| `-keys "t:sc:state,..."` | Feed timed keyboard events to the guest |
| `-wav FILE` | Capture audio to WAV |

The tracing flags (`-t`, `-xring`, `-trap`, `-dosdbg`, `-iotrace`, `-pll`,
`-snddbg`, `-flipdbg`, `-vscan`, `-dmd`, `-balldbg`, `-matdbg`, `-scoredbg`,
`-keepoverlay`, `-mem`,
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

## Legal

pfemu is a fan-made community project. It is not affiliated with, sponsored,
endorsed or approved by Rebellion or any other rights holder of Pinball
Fantasies. Pinball Fantasies and all related names are the property of their
respective owners.

pfemu contains no game files. To play, you need your own copy of the game.
