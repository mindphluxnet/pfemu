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

## Hostile input

Everything above is about a file this program wrote. A verification
service takes files it did not write, so `parse_file()` bounds every
number it will act on: `ips` and `speed` (NaN-safe, because a NaN walks
through every `<= 0` clamp downstream), the quality notch, `start_table`,
the footer, and the event count. Events must be sorted, must not mix the
cycle-stamped and legacy forms, and must not be stamped past the footer -
that last one is not cosmetic: `replay_should_stop()` returns 0 while
`ev_idx < nev`, so an event that never comes due means a replay that never
ends.

`-strict` adds the policy half, for a verifier rather than a player: it
refuses a file with no `file_hash:` line and one carrying pre-cycle
events, both of which ordinary play still accepts so that a player's own
older recordings keep working.

`-verify FILE` is the other half of the same story, at the other end of
the run: one JSON object saying whether this replay reproduced its
recording, and what score - if any - is eligible. A service reads that
and nothing else the emulator prints. See [Verification](VERIFY.md).

`tests/fuzz/` is the test for all of it - `-selftest` for the fixed cases,
a mutation driver for the rest. Its README explains why the harness has to
recompute the FNV-1a after mutating, which is also why that hash is not a
security property.

Two rules keep replays honest:

- **Trainer is incompatible.** Recording or replaying with it enabled is
  refused, and hotkeys `1`-`3`, `Z` stay dead in both modes.
- **Volume is live-only.** Slider, `-vol`, and in-window `-`/`+`/`*` keep
  working in both modes, are never stored in the `.pfr`, and never affect
  `-wav` (captured upstream of the gain).

While recording, a small red `REC` badge sits in the window corner (green
`PLAY` while replaying). Both are composed host-side into a back buffer -
they never reach the game, the `.pfr`, screenshots, or `-shotevery` captures.

## Ranked recordings

A replay copies the replaying machine's `PFEMU-STATE/`, and the recording
saw the player's. Those two are not the same input, and the difference
reaches the game. The high-score tables alone change it, and they do so
during play, not at the name entry. The Party Land vector, replayed with
every `table1.hi` entry at 99,999,999, is identical up to t=67 s. At
t=72.58 s one score event comes 1 ms early, on ball 1 at 4.55M, far from
any table entry. With the file missing, the replay diverges at t=6.6 s.
The same inputs give a different game.

So a recording that is meant to rank is made against a state that neither
side chooses:

    pfemu -record run.pfr -ranked

`-ranked` records against the **canonical state** `canonical-1` instead of
the install's `PFEMU-STATE/`. That state is a temporary overlay holding one
file, `SOUND.CFG`, built from the session's own sound setting:

- **sound off:** the 16 bytes SETSOUND writes for `NOSOUND.SDR`.
- **sound on:** 25 bytes, `SBLASTER.SDR` at 220h, IRQ 7, with the quality
  notch at 0x14.

There are no `*.HI` files, so every table starts from the game's built-in
high scores, the same way a fresh install does. There is no `PINBALL.CFG`
either, and the boot never reads one anyway. The DOS layer also hides the
install's own copies of those files (`SOUND.CFG`, `PINBALL.CFG`, `*.HI`),
the same set `release.c` excludes from identity. A `TABLE1.HI` left in the
game directory by real DOS therefore cannot reach a ranked session, on
either side. Whatever the game writes goes into the temporary overlay and
is deleted at exit (`-keepoverlay` keeps it). As a result, the player's own
high-score table is neither shown during a ranked game nor updated by it.

The header records it as `state: canonical-1`, and `overlay:` then holds
the hash of that overlay. The state is fully determined by `sound:` and
`quality:`, so the parser recomputes the hash and refuses a file whose
`overlay:` does not match. It also refuses a `state:` it does not know. A
definition that changes gets a new name. `canonical-1` is never edited,
because stored recordings replay only against the same bytes. The bytes
are spelled out in `src/replay.c` and deliberately not shared with the
launcher's `write_sound_cfg()`, which writes a preference that is free to
change.

On replay, a canonical file builds the same overlay itself. It never looks
at the replaying install's `PFEMU-STATE/`, and it skips the check that the
install's Sound setting matches the file's. **`-strict` refuses any file
without the line** (`error: state_not_canonical` under `-verify`). Such a
file still replays for watching, but it is no evidence of a score.

What the canonical state does not cover: the game's data files come from
the install. `release.c` pins the programs and most of `INTRO.MOD`, but
not the other `.MOD` files. The game opens those for writing, so copies of
them also end up in the overlay. An install that differs there produces a
mismatch, not a false verification.

## File format (`.pfr`)

Text, one file per session:

```
header:  magic + version, release_id, code hash vector, summary (display only),
         boot + start program, ips, speed, nopatch/nolzexe, sound on/off +
         quality notch, 6-byte options blob, trainer_assert_off,
         overlay hash/snapshot ref, state (canonical-1 when ranked, absent
         for the player's own PFEMU-STATE/), source dir (hint only)
events:  cycles, emu_time, scancode, down/up - sorted (cycles are the clock;
         integer-exact so a fast-counter RNG reads what it read on record)
footer:  final emu_time + cpu.cycles, capture hash (FNV-1a over the sample
         bytes, "none" only when the session had no sound at all) + count,
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
  - **A capture observes the run; it must never alter it.** The first attempt
    at the fix above clamped the instruction batch to the capture deadline,
    the way the replay, `-keys` and `-untilemu` deadlines are clamped. That
    made taking a picture change the sound: the same replay gave wav
    `dd938f6bd1540842` with `-shotevery` and `dfb1427eef9a2239` without. The
    clamp is right for an injection - a key has to land on the instruction it
    is due on - and wrong for an observation. Frames are now taken at the
    first batch boundary at or after each due time, which two runs already
    agree on, and no batch is moved.
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
