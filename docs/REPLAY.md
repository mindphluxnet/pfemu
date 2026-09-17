# Session record and replay

Record a session's keypresses with emulated-time stamps into a `.pfr` file
and replay it later on the same clock, so host stutter can't shift what the
game sees. Implemented in `src/replay.c` (hooks in `main.c`, `dev.c`,
`dos.c`, `fantasies.c`, `launch.c`, `release.c`). Use it from the launcher's
**Session** row or with `-record FILE` / `-replay FILE`.

## How it works

- **Records from the boot program**, including the table-select `F1`-`F4`
  keys. There is no direct-to-table shortcut - booting the full
  `PINBALL.EXE/PF.EXE -> INTRO.PRG -> TABLEn.PRG` chain is the only path, so
  there is no second fidelity question.
- **Injection clock is emulated time** (`emu_time` / `cpu.cycles`), not wall
  time. The old wall-clock `-keys` path is replaced during replay; no live
  keyboard is merged in.
- **Identity is release + code hashes** (`release_id` + the five program
  SHA-256s from `release.c`), never directory name or timestamps. Replay
  refuses a file whose vector differs, and restores the recorded install in
  the launcher (exact match preferred, same-release copy accepted, otherwise
  Launch stays disabled with a recorded-vs-found report).
- **Environment travels with the file**: `ips`, `speed` (= 1 on record),
  `nopatch`/`nolzexe`, sound on/off + quality notch, the 6-byte options blob,
  boot/start program. These are forced on replay.
- **Guest-visible time is frozen** (`INT 21h` date/time, file timestamps) and
  **writes are isolated** - the game runs against a throwaway overlay copy,
  never your real `PFEMU-STATE/`.
- **Host leakage is suppressed**: physical-keyboard reconciliation,
  focus-loss releases, and `Alt+Enter`/screenshot keys are disabled or routed
  around the guest during replay.

Two rules keep replays honest:

- **Trainer is incompatible.** Recording or replaying with it enabled is
  refused, and hotkeys `1`-`3`, `Z` stay dead in both modes.
- **Volume is live-only.** Slider, `-vol`, and in-window `-`/`+`/`*` keep
  working in both modes, are never stored in the `.pfr`, and never affect
  `-wav` (captured upstream of the gain).

While recording, a small red `REC` badge sits in the window corner (green
`PLAY` while replaying). Both are composed host-side into a back buffer -
they never reach the game, the `.pfr`, screenshots, or `-shotevery` captures.

## File format (`.pfr`)

Text, one file per session:

```
header:  magic + version, release_id, code hash vector, summary (display only),
         boot + start program, ips, speed, nopatch/nolzexe, sound on/off +
         quality notch, 6-byte options blob, trainer_assert_off,
         overlay hash/snapshot ref, source dir (hint only)
events:  cycles, emu_time, scancode, down/up - sorted (cycles are the clock;
         integer-exact so a fast-counter RNG reads what it read on record)
footer:  final emu_time + cpu.cycles, optional -wav hash,
         FNV-1a file hash (mismatch refused loudly)
```

## Launcher

`show_launcher()` owns every replay-relevant setting per install. A mode row
(`Play` / `Record` / `Replay`) plus file field (default
`sessions/<install>_<date>.pfr`, `Browse...` for replay) feeds the same
`{dir, prog, fullscreen, mode, path}` commit path as normal launch. The
trainer checkbox is greyed out in record/replay mode; the volume slider stays
enabled in all modes. CLI and launcher are thin frontends to the same
injector.

## Validation ("accurate" means)

- Replay twice -> `-wav` hash, `-shotevery` frames, and exit `emu_time/cycles`
  must all match. Any mismatch is a nondeterminism bug.
- Game entropy must come only from emulated sources (PIT, BDA tick `0x46C`,
  `INT 1Ah`), never host time.
- Manual play-through per table (Party Land first - most tested).

## Not goals

Local files only (no network/leaderboard), no cross-release replay (refused
by design). Mid-table savestates and a replay scrubber are open polish ideas;
direct-to-table replay was considered and dropped.
