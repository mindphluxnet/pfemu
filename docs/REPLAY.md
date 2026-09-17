# Session record / replay — plan

Goal: let a user record a Pinball Fantasies session (keypresses + timing)
and replay it later accurately. Phases 1 (deterministic replay core) and 2
(launcher) are implemented in `src/replay.c` with hooks in `src/main.c`,
`src/dev.c`, `src/dos.c`, `src/fantasies.c`, `src/launch.c` and
`src/release.c`; only the optional polish phase is still open
(direct-to-table was decided against, §3.4).
This doc is the agreed plan and the reference for what "accurate" means.

## 1. Background: what already exists

- Direct table boot: `pfemu.exe -p TABLEn.PRG` loads a table without the
  intro/selector (`src/main.c` arg parsing, `dos_exec()` in `src/dos.c`).
  Proven as a test path in `docs/FANTASYA.md` (all four tables load,
  locators arm), but noted there as "not a playable substitute" for the
  full `PINBALL.EXE/PF.EXE → INTRO.PRG → TABLEn.PRG` chain.
- Timed input injection: `-keys "t:scancode:state,..."` in `src/main.c`
  (`run_keyscript()`) already drives `kbd_key()`.
- Headless aids: `-secs`, `-ips`, `-speed`, `-wav`, `-shotevery`, `-mem`,
  `-dumpseg`, `-t`.
- Full launch chain documented in `docs/WRITEUP-PHASE2.md §2`:
  shell hooks `INT 9/24`, API on `INT 65h`, 6-byte state blob shared via
  `INT 65h` / `PINBALL.CFG`, each program `EXEC`s its own `.SDR` driver
  (TSR on `INT 66h` + `INT 8`, owns machine time).

## 2. Why naive "keypresses + physics" does not replay accurately today

1. **Wrong clock.** `run_keyscript()` schedules on `real = wall * speed`
   (`src/main.c`, main loop), not on `emu_time` / `cpu.cycles`
   (`src/dev.c`). Host stutter, coarse `plat_sleep_ms(1)`, the
   `fell behind` resync, and the present-phase early-break all shift what
   emulated time a wall-time event lands on.
2. **Direct-table ≠ full boot.** The shell's `INT 9/24/65h` hooks, the
   6-byte blob handoff, intro options poke, and per-table `.SDR` load are
   skipped by `-p TABLEn.PRG`. Equivalence is unproven.
3. **Guest-visible host time.** `INT 21h AH=2Ah/2Ch` returns
   `time(NULL)/localtime()` (`src/dos.c`); `FindFirst/Next` returns host
   timestamps. Must be frozen/virtualized on replay.
4. **Host input leakage.** `kbd_reconcile_physical()` (`src/main.c`),
   `kbd_release_all()` on focus loss, StickyKeys suppression, and the
   `Alt+Enter` / `F11` / `ScrollLock` keys inject or suppress `kbd_key()`
   events outside any log. (Volume `-`/`+`/`*` keys are host-sink-only per
   `src/sound.c` and stay live — see §3.3.)
5. **Persistent state.** The `PFEMU-STATE/` write overlay (`.hi` scores,
   `INTRO.MOD` sentinel bytes, `SOUND.CFG`, `pfemu_*.cfg`) persists across
   runs (`src/dos.c`, `src/launch.c`, `src/fantasies.c`). Same inputs +
   different overlay can diverge.
6. **Unlogged config.** Release, `-ips`, `-speed`, `-nopatch`/`-nolzexe`,
   sound-quality notch (guest mixing rate + 386 budget), and the launcher
   6-byte options all change execution; none are in `-keys`. (Volume is
   host-sink-only and deliberately excluded — §3.3. Trainer is forced
   off — §3.2/§3.3.)

Counterpoints (why this is still feasible): `emu_time` is `cycles/ips`
(`src/dev.c`); PIT/PIC/VGA/`sb_tick()` are functions of emulated time;
`audio_volume`/waveOut drops are host-sink-only (`src/sound.c`). The
`.SDR` PLL calibration (~2.2 s, narrow lock band) is emulated-time
deterministic and should repeat given identical inputs.

## 3. Design

### 3.1 Replay file (`.pfr`)

Text or binary, one file per session:

```
header:
  magic + version
  release_id            # floppy / power_pack / deluxe / demo (identity key)
  code hash vector      # the five program SHA-256s release.c already computes
  summary               # display only, never matched on
  boot prog + start prog (TABLEn for direct-table replays)
  ips, speed (=1 on record), nopatch/nolzexe flags
  sound on/off, sound-quality notch, 6-byte options blob, fullscreen
  trainer_assert_off    # header records the invariant, not a setting
  overlay hash / snapshot ref
  source install dir    # hint only, never identity
  # volume is deliberately absent: host-sink-only, always live
events (sorted):
  cycles, emu_time, scancode, down/up (cycles are the injection clock -
  integer-exact, so a guest RNG sampled from a fast counter reads what it
  read on record; pre-cycle three-field files still replay on emu_time)
footer:
  final emu_time + cpu.cycles, optional -wav hash
  file_hash: FNV-1a over all preceding bytes (refused on mismatch,
  so edits and truncation fail loudly; files predating it warn once)
```

`summary` and `dir` are hints/display only. Identity is `release_id` +
hash vector, per `src/release.c` (dir name, boot filename, and
timestamps are explicitly not identity).

### 3.2 Recording (`-record FILE`)

- Refuse to start when the trainer is enabled (`pfemu_cheats.cfg` on, or
  equivalent CLI state); the launcher also greys the trainer checkbox in
   record mode. Trainer hotkeys (`1`-`3`, `Z`) are dead while recording.
- Log every `kbd_key()` entry with `emu_now()` at record time.
- Record `down` and `up` (flipper overlap repair depends on both).
- Force `speed=1` during record; record effective `-ips`.
- Snapshot (or at least hash) the effective `PFEMU-STATE/` + configs in
  the header so replay can detect drift.
- v1 records from the boot program **including** table-select `F1–F4`,
  not from table start. Avoids the §2.2 fidelity question.

### 3.3 Replay (`-replay FILE`)

- Refuse to start when the header or the current install has the trainer
   enabled; trainer hotkeys (`1`-`3`, `Z`) stay dead for the whole replay.
- Volume stays live and unrecorded: the slider, `-vol`, and the in-window
  `-`/`+`/`*` keys keep working (host gain only, `-wav` is captured
  upstream per `src/sound.c`), and are never written into the `.pfr`.
- Inject on `emu_time` (instruction-exact `emu_now()`), replacing the
  wall-clock `run_keyscript()` path. No live-keyboard merge in v1 (or an
  explicit `--allow-live-keys` escape hatch that voids accuracy).
- Force recorded `-ips`/`-speed`; freeze `INT 21h 2Ah/2Ch` and file
  timestamps to recorded values (or a fixed epoch).
- Isolate `PFEMU-STATE/`: temp copy or read-only mode; never write the
  user's real overlay during replay.
- Suppress host leakage: disable `kbd_reconcile_physical()`, focus-loss
  releases, accessibility side paths, `Alt+Enter`/fullscreen/screenshot
  keys (or route them around the guest + log). Volume keys are the
  exception and stay live.
- End condition: footer `emu_time`/`cycles`, or `ScrollLock`/window close
  as today; report mismatch stats at exit.

### 3.4 Direct-into-table (decided against)

- Technically `dos_exec(TABLEn)` already works; the question was fidelity
  (options blob, `.SDR` per-table load, `.hi` handling, attract music
  slot behaviour noted in `src/launch.c`).
- Dropped: the people who'd use replays know how to reach gameplay fast
  (Space skips the intro), so a boot-to-table shortcut isn't worth a
  second fidelity question. v1 records from the boot program, full stop,
  and files naming anything else stay refused.

## 4. Launcher integration

`show_launcher()` (`src/launch.c`) is modal pre-boot (`src/main.c`) and
already owns every replay-relevant setting per install (`SOUND.CFG` +
quality, `pfemu_audio.cfg`, `pfemu_options.cfg`, `pfemu_cheats.cfg`,
`pfemu_display.cfg`, install `dir`/`boot`, `reload_for_dir()`).

- Extend `LaunchChoice` (`src/pfemu.h`) from `{dir,prog,fullscreen}` to
  `{dir,prog,fullscreen,mode,path}` with
  `mode = play | record | replay`.
- UI (same dialog, below fullscreen): `(•) Play ( ) Record ( ) Replay`
  + file field with `Browse…`, default
  `sessions/<install>_<date>.pfr`. Reuse the `ID_LAUNCH` commit path:
  write configs first, then hand `mode/path` to `main()`.
- Record: normal boot path (`r->boot`); `main()` arms recording on the
  selected install. Header auto-captures release, `ips`, quality,
  options. The trainer checkbox is greyed out in record mode, and
  recording refuses to start if cheats are on.
- Replay: file picker for `.pfr`; run the auto-restore below (§4.1)
  before enabling `Launch`. The trainer checkbox is greyed out in replay
  mode; replay refuses trainer-enabled headers/installs. The volume
  slider stays enabled in all modes. CLI `-record/-replay` and launcher are thin
  frontends to the same emu-time injector.

### 4.1 Auto-restore of the recorded release on replay load

On replay-file load, `release_scan()` the current installs and:

1. **Exact:** runnable install with same `rel->id` and same hash vector;
   prefer the recorded `dir` if it still matches. Auto-select silently.
2. **Same release, different copy:** first runnable install with same
   `rel->id`. Auto-select, show `Detected: <summary>` as today.
3. **No match:** refuse auto-launch (`Launch` disabled, as for unknown
   installs today); show recorded-vs-found report via the existing
   `Details` path. Never apply one release's memory layout to another.

## 5. Validation (defines "accurate")

- Replay twice → compare `-wav` hash + `-shotevery` frames + exit
  `emu_time/cycles`. Any mismatch is a nondeterminism bug.
- A/B: full-boot record vs. direct-table record of the same play;
  promotes §3.4 only if frame/audio-identical.
- Confirm game entropy comes only from emulated PIT/BDA tick (`0x46C`)
  / `INT 1Ah`, not host time.
- Manual play-through per table (Party Land first — most tested).

## 6. Phases

1. **Deterministic replay core:** emu-time injector, `-record/-replay`,
   time freeze, overlay isolation, input-leak suppression.
2. **Launcher:** mode + picker + auto-restore (§4–4.1).
3. **Polish (optional):** mid-table savestates (RAM+PIC/PIT/VGA/DOS),
   replay scrubber. (Session badges are done: tiny static REC / green PLAY
   twins, host-only, composed tear-free in a back buffer; captures read the
   framebuffer, so validation artifacts stay pixel-clean.)

(Direct-to-table replay was phase 3; dropped per §3.4.)

## 7. Non-goals / decided

- Local files only. No network/leaderboard.
- No cross-release replay (refused by design).
- Volume is live-only: slider, `-vol`, and in-window `-`/`+`/`*` apply
  during record and replay, are never stored in the `.pfr`, and never
  affect the `-wav` capture (upstream of gain).
- Trainer is incompatible with replay: recording requires it off and
  cannot enable it; replay requires both header and install off; trainer
  hotkeys are dead in both modes.
- Open: exact `.pfr` encoding (text for v1 readability?); session
  naming/retention UX in the launcher.
