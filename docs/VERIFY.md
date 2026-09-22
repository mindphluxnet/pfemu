# Server-side replay verification (design note)

**Status: Spike A is built, the rest is not.** `-scoredbg` (src/fantasies.c)
locates the score and prints the per-attempt table described below; nothing
else here exists. Two spikes decide whether any of the rest gets built
(see [Phasing](#phasing)); everything after them is plumbing.

The idea: a player uploads a `.pfr`, a headless Linux build re-simulates it,
and the server derives the score itself. The client's emulator is never
trusted for anything - it contributes inputs, not outcomes.

## Why the architecture already fits

A `.pfr` (see REPLAY.md) is not a recording of what happened. It is
identity + environment + a list of `(cycles, scancode, up/down)` events on
the emulated clock. That is exactly a verification input package, and the
properties it already has are the ones a verifier needs:

- **Identity is release + code hashes**, never a directory name. A replay
  whose five program SHA-256s do not match is refused before it starts
  (`src/release.c`, `replay_verify_install()`).
- **The injection clock is `cpu.cycles`**, not wall time. Host stutter -
  and, in principle, host speed - cannot shift what the game sees.
- **Guest-visible host time is frozen** on replay: `INT 21h` `AH=2Ah`/`2Ch`
  and file timestamps come from a fixed epoch.
- **Writes are isolated** to a throwaway overlay; the install is read-only.
- **The trainer is refused** in both record and replay mode, and savestates
  are refused during replay - so `1`/`2`/`3`/`Z` and state-scumming are
  already closed.
- **The footer carries** final `emu_time`, final `cpu.cycles`, and an FNV-1a
  over the `-wav` samples. REPLAY.md already names the criterion:

  > Replay twice -> `-wav` hash, `-shotevery` frames, and exit
  > `emu_time/cycles` must all match. Any mismatch is a nondeterminism bug.

  Substitute "on two platforms" for "twice" and that is the entire
  feasibility question for this document.

Two things are genuinely missing: **the score** (nothing in the emulator
reads one today) and **a build that runs without a window**.

## What this can and cannot prove

It proves, unforgeably: *these inputs, on this exact build, under this exact
configuration, produce this score.* A hacked client is irrelevant by
construction, because nothing the client computes is believed.

It cannot prove *a human pressed those keys in real time*. A `.pfr` is plain
text and FNV-1a is a checksum, not a MAC, so an input stream can be authored
by hand or by a solver and will verify perfectly. Signing does not help - the
client would hold the key.

This is the shape of the problem, not a defect to engineer around. Either
accept it and run one board, or run Human and TAS boards and let TAS be a
feature. Input-timing heuristics for "was this human" are weak and will
eventually accuse someone innocent; do not ship one.

## Eligibility and boards

Two different questions, and conflating them is the mistake to avoid:

- **What is accepted for verification** - a hard gate. Non-negotiable,
  because outside it the verifier cannot produce a meaningful number at all.
- **What is ranked against what** - a presentation choice. Cheap to change
  later, because a stored attempt never needs re-verifying to be re-sliced.

### Accepted for verification

Checkable from the `.pfr` header alone, with no simulation, which makes it a
free triage tier:

| Constraint | Value | Why |
| --- | --- | --- |
| Release | `floppy`, `power_pack`, `deluxe` | The 1993 demo is excluded - nobody competing would use it, and it has no options structure at all (`cfg_buf = 0`). |
| Balls | 3 | Classic machines are 3-ball. A 5-ball game is a different game and is not comparable. |
| Players | 1 | See [Player count](#player-count). Multi-player interleaves scores across alternating balls and is dropped entirely. |
| Trainer | off | Already asserted by the format. |

Player count is the one line here that cannot be decided from the header; it
comes out of the run (see below) and rejects the submission after the fact.

### Ranked

Everything else - all six option bytes plus `ips` - is **stored with every
verified attempt** and never discarded. Storage is free, and an attempt whose
full configuration is on record can be re-sliced into new boards at any time
without re-running a single instruction. That is the real argument for
recording broadly: not disk, but never having to re-verify.

What gets *displayed* is narrower, and the constraint is population, not
storage. A board with four entries is not a leaderboard. So:

| Option | Boards | Why |
| --- | --- | --- |
| Resolution | **Normal and High, separately** | Not cosmetic. `init_ballspeed` skips its 5/6 downscale in hi-res (main.c:1198), so ball speed genuinely differs. Hi-res also auto-bumps `emu_ips` when `-ips` is not given, so the two boards sit on different modelled CPU speeds - which is fine, as long as each board pins its own. |
| Ingame Music | pinned | The MOD player costs guest CPU against a fixed `ips` budget, so turning it off can change frame timing. |
| Angle, Scrolling | pinned, pending investigation | **Open question** - see below. If either turns out to change physics, it graduates to a split axis like Resolution. |
| Color Mode | pinned | Almost certainly cosmetic. |

The rule that generates that table: **split on an axis when it demonstrably
changes how the game plays; pin it otherwise.** A cosmetic axis left open
fragments the board for no competitive reason, and four tables times a fully
open option blob is 384 boards, of which about 380 would be empty. Start
narrow, keep the data wide, and widen the display when a configuration
actually attracts players.

## Attempt segmentation

A `.pfr` is a whole session: boot, intro, menu, table, possibly back to the
menu, possibly another table, possibly several games on the same table. A
verifier has to cut that into attempts. Every primitive needed already
exists.

**Table identity is free.** `fantasies_on_exec()` fires from `dos_exec()`
(dos.c:604) on every EXEC, and `prog_slot()` resolves it to 1-4 through the
detected release's own layout - so it never guesses from a filename. Leaving
a table comes back through `dos_terminate()`. The session is therefore
already sliced into *table residencies* with no new machinery.

**A residency is not an attempt.** The player can drain out and start a new
game without leaving the table, so the boundaries have to come from game
state.

**The ball counter is already located.** `fantasies_patch_balls()` finds the
ball-lost handler by masked opcode signature and stashes the DS offset of the
counter. `-scoredbg` extends that same signature backwards by three
instructions and resolves the segment through the `data_seg` majority vote the
spring and jump locators already use, so the counter, `NO_OF_BALLS`, `PLAYER`
and `PLAYERS` all come out of one site.

Two corrections to what this document originally assumed, both from the
binary. The counter is `BALLS[11]` and it counts **up** from 1 to
`NO_OF_BALLS`; there is no `balls_left` countdown. (The byte in the existing
signature is `FE /0` = `INC`, though the comment above it says `dec` - the
infinite-balls cheat NOPs the increment, so its behaviour was never in doubt,
only its description.) And the match does not put a ball back by rewinding
that counter: `_knacket` sets `XXBALLE`, which routes into the same
`shoot_again` task an extra ball uses, so play resumes with the counter left
at `NO_OF_BALLS + 1`. A ball number past the last ball is therefore normal at
the end of every game and says nothing on its own; what does say something is
a *launch* observed while the counter is past the last ball, which is what
`-scoredbg` counts.

**Address hooks already exist.** `cpu_step()` (cpu.c:797) already dispatches
"call me when the guest executes this linear address" for the ball-gap hooks,
and that one is live in normal play rather than only under `-balldbg`. So
boundaries can be event-driven off the ball-lost handler instead of polled -
exact, and nearly free.

The state machine, per residency:

- **ARMED** - `balls_left` reads its starting value and the score reads 0.
  A game is pending but the player count is not yet fixed.
- **LOCKED** - the first launch of ball 1: the ball is away, so the player
  count can no longer change. Sample it here and reject the session if it is
  not 1.

  The original plan was to read this off `SPRING_VALID`, already located by
  `fantasies_patch_spring()`, as a true->false transition while the ball
  counter is still at its starting value. **That does not work, and the first
  playtest is what said so:** the flag clears exactly twice per ball where
  there was one plunger shot. It is driven by the table's own lane switches
  (`BYGEL12`/`BYGEL28` in `PLAND.ASM`), and a ball crosses them more than once
  on its way out, so the level is honest but the transition count is not a
  launch count.

  The launch is an instruction instead. `SPRINGUP` (`FANTASIE.ASM`) computes
  the plunger speed from how far the spring was drawn, dithers it with the
  free-running counter, and stores it into the ball's vertical velocity;
  nothing else reaches that store, and it is skipped entirely when
  `SPRING_VALID` is false. Hooking it through `cpu_step()` is exact and needs
  no qualifier at all. `-scoredbg` still counts the flag transitions
  separately, and reports both numbers, so the discrepancy stays visible
  instead of turning into folklore.
- **DRAINED** - the ball counter passes `NO_OF_BALLS`.
  **Provisional only. This is not the end of the attempt.**
- **ENDED** - the table returns to attract mode: `GO_DEMO_MODE`'s
  `MOV DEMOMODE,TRUE`, hooked through `cpu_step()`. This is the authoritative
  terminal signal and the only point at which the score may be sampled - and
  it has to be this instruction rather than anything later, because
  `TO_DEMO_FROM_GAME` calls `GO_DEMO_MODE` and then `zeroscore`, in that
  order, a few instructions apart.

### Why running out of balls does not end an attempt

Two separate mechanisms put balls back:

- **The match.** After all balls are lost the game draws a random number
  0-9 and displays it; on a hit the player gets another go, and the score
  carries on accumulating. This is the classic real-machine match feature,
  and it means a score sampled at DRAINED can undercount by an entire
  additional ball's worth of play.
- **Extra balls awarded during play.** `LET_HIM_SHOOT_AGAIN` decrements
  `XBALLS` and goes straight back to a new ball without touching the ball
  counter at all, so the counter is not a count of balls played either.

So the segmenter must treat DRAINED as "possibly over" and wait for attract
mode to commit. The attract-mode entry is `GO_DEMO_MODE` (`FANTASIE.ASM`),
signature-scanned the same way as everything else in `fantasies.c` and hooked
through the existing `cpu_step()` address-hook mechanism.

The match draw is **not** a verification problem. Its entropy comes from the
emulated machine, and REPLAY.md already stores event cycles integer-exact
specifically so "a fast-counter RNG reads what it read on record" - so the
match outcome reproduces on the server like everything else. It is, however,
an unusually sensitive canary: a one-cycle divergence can flip the draw and
change the final score outright. That makes a session containing at least one
match an excellent golden-vector candidate (see
[Cross-platform determinism](#cross-platform-determinism)).

Output per attempt: `(table, index, start_cycles, end_cycles, score,
terminated_how)`. Same-table-repeated and mixed-table sessions both fall out
of this without special cases.

An attempt is only rankable if it started from a clean start observed *in
this session* and reached attract mode on its own. Quitting a table mid-game
is recorded as abandoned, not scored.

**The segmenter must be strictly read-only.** No poking guest state, ever, or
a scoring run diverges from a normal one. Directly testable: replay the same
`.pfr` with and without the flag and the footers must be byte-identical.

### Player count

Players are added with `F1`-`F5` while the ball sits on the spring and has
not yet been shot for the first time, or during attract mode, and the count
can still change up until that first launch.

Context disambiguates the scancodes completely - `F1`-`F4` mean table select
at the menu and add-a-player inside a table, and `fantasies_on_exec()` has
already told us which program is loaded, so the emulator always knows which
one it is looking at.

What that context is *not* is available before the run: it is a consequence
of guest execution, so there is no header- or event-stream-only pre-filter
that rejects a multi-player submission without simulating. This does not
matter. The check lands at the LOCKED transition, which is boot + intro +
table load + first launch into the session - a minute or two of emulated
time, seconds of CPU. A multi-player submission is **aborted early**, not
after a full run, so the cost of rejecting one is bounded by time-to-first-
launch rather than by session length. Early abort, not pre-filter.

The count itself must be read from guest memory at LOCKED. Where the game
stores it is an open question - trace it, do not assume.

## Locating the score

Use the method the rest of `fantasies.c` uses, not hardcoded per-release
offsets: masked opcode-signature scan at table load, immediates wildcarded,
address read out of the immediate, and **confirmed unique by static byte-scan
across all shipped `TABLE1-4.PRG` of every ranked release**. The reconstructed
`FANTASIE.ASM` already cited at fantasies.c:1015 is the map for finding the
routine; the byte-scan is what makes the result trustworthy. Release detection
already demonstrates why hardcoding loses - the options struct alone sits at
three different DS offsets across builds.

Cross-check against a second, independent source: the DMD score-display
routine has to read the score from somewhere, so the formatter gives a second
handle on the same value. Score variable and rendered DMD agreeing is the
standard to hold to here; one source is a guess.

Expect the score to be BCD or multi-word. Verify monotonicity across an
attempt - a decrease means the address is wrong, or something worse, and
should invalidate the whole submission rather than get clamped.

### What that found

The score is `SIFFRORNA` - "the digits" - **12 unpacked BCD bytes, most
significant first**, one digit per byte, ceiling 999,999,999,999.
`FANTASIE.ASM`'s `addscoreBCD` is twelve unrolled `mov al,[di+n] / adc
al,[si+n] / aaa / mov [di+n],al` blocks running from index 11 down to 0, which
is where both the layout and the digit range come from.

Two signatures name the address, written for unrelated purposes and required
to agree:

| Source | Shape | What it is |
| --- | --- | --- |
| `zeroscore` (`PLAND.ASM`) | `PUSH ES / PUSH DS / POP ES / MOV CX,6 / MOV AX,0 / MOV DI,`*imm*` / REP STOSW / POP ES / RETN` | the game wiping the score on the way back to attract mode |
| `ONLY_SCORE` (`FANTASIE.ASM`) | `MOV SI,`*imm*` / MOV BX,0C8h / PUSH CS / POP ES / CALL DWORD PTR ES:[PEKOR]` | the dot-matrix print - the number the player is reading |

Static byte-scan over all twelve `TABLE1-4.PRG` of the floppy, Power Pack and
Deluxe releases: `zeroscore` matches exactly once per program, the DMD print
exactly twice, and in all twelve the three immediates are the same address.
The same scan validates the other five locators - attract-mode entry, game
start, the ball/player site, the add-player handler and the plunger launch -
at one, one, one, two and one matches respectively, with every duplicated
operand agreeing.

The launch locator gets a cross-check of its own for free: the word
`SPRINGUP` stores the plunger speed into is the same word
`fantasies_patch_jump()` already located from the ball's motion integrator, by
a signature with nothing in common with it. Both agree in all twelve
programs, which is what makes "this store is the launch" a measurement rather
than a reading of the reconstructed source.

The addresses
themselves differ per table *and* per release (`DS:45B8` in floppy Table 1,
`DS:4608` in the Deluxe one), which is the usual argument against hardcoding.

The 1993 demo matches none of them, as expected: its programs are LZEXE-packed
on disk. It is excluded from verification anyway, and nothing checked whether
the signatures hold in the unpacked image.

That scan is `tools/scorescan.py`, so the claim is re-runnable rather than
historical - and it is what a newly identified release has to pass before it
can be ranked:

```
python tools/scorescan.py FANTASY FANTASYA FANTASYDX
```

## Cross-platform determinism

The other thing that can kill the project. The evidence is encouraging but
the traps are specific:

- **`-ffp-contract`.** `emu_time`, the PIT phase math and the VGA phase math
  are doubles. IEEE `+ - * /` is exactly specified, but gcc and clang default
  to fusing `a*b+c` into an FMA, which changes results. Build with
  `-ffp-contract=off` and no `-ffast-math`. Same reasoning that already keeps
  `/fp:fast` off under MSVC because the PLL lock needs exact comparisons.
- **Strict aliasing.** MSVC is effectively no-strict-aliasing; gcc `-O2` is
  not, and this codebase type-puns over guest RAM constantly. Build with
  `-fno-strict-aliasing`.
- **Latent UB.** Shift counts >= width, signed overflow and friends in
  `cpu.c` - x86 masks shift counts, C does not define them. One UBSan run on
  the Linux build will surface these, and fixing them improves the Windows
  build too.
- **libm is already gone** from every guest-visible path: dev.c:439 removed
  `fmod()`/`floor()` deliberately. There are no transcendentals anywhere the
  guest can observe. The single most encouraging fact in the file.
- **Host time is already virtualized** on replay, so there is no clock to
  leak.

The gate: a golden-vector suite. A set of `.pfr` files with expected
`end_emu`, `end_cycles`, `-wav` hash and periodic frame hashes; both
platforms must reproduce all of it exactly, on every commit. Include at least
one session in which the end-of-game match fires: its draw is cycle-derived,
so it amplifies a one-cycle divergence into a different final score, which
makes it the most sensitive vector in the suite. When something
diverges, snapshot at intervals and bisect with `tools/pfsdiff.py`, which
already diffs section by section.

That CI gate is worth having **even if the leaderboard is never built** - it
is a regression net for the emulator itself.

## The headless Linux build

Smaller than it looks. The emulation core - `cpu.c`, `vga.c`, `dev.c`,
`bios.c`, `fantasies.c`, `lzexe.c`, `png.c` - contains no Win32 at all.

| File | Win32 surface | Work |
| --- | --- | --- |
| `launch.c` | the GUI launcher | Excluded from the headless build entirely. No work. |
| `main.c` | window, present, keyboard, timing | Split into a host layer and the emulation loop, plus a null host. The only real refactor. |
| `sound.c` | the DirectSound sink | Null sink. All synthesis is already platform-free. |
| `dos.c`, `release.c`, `replay.c`, `snapshot.c`, `cfg.c` | `FindFirstFile`, `GetTempPath`, `GetFullPathName` | A small posix shim. |

Two specifics:

- **Case sensitivity.** DOS paths are uppercase, ext4 is not case-folding.
  The overlay and open paths need a case-insensitive resolver.
- **Speed forcing.** `replay.c` currently forces the recorded speed (= 1 on
  record) over `-speed`, so a replay plays back in real time. Since the
  injection clock is `cpu.cycles` and guest-visible host time is already
  frozen, `speed` *should* be pure host pacing with no guest-visible effect -
  but that is a hypothesis, and the measurement is cheap: replay the same
  file at 1x and at 50x and compare footers. Verification unthrottles it.

Headless also drops `vga_render()`, the resampler, the EQ chain and the audio
device - all host-side, all downstream of anything the guest can see.

## Pipeline

Three tiers, cheapest first:

0. **Header only** (microseconds, no simulation): release in the allowlist,
   balls = 3, trainer asserted off, file hash intact, footer present,
   duration and event-count caps.
1. **Parse**: event stream sanity, monotonic cycles, bounded rates.
2. **Simulate**: headless replay -> segmentation -> player-count check ->
   per-attempt scores -> database, full configuration stored alongside.

**Fuzz the `.pfr` parser before writing a line of server code.** It is C
eating attacker-controlled text, and it - not the emulator - is the real
attack surface. Workers run sandboxed: container, no network, read-only game
directory, tmpfs overlay, CPU and wall-clock timeouts.

The server needs its own copies of each supported release. Uploads are fine
(a `.pfr` carries hashes, not code), but the operator has to hold the game
binaries, and every newly identified release is a new verification target.

## Capacity

Gameplay runs at 32 MIPS host against a 6 MIPS guest (EMULATOR.md,
Performance) - about 5x real time with a window and audio attached, and
headless should be better. One hour of gameplay is therefore roughly twelve
minutes of one core; a 4-vCPU box verifies about 20 gameplay-hours per
wall-clock hour. For a community this size the queue is permanently empty on
the cheapest VPS available. Cap accepted session duration anyway.

If that ever stops being true, attempts can be verified in parallel: have the
client upload periodic snapshots, verify chunk *N* by re-simulating from
snapshot *N* and checking the result equals the declared snapshot *N+1*. A
forged mid-chain snapshot fails because the preceding chunk's honest
re-simulation will not match it, and chunk 0 starts from a canonical boot, so
the chain is sound. It needs snapshots to be permitted during replay
(currently refused, for good reasons) and a portable snapshot format instead
of raw struct dumps. Not worth building until the queue is actually full.

## Open questions

Still open:

- Do **Angle** and **Scrolling** affect physics, or only the view? Pinned
  either way for now; the answer decides whether either becomes a split axis
  like Resolution.
- Is `speed` truly guest-invisible during replay?
- How does the ball counter behave under **multiball**? It counts balls
  played, not balls in play, so it should be fine - but confirm.
- Does `PLAYERS` stay put for a game started from the in-table add-player
  path rather than from attract mode? `-scoredbg` samples it at the first
  launch either way, which is after both paths have written it, but this has
  only been read out of the source so far.

Answered by Spike A, all from the shipped binaries rather than from the
reconstructed source alone:

- **Player count** is `PLAYERS`, a byte in DATA, located from the ball-lost
  site and confirmed against the F1-F8 add-player handler. It is readable
  throughout, and `-scoredbg` samples it at the first launch - the last moment
  it can still change.
- **The score** is one variable per table (`SIFFRORNA`, 12 BCD digits) and one
  pair of signatures finds it in all twelve programs of the three ranked
  releases. See [Locating the score](#what-that-found).
- **Attract-mode entry** is `GO_DEMO_MODE`, unique in every ranked program,
  and its companion `GO_GAME_MODE` gives the opening boundary for free.
- **The match** re-enters play through the extra-ball route (`XXBALLE` ->
  `shoot_again`), not by rewinding the ball counter, and the score is
  untouched on the way in - so it accumulates into the same attempt, which is
  what this needs.

## Phasing

Two independent spikes gate everything. Either can be done first; both are
cheap; nothing else should start until both come back green.

- **Spike A - score and segmentation. Built; one game played, one bug found
  and fixed; wider playtest pending.** `-scoredbg` prints the per-attempt
  table the verifier would emit, and every locator it rests on was confirmed
  unique by static byte-scan across all twelve `TABLE1-4.PRG` of the three
  ranked releases before it was written.

  The first real game - Party Land, Deluxe, three balls, 7.5 emulated
  minutes - segmented cleanly end to end: one attempt opened at the game
  start, locked at `players=1`, tracked three balls plus a fourth from the
  match, and closed on `ended=attract` with a final score of 15,339,660. A
  second game opened immediately after and was correctly reported as
  unfinished when the run was stopped. Two things came out of that log: the
  spring-flag launch count was wrong (see [LOCKED](#attempt-segmentation)),
  which is what the plunger-launch hook replaced it with, and a table booting
  into attract mode was being reported as an unbalanced end. Both fixed.

  What is still unverified is the part only a human can check: that the
  number in the log is the number on the panel. So the spike is not green
  until someone plays a game on each table of each release and compares:

  ```
  pfemu.exe -d FANTASYDX -scoredbg > score.log 2>&1
  ```

  What to check, in order: the `[score] table N:` locator line appears on
  every table load; the running `score=` lines agree with the dot matrix; the
  attempt closes on `ended=attract` with the final score the panel shows, and
  not one ball early; `launches` now counts one per plunger shot (the log also
  prints `springflips`, and the two are expected to differ - that is the bug
  above, kept visible on purpose); a game that ran into the match reports
  `extra_after_last=1` or more; and a two-player game locks at `players=2` and
  says so seconds after the first launch rather than at the end of the run.
  The flag is read-only by construction, which is directly testable: replay
  the same `.pfr` with and without it and the footers must be identical.
- **Spike B - determinism.** Build the core on Linux with a stub host,
  replay one existing `.pfr` unthrottled on both platforms, compare
  `end_emu` / `end_cycles` / wav hash. Match means the rest is plumbing.

Then, in order: the headless host split and portability hardening with the
golden-vector CI; eligibility enforcement and verifier output; the service.
Chunked parallel verification only if capacity ever demands it.
