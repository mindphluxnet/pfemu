# Server-side replay verification (design note)

**Status: both spikes are green. The service is not built.** `-scoredbg`
(src/fantasies.c) locates the score and prints the per-attempt table described
below, validated on every table of every ranked release. `make` builds a
headless Linux binary that reproduces a Windows recording bit for bit - wav,
frames and footer (see [Phasing](#phasing)). Neither spike found a reason to
stop, so everything after them is plumbing: the golden-vector CI, eligibility
enforcement, and the service itself. None of that exists yet.

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

Informally corroborated, and worth writing down because it rules out the
worse possibility: every recorded session whose match *hit* has replayed as a
hit, watched on screen across several sessions. That is same-machine replay,
eyeballed rather than hashed, so it is not the gate - but if the draw read
entropy the cycle-exact injection failed to capture, it would already be
flipping there, and it is not. The open question was never whether the draw
is reproducible in principle; it is whether gcc-on-Linux and MSVC-on-Windows
agree on the cycle it lands on. So this raises the odds that a match-fires
vector passes; it does not remove the reason to have one.

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

**That last sentence was too strong, and it cost a good run.** What the
signatures find is `SIFFRORNA`, the game's *live working copy* of the score,
and the game rebuilds that buffer from the current player's saved score at
every new ball: `RESET_VARS` zeroes it, `P_STRUC_2_VARS` copies the saved
value straight back. A 500 Hz poll lands in the gap often enough to matter,
and it did - a genuine 8.8M Table 3 run was thrown out on a transient zero
that the next few guest instructions undid. The full diagnosis is in
[VERIFY-BUG.md](VERIFY-BUG.md).

Monotonicity is a property of the *logical* score, so it can only be applied
to observations that are logically meaningful. `-scoredbg` now hooks both
brackets of that rebuild, ignores the buffer in between, and compares the
restored value against the pre-clear one when the transaction closes - so a
restore that really did lose points still invalidates the attempt, and an
asynchronous glimpse of the demolition does not. A clear that is never
followed by a restore ends the window on a watchdog and says so, because
silently suspending the check forever would be the worse bug.

The rule generalises past this one buffer: **sample guest state at semantic
boundaries the guest itself defines, not on a timer.** Every value this spike
reads asynchronously is a candidate for the same mistake.

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
operand agreeing. It also validates the two brackets of the new-ball score
transaction described above: exactly one clear and exactly one restore per
program, the restore copying words on tables 1, 2 and 4 and bytes on table 3.
Those two shapes are short enough to be meaningless on their own; what makes
them safe is that both are anchored on the score address the other signatures
already agreed on.

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
- **Latent UB.** Measured, and the prediction below was aimed at the wrong
  file. UBSan on Debian/gcc 14 reported 32 sites, every one an unaligned
  guest-RAM access in `dos.c` or `bios.c` - `*(uint16_t*)&ram[a]`, where about
  half of the guest's addresses are odd - and **none in `cpu.c`**, whose hot
  path assembles bytes by hand in `cpu_ld16` and was already clean. Fixed with
  `memcpy`-based `ld16u`/`st16u` in `pfemu.h`, which compile to the same
  unaligned `mov`. What was predicted here - shift counts >= width and signed
  overflow in `cpu.c`, since x86 masks shift counts and C does not define them
  - is neither confirmed nor refuted: one vector instruments only the opcodes
  it runs.
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
makes it the most sensitive vector in the suite.

**Started: `tests/golden/`.** One vector so far - the human-played Deluxe
Table 3 session above - plus `run.sh`, which checks the footer, the wav hash
and eleven frame hashes, and passes on the headless build - under
Windows/MSVC, and on a Debian server with gcc 14.2 at `-O2`, which reproduced
the MSVC-recorded wav hash `575858f2a652fce2` and all eleven frame hashes
exactly, 1.2 billion cycles in. The cross-platform claim is tested now rather
than assumed. The vectors are
marked `-text` in `.gitattributes`, because a `.pfr` is hashed over its own
raw disk bytes and end-of-line conversion on checkout refuses it; that is not
hypothetical, the first commit of the vector was normalised on the way in and
was already broken. Still missing: a match-fires vector, per the paragraph
above, and the game files, which are not in this repository - so `run.sh`
takes an installation path, and that is the one thing between it and CI.

When something
diverges, snapshot at intervals and bisect with `tools/pfsdiff.py`, which
already diffs section by section.

That CI gate is worth having **even if the leaderboard is never built** - it
is a regression net for the emulator itself.

## The headless Linux build

**Built.** `make` produces `pfemu-headless`. It was smaller than it looked:
the emulation core - `cpu.c`, `vga.c`, `dev.c`, `bios.c`, `fantasies.c`,
`lzexe.c`, `png.c` - contained no Win32 at all, exactly as this table
predicted.

| File | Win32 surface | Work | Done |
| --- | --- | --- | --- |
| `launch.c` | the GUI launcher | Excluded entirely. No work. | yes |
| `main.c` | window, present, keyboard, timing | Split into a host layer and the emulation loop, plus a null host. The only real refactor. | `src/run.c` + `src/host_null.c` |
| `sound.c` | the DirectSound sink | Null sink. All synthesis is already platform-free. | `waveOutOpen` fails, `hwo` stays NULL - a path it already had |
| `dos.c`, `release.c`, `replay.c`, `snapshot.c`, `cfg.c` | `FindFirstFile`, `GetTempPath`, `GetFullPathName` | A small posix shim. | `src/compat.h` + `src/posix.c` |

The split is worth one note. `run.c` was *moved* out of `main.c`, not
rewritten: the loop body is byte-identical to what it replaced, checked by
diffing the old file against the two new ones line for line. Four call sites
changed shape and no others. That is not tidiness - if each host carried its
own transcription of the loop, a cross-platform mismatch would not say which
of the two was responsible.

Four things the port turned up that were not on this list, none of them
introduced by it:

- **`-keys` had done nothing since 953d83a**, which replaced the wall-clock
  keyscript with the emu-time injector, wrote `keys_parse()`, and never called
  it. The flag was accepted and the script silently discarded. It matters here
  because `-keys` is one of only two ways to drive a headless run, so a
  comparison built on it would have had both platforms agree perfectly on
  having done nothing.
- **Host paths were built with `\`** in `release.c` and `snapshot.c`. Win32
  accepts `/` everywhere, including in a `FindFirstFile` pattern; a backslash
  is an ordinary filename character off Windows, so the installation scan
  found nothing at all. Now `/` throughout.
- **`mem_w8(d+0x1E+i, ...)`** in `dos.c` is a single preprocessing number in
  C99 - the `E+` reads as an exponent. MSVC accepts it, gcc does not.
- **The text-mode font was rasterised from Consolas through GDI** at startup.
  That tied a core rendering path to a Win32 API, made text-mode output depend
  on which Consolas the host shipped, and left the headless build with no font
  at all - so a captured frame could differ between two *Windows* machines for
  a reason the emulated machine knew nothing about. It is now static CP437
  data (`src/vgafont.c`), the shapes real hardware would draw, identical on
  both builds. The tables are read in exactly one place, `vga_render()`, and
  nothing copies them into guest RAM, so this is pure output and cannot reach
  the guest.

Two specifics:

- **Case sensitivity.** DOS paths are uppercase, ext4 is not case-folding.
  The overlay and open paths need a case-insensitive resolver.

  This was the real one, and it is what Spike B's first run found. Deluxe
  opens its sound configuration as `SoUnD.cFg` while the file on disk is
  `SOUND.CFG`. DOS did not care and neither does NTFS. ext4 does: the open
  failed, the table never got its sound driver, and the run sat in text mode
  for 121 emulated seconds while the replay dutifully injected all 292 events
  into nothing. `host_casefix()` (`src/posix.c`, a no-op on Windows) resolves
  the last component against the directory when the exact name is not there -
  the last component only, because this DOS layer has no subdirectories. An
  exact match is never touched, so a newly created file keeps the name the
  guest asked for.

  Worth noting where the isolated overlay lives: under `/tmp`, i.e. real
  ext4. Running the install itself off `/mnt/c` hides the problem, because
  DrvFs is case-insensitive.
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

**Tiers 0 and 1 now exist**, in `parse_file()` rather than in a server, so
every caller gets them: bounds on `ips`, `speed`, the quality notch,
`start_table`, the footer and the event count, plus sorted events that may
not mix the cycle-stamped and legacy forms and may not be stamped past the
footer. The service still owns the policy half of tier 0 - release
allowlist, balls = 3 - because neither is the parser's business. `-strict`
covers what is: refuse a file with no integrity line, or with pre-cycle
events.

Two of those were live bugs, and both ended as a process that never exits:
an event stamped past the footer (`replay_should_stop()` returns 0 while
`ev_idx < nev`, so it never comes due and the footer stop is never
consulted) and `ips: nan` (every comparison against NaN is false, so it
walks through `run.c`'s `emu_ips <= 0.0` clamp and `emu_now()` returns NaN
forever after).

The fuzzing itself is **started, not finished**. `tests/fuzz/` holds a
22-case regression suite for the above - 14 of which the previous parser
accepted - and a deterministic mutation driver: 1,000,004 cases under
ASan+UBSan across four seeds, no crash and no assertion. That is a gcc
mutator against a corpus of one vector, not a coverage-guided campaign.
`make fuzz-clang` builds the same code as a libFuzzer target for a host
that has clang, and that is what should run for hours before the service
takes an upload from anyone.

One thing that generalises beyond this parser: **the integrity line has to
be repaired after each mutation or the fuzzer tests the checksum instead of
the code.** The first run of the harness accepted 8 cases out of 20001 and
never reached the event loop. It is also the right threat model - FNV-1a is
a checksum, not a MAC, and the client holds no key, so an attacker
recomputes it exactly the way the harness does.

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

**Both numbers in that paragraph were wrong, in opposite directions.**

It assumed an unthrottled replay, which did not exist. Every replay paced
itself to wall time: `replay.c` forces the recorded `speed` (always 1) over
`-speed`, and the loop gates on `emu_time < wall * speed`. The 200-second
vector took 200 seconds of wall clock at `host_mips=6.00`. So the real
figure was 1:1 - four gameplay-hours per wall-clock hour on a 4-vCPU box,
not twenty. `-unthrottle` (EMULATOR.md) is the switch that removes the
pacer.

And 5x was optimistic for a headless replay. Measured with
`tests/golden/speed-ab.sh` on the 200s vector:

| Host | Paced | Unthrottled | Ratio | `host_mips` |
| --- | --- | --- | --- | --- |
| Debian server (2012 Mac Mini) | 200s | 53.6s | 3.7x | 22.39 |
| WSL dev laptop | 193s | 61.6s | 3.3x | 19.51 |

So one gameplay-hour is about **16 minutes of one core**, not twelve, and a
4-vCPU box verifies roughly **15 gameplay-hours per wall-clock hour**, not
twenty. The conclusion survives - the queue is still permanently empty for a
community this size - but size from 3.7x, and re-measure on the host that
will actually run it. Note the ten-year-old Mac Mini beat the laptop: 22.39
MIPS against 19.51, which is WSL overhead rather than anything about the
silicon.

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
- ~~Is `speed` truly guest-invisible during replay?~~ **Answered: yes, on
  this vector.** `tests/golden/speed-ab.sh` replays paced and unthrottled
  and compares the artifacts. On two hosts the footer, the `-wav` hash and
  all eleven `-shotevery` frames are byte-identical between a run paced to
  wall time and one going 3.7x as fast, and both still match the hash the
  original Windows session recorded. The mechanism says why: the pacer
  decides how many batches run per outer iteration, never where one begins
  or ends, because every batch deadline - `dev_next_deadline()`,
  `replay_next_deadline()`, `until_cycles()`, the 256-instruction cap - is
  an emulated-clock quantity that cannot see `plat_time()`. Same caveat as
  every other claim here: one vector is not the opcode space.
- **Does directory enumeration order reach the guest?** `INT 21h` `AH=4Eh/4Fh`
  hands `FindFirstFile`/`FindNextFile` results straight to the program, so the
  order is guest-visible. `src/posix.c` sorts, case-insensitively, so the
  Linux side is at least deterministic and independent of the filesystem -
  but Windows returns the NTFS b-tree order and nothing sorts it, so the two
  only *happen* to agree. They did on this vector; a directory whose contents
  collate differently might not. The fix, if it ever bites, is to sort on both
  sides rather than to guess at NTFS collation.
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

- **Spike A - score and segmentation. GREEN.** Every table played end to end
  on every ranked release, the score confirmed against the game's own dot
  matrix, the multi-player rejection demonstrated on a real game, and three
  bugs found and fixed along the way. `-scoredbg` prints the
  per-attempt table the verifier would emit, and every locator it rests on
  was confirmed unique by static byte-scan across all twelve `TABLE1-4.PRG`
  of the three ranked releases before it was written.

  The first real game - Party Land, Deluxe, three balls, 7.5 emulated
  minutes - segmented cleanly end to end: one attempt opened at the game
  start, locked at `players=1`, tracked three balls plus a fourth from the
  match, and closed on `ended=attract` with a final score of 15,339,660. A
  second game opened immediately after and was correctly reported as
  unfinished when the run was stopped. Two things came out of that log: the
  spring-flag launch count was wrong (see [LOCKED](#attempt-segmentation)),
  which is what the plunger-launch hook replaced it with, and a table booting
  into attract mode was being reported as an unbalanced end. Both fixed.

  A second game, recorded this time (`sessions/FANTASYDX_20260918_062237.pfr`,
  Table 3), found the third and worst: an 8.8M run thrown out as "score
  decreased" on a transient zero inside the game's own new-ball score rebuild.
  That is the sampling-model bug written up in [VERIFY-BUG.md](VERIFY-BUG.md)
  and fixed by the transaction brackets described under
  [Locating the score](#what-that-found). Replaying that recording against the
  fix now reports the run as `[RANKABLE]` at 8,826,490, with four
  transactions opened and closed, no spurious decrease and no watchdog trip.

  That replay also settles the read-only question, in a stronger form than
  the A/B it was meant to be: the recording was made *without* `-scoredbg`,
  the replay ran *with* it, and the two agree on `278.499884s /
  1670999301 cycles` exactly. Five `cpu_step()` hooks and a 500 Hz poll cost
  the guest nothing it can observe. Only the wav hash is still uncovered,
  because neither run captured audio.

  One incidental finding from the two logs, and the reason the flag-based
  launch counter had to go: the relationship between `SPRING_VALID` and a
  launch is table-dependent. Table 3 clears the flag once per launch, about
  100 ms after it; Table 1 clears it twice. The instruction hook is 1:1 on
  both.

  The last thing none of this could check from the inside - that the number
  in the log is the number the game itself has - is now checked, on Deluxe
  Table 3. A sweep of the recording (`-shotevery 5`, 55 frames) was
  transcribed with `tools/dmdpanel.py` and compared against the log at the
  same moments:

  | Frame | Emulated time | Panel | `-scoredbg` |
  | --- | --- | --- | --- |
  | `seq020` | ~100 s, ball 3 | `1553240` | 1,553,240 (t=96.3 to 100.3) |
  | `seq053` | ~265 s, ball 4 | `8739490` | 8,739,490 (t=263.9 to 270.8) |

  Both taken inside a stretch where the score was not changing, so the
  comparison is not a race. The panel's own `PLAYER 1` / `BALL 4` agrees with
  the log's `ball=4 player=1/1` in the same frame, which checks the ball and
  player locators the same way. Two readings, on two different balls, from
  guest memory and from the pixels the player sees.

  `tools/dmdpanel.py` deliberately stops at the ASCII and leaves the digits
  to a person: the score field is right aligned, so a fixed glyph grid
  mis-slices as soon as the digit count changes, and a confident wrong
  transcription would be worse than none.

  The game also sometimes answers in writing. `SAVE_HIGHS` writes
  `TABLEn.HI` - four records of 12 unpacked BCD digits and three initials,
  the same encoding as the live buffer, in a file pfemu had no part in
  producing - and `tools/hiscore.py` decodes it. A replay writes into the
  isolated overlay, which `-keepoverlay` leaves behind instead of deleting.

  **It is an opportunistic check, not the validation path**, because it needs
  three things to line up: the score has to beat the lowest entry (5,000,000
  in a factory-fresh file), the player has to complete the initials entry,
  and - the one that actually bites - the table program has to *quit*. The
  reconstruction calls `SAVE_HIGHS` only from the quit path, and this
  install's own files agree: `FANTASYDX/PFEMU-STATE/` holds a `table1.hi` and
  no `table3.hi` at all, although the 8.8M Table 3 game was played on the
  same day. That recording ends with another game already under way, so it
  never quits the table and never writes the file. Nothing to compare.

  It is still worth having where it does apply, and it has already paid for
  itself once: a leftover overlay from an earlier session decodes to a real
  played score of 9,907,560 with the 12-digit layout the score locator
  assumes, which confirms the *encoding* from outside the instrument even
  though it confirms no particular run.

  The panel check itself is one replay. `-shot` fires at exit and `-untilemu`
  says when that is, but a single frame is a guess - the panel spends much of
  the end of a game on scrolling messages rather than digits, which is what
  an attempt at t=274 on this recording ran into. `-shotevery` covers a whole
  session instead:

  ```
  pfemu.exe -replay sessions\FANTASYDX_20260918_062237.pfr -shotevery 5 -untilemu 272 -scoredbg > sweep.log 2>&1
  python tools/dmdpanel.py seq053.ppm 205 240 190 320
  ```

  That writes `seq000.ppm`, `seq001.ppm`, ... one per five seconds of replay
  (`-shotevery` counts wall time scaled by speed, and replay pins speed to 1,
  so the index is roughly the emulated second divided by five). Pick frames
  that fall inside a long gap between `score=` lines; the region arguments
  crop to the score field, which on the 320x240 table view is the right-hand
  end of the bottom strip.

  Coverage is now complete. Every table has been played end to end and every
  ranked release has been exercised:

  | Release | Table | Result |
  | --- | --- | --- |
  | floppy | 1 | `[RANKABLE]` 15,339,660, match ball |
  | Power Pack | 2 | `[RANKABLE]` 2,554,470 |
  | Deluxe | 3 | `[RANKABLE]` 8,826,490, match ball, panel-confirmed |
  | Deluxe | 4 | `[RANKABLE]` 3,757,650, panel-confirmed |

  A two-player game on Deluxe Table 1 locked at `players=2` **16 seconds into
  the session**, at the first launch, and the run was abandoned there - the
  early abort working on a real game rather than in theory.

  The panel check was repeated on the new runs from F11 screenshots. Table 4
  is an exact hit: the panel reads `1359060` and the log has
  `score=1359060` at t=52.234. Table 2's shot reads `706870` between a logged
  506,870 and the next logged 716,890 - the end-of-ball bonus had landed
  inside the new-ball transaction, where the running lines are deliberately
  silent. `716890 = 706870 + 10020`, one Table 2 increment, so the panel value
  is the live one; `-scoredbg` now prints a checkpoint line at the restore
  bracket so that stretch is no longer a gap in the trace.

  The remaining commands, for a release or table added later:

  ```
  pfemu.exe -d FANTASYDX -scoredbg > score.log 2>&1
  ```

  What to check, in order: the `[score] table N:` locator lines appear on
  every table load; the running `score=` lines agree with the dot matrix; the
  attempt closes on `ended=attract` with the final score the panel shows, and
  not one ball early; `launches` counts one per plunger shot (`springflips`
  is printed beside it and may differ - that is the table-dependence above,
  kept visible on purpose); a game that ran into the match reports
  `extra_after_last=1` or more; and a two-player game locks at `players=2` and
  says so seconds after the first launch rather than at the end of the run.

  Two numbers in the per-attempt line are worth reading as instrument health
  rather than as results: `resets` should be one per ball started, and any
  `new-ball score clear was never followed by a restore` line means the
  transaction brackets are wrong for that build.
- **Spike B - determinism. GREEN.** The same `.pfr`, recorded on Windows,
  replayed on Windows and on Linux (gcc 13.3, `-O2 -ffp-contract=off
  -fno-strict-aliasing`):

  | | |
  | --- | --- |
  | `end_emu` / `end_cycles` | `121.707901s` / `730247408` - identical |
  | `-wav` FNV-1a | `dfb1427eef9a2239`, 2521088 samples - identical |
  | the `-wav` file itself | 5042220 bytes, byte-identical |
  | `-shotevery` frames | 13 of 13, byte-identical |
  | exit counters | 3DA reads 9240475, bit0 1891121, bit3 21262, page flips 1503, final mode 13h - all identical |

  730 million instructions of x86, a 2.5 million sample mix and thirteen
  framebuffers, bit for bit, across two compilers and two operating systems.
  The entire log diff was one line of null-host noise.

  The `-wav` hash above is also what a run with no `-shotevery` at all
  produces, on both platforms. That is a stronger statement than the table:
  the capture is an observation the run cannot feel, so a golden vector's
  frames and its audio describe the same execution rather than the one the
  measurement created. It took a wrong turn to get there - see the note under
  `-shotevery` in REPLAY.md - and it was a user watching the game window who
  caught it, not any comparison in this repo. Windows and Linux were being
  perturbed identically and agreed with each other perfectly the whole time.

  **Read the footer match carefully, though: it is the weakest of the three.**
  A replay stops on the recorded cycle, so `end_cycles` agrees *by
  construction* and `end_emu` is derived from it. The wav hash and the frames
  are the independent evidence, and they are what caught the case-sensitivity
  divergence above - the footers matched perfectly through a run that had
  never started the game. Any golden-vector suite built on footers alone would
  have passed that run.

  Two things this did not test, and should not be read as having tested:

  - **`-ffp-contract=off` is set, not exercised.** A build with
    `-ffp-contract=fast -march=x86-64-v3`, which emits 50 fused
    multiply-adds, produced the same wav and the same 13 frames. So on this
    vector FMA contraction is not observable. That makes the flag a justified
    precaution rather than a demonstrated necessity, and it means the suite
    still needs a vector that does reach the PIT and VGA phase math hard
    enough to tell.
  - **UBSan has now been run**, on the Deluxe Table 3 vector: 32 unaligned
    guest-RAM accesses in `dos.c` and `bios.c`, since fixed, after which that
    vector reports nothing. See the latent-UB bullet above - the file it named
    was not the file at fault.

  And one thing it raised that is larger than the spike. Batch structure is
  guest-observable - that is what the `-shotevery` mistake proved. Replay
  clamps every batch to the next recorded event so a key lands on the
  instruction it is due on; a recorded session has no such clamp, because the
  keys arrive from the host whenever they arrive. So **a replay's batch
  structure is not necessarily the recorded session's**, and whether a replay
  reproduces the session it recorded had never been measured. Everything here
  rests on it.

  **Measured, and it holds.** A human-played session - Deluxe, Table 3, 200
  emulated seconds, 255 events, 1.2 billion instructions - recorded on Windows
  with `-wav` so the footer carried the original run's sample hash, then
  replayed:

  | | |
  | --- | --- |
  | replayed on Windows | `wav hash MATCH: 575858f2a652fce2`, 4184064 samples |
  | replayed on Linux, headless | `wav hash MATCH: 575858f2a652fce2` |
  | record capture vs Linux replay capture | byte-identical, all 8368172 bytes |

  So the batch-structure asymmetry does not reach the guest: record does not
  clamp, replay does, and the mix comes out the same anyway. That was the last
  unmeasured assumption under the whole design, and the emulator checks it
  itself - the number in the footer was written by the session a person
  played, not by another replay.

  A scripted 40-second round-trip matched too, but it proves less than it
  looks: `-keys` clamps the batch during *record* as well
  (`keys_next_deadline()`, src/run.c), so both runs had the same batch
  structure and the case of interest never arose. The human-played vector is
  the one that settles it, which is why it is the one that got committed -
  see `tests/golden/`.

  The gate this argues for is unchanged, but its contents are now specific:
  compare the wav hash and the frame hashes, not just the footer, and include
  a session in which the end-of-game match fires.

The headless host split and portability hardening are done. Then, in order:
the golden-vector CI; eligibility enforcement and verifier output; the service.
Chunked parallel verification only if capacity ever demands it.
