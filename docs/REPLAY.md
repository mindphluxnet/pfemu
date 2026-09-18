# Session record and replay

Record a session's keypresses with emulated-time stamps into a `.pfr` file
and replay it later on the same clock, so host stutter can't shift what the
game sees. Implemented in `src/replay.c` (hooks in `main.c`, `dev.c`,
`dos.c`, `fantasies.c`, `launch.c`, `release.c`). Use it from the launcher's
**Session** row or with `-record FILE` / `-replay FILE`.

## How it works

- **Records from the boot program**, including the table-select `F1`-`F4`
  keys. A session started with **Start at** records too: the boot program
  runs either way, so the only difference is that its first intro EXEC was
  skipped. The `.pfr` stores that as `start_table:` and replay forces it,
  because the event stream assumes whichever way the session began - a
  recorded table session replayed from the menu desyncs on the first key.
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
footer:  final emu_time + cpu.cycles, -wav capture hash (FNV-1a over the
         sample bytes, "none" without -wav) + sample count,
         FNV-1a file hash (mismatch refused loudly)
```

## Launcher

`show_launcher()` owns every replay-relevant setting per install. A mode row
(`Play` / `Record` / `Replay`) plus file field (default
`sessions/<install>_<date>.pfr`, `Browse...` for both modes - save dialog
with overwrite confirm for record, open dialog for replay) feeds the same
`{dir, prog, fullscreen, mode, path}` commit path as normal launch. The
last-used session file per install is remembered in
`PFEMU-STATE/pfemu_session.cfg` and restored into the field (a typed or
picked path always wins; record re-targets on install switch, replay keeps
the loaded file). A typed record path gains the `.pfr` extension when it
has none. The trainer checkbox is greyed out in record/replay mode; the
volume slider stays enabled in all modes. CLI and launcher are thin
frontends to the same injector. Details in replay mode opens a report
window: whether the file can play and why not, the recorded session
(events, duration, options, sound, capture hash), the recorded code vector
compared program by program against the selected install, and that
install's own detection report.

## Validation ("accurate" means)

- Replay twice -> `-wav` hash, `-shotevery` frames, and exit `emu_time/cycles`
  must all match. Any mismatch is a nondeterminism bug.
- Game entropy must come only from emulated sources (PIT, BDA tick `0x46C`,
  `INT 1Ah`), never host time.
- Manual play-through per table (Party Land first - most tested).

## Not goals

Local files only (no network/leaderboard), no cross-release replay (refused
by design). Mid-table savestates and direct-to-table have both landed (see
EMULATOR.md).

**No replay scrubber.** Pause, step, speed and seek were built and then
removed. Seeking is the part that cannot work: fast-forward runs at the
emulator's unthrottled ceiling of 22-32 MIPS against a 6 MIPS guest, so
about 4-5x real time - and the viewer speed control already offered 4x, so
a forward seek bought nothing over simply watching. Backward seeking was
worse: with no keyframes it re-simulated from t=0, which meant tens of
seconds of waiting to step back ten, over a window still painting the old
frame while the restored sound hardware played the intro music. Keyframes
would have bounded the backward case at roughly 1.3 MB per snapshot, but
not the forward one, so the whole feature came out rather than half of it
staying to disappoint. Lifting the ceiling means a dynarec, which
Performance in EMULATOR.md rules out as a rewrite.
