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
last-used session file per install is remembered as the `session` key in
`PFEMU-STATE/pfemu.cfg` and restored into the field **in replay mode
only** - so the file you just recorded is the one offered to replay. Record
mode builds a fresh target every time it is entered, because the remembered
path is the previous recording and reusing it would silently overwrite a
playthrough that cannot be reproduced. If that name already exists (two
recordings started in the same second), it gains a `_2`, `_3`, ... suffix
rather than the file being replaced. A typed or picked path always wins
over both; record re-targets on install switch, replay keeps the loaded
file. A typed record path gains the `.pfr` extension when it has none. The trainer checkbox is greyed out in record/replay mode; the
volume slider stays enabled in all modes. CLI and launcher are thin
frontends to the same injector. Details in replay mode opens a report
window: whether the file can play and why not, the recorded session
(events, duration, options, sound, capture hash), the recorded code vector
compared program by program against the selected install, and that
install's own detection report.

**A `-replay` with no `-d` picks the install the file needs.** The launcher
never runs for a replay, so without `-d` the install would otherwise be
whatever the detector lists first - and on a machine with several releases
side by side that is usually not the one the recording came from, which then
refuses for a reason that has nothing to do with the recording. The file
names its release, so the scan prefers a runnable install of that release and
says which one it took. This only chooses between installs nobody chose: an
explicit `-d` is never overridden, because an explicit wrong one deserves the
refusal rather than a silent substitution - though that refusal now names an
install that would have worked, if there is one. Identity is still the hash
vector; this only decides which install gets offered to
`replay_verify_install()`, which then checks it properly.

**`-keepoverlay` leaves the isolated copy behind.** Normally it is deleted at
exit; with the flag it stays and its path is printed. What is in it is
everything the guest wrote during the replay, which can include the game's
own `TABLEn.HI` high-score file - an independent reading of a run's final
score, written by the game rather than read out of its memory
(`tools/hiscore.py` decodes it). Only *can*: the game writes that file when
the table program quits, not at game over, so a recording that ends while a
table is still running leaves it untouched. The flag only affects the
end-of-session cleanup: a refused
attempt still removes its copy, because nothing ran in it. The directory is
then the caller's to delete.

**A refusal refuses the launch, not the session.** Everything from the
picker to the first executed instruction is one attempt: a replay whose
install does not match, a record target that will not open, the trainer
left on in either mode, a boot program that will not load. Any of these
shows its message box and then brings the picker back, on the same
installation, so the user can pick something else - rather than exiting,
which from the picker looked exactly like "Launch quits the app". A retry
starts from the command line's own values again, re-runs every init, and
disarms whatever the failed attempt had armed (`replay_abort()`: a parsed
replay, a header-only record file, an isolated overlay copy). The game
window is created after the last of those checks, so a refused launch never
flashes an empty window up. Out of memory is the one failure left that
still ends the process - it prints its box first. On the command line
nothing changed: with no picker to return to, every refusal exits with a
status.

## Validation ("accurate" means)

- Replay twice -> `-wav` hash, `-shotevery` frames, and exit `emu_time/cycles`
  must all match. Any mismatch is a nondeterminism bug.

  Two cautions on reading that, both learned the hard way in Spike B
  (docs/VERIFY.md):

  - The frames only became comparable once `-shotevery` was moved onto the
    emulated clock. It used to ride the present path, which is paced by the
    wall clock, so two runs of one replay on one machine already produced
    different files.
  - **The footer is the weakest of the three.** A replay stops on the
    recorded cycle, so `emu_time/cycles` agree by construction. A run that
    failed to open a file, never started the game and sat in text mode for
    the whole session still matched the footer exactly. The wav hash and the
    frames are what actually caught it.
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
