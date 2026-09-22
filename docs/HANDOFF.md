# Handoff

State as of 2026-09-22, `main` at `26b7a25` plus the verdict work below.

Read this with the determinism section of `VERIFY.md`, which is the document
this work serves. This file is the short version plus what to do next.

## Where things stand

The cross-platform determinism claim is **tested rather than assumed**, for the
first time. A Debian server (gcc 14.2, x86-64, `-O2`) replayed
`tests/golden/deluxe-table3-200s.pfr` and reproduced the wav hash
`575858f2a652fce2` recorded by a human playing on Windows/MSVC, along with all
eleven frame hashes and `200.167547s / 1201005284 cycles`. Byte for byte, 1.2
billion cycles in, across two compilers and two operating systems.

`make ubsan` has also been run, once, on that vector. It reported 32 unaligned
guest-RAM accesses - all in `dos.c` and `bios.c`, all of them a wider pointer
punned at `&ram[a]`, where about half of the guest's addresses are odd. Those
are fixed (`ld16u`/`st16u`/`ld32u`/`st32u` in `pfemu.h`, `BDA16_SET` in
`bios.c`) and that vector now reports nothing.

Note what ubsan did *not* find: anything in `cpu.c`. Both the Makefile and
VERIFY.md predicted the latent UB would be there - shift counts >= width,
signed overflow. `cpu_ld16` assembles bytes by hand for speed, which
incidentally made the hot path the one place already free of this. Both
documents now say what was actually found.

## Verified, and not

| Claim | Status |
| --- | --- |
| MSVC and gcc 14.2 agree byte-for-byte | **Verified**, on one vector |
| The unaligned accesses are gone | **Verified** - 0 ubsan reports, and the vector is still byte-identical |
| `-ffp-contract=off` is *necessary* | **Not demonstrated.** VERIFY.md records `-ffp-contract=fast -march=x86-64-v3` emitting 50 FMAs and producing the same wav and frames. A justified precaution, not a proven need |
| Shift counts >= width in `cpu.c` | **Looked for properly, not found.** `tests/golden/ubsan.sh` on both vectors - two table programs, one a complete game - reports nothing at all. ubsan instruments what runs, so this covers the code these sessions execute, not the opcode space. `TABLE2`, `TABLE4`, the intro and three other releases are untouched |
| Determinism across optimisation levels | **Verified**, incidentally. Both vectors reproduce their capture hash under `-O1` + ubsan against `-O2` without. Two compilers, two operating systems, two optimisation levels |
| The suite catches a wrong **score** | **Verified** as a mechanism, on one vector. `deluxe-table1-partyon-295s` pins `[RANKABLE] 20,652,570`, and `run.sh` fails the vector if a replay disagrees or if the ball-counter watchdog fires |
| Two platforms agree on a **score** | **Verified**, on one vector. `deluxe-table1-partyon-295s` was recorded on Windows/MSVC and replayed on Debian/gcc 14.2 at `-O2`: same capture hash, all 15 frames, same 1,773,389,028 cycles, same 20,652,570. This is the claim the whole service rests on |
| Host pacing is guest-invisible | **Verified**, on one vector, on two hosts. `tests/golden/speed-ab.sh` replays paced and unthrottled: footer, wav hash and all 11 frames byte-identical, and both still match the original Windows session |
| A replay can run faster than real time | **Verified** - 3.7x on the server, 3.3x under WSL. It could not before `-unthrottle`: `-speed` is discarded during replay by design |
| The `.pfr` parser refuses hostile input | **Verified** for the cases in `tests/fuzz` - 22 of them, 14 of which the previous parser accepted. `-selftest` is the regression test |
| The `.pfr` parser is memory-safe | **No finding**, which is weaker than verified. 1,000,004 mutation cases under ASan+UBSan across four seeds, ~1.8% of them accepted deep into the parser. No crash, no assertion. A gcc mutation driver is still not a coverage-guided campaign |
| The verdict object is stable | **Verified** for its own logic. `tests/verify/` drives 17 cases through `src/verify.c` against stubs under ASan+UBSan: field names, the three statuses, the exit codes, the eligibility rule, JSON escaping, write-once. Every emitted object was also parsed with a real JSON parser |
| The verdict matches the report | **Verified** on the laptop (WSL/gcc), on the vector with a score. The JSON `best` and the `[RANKABLE]` line printed by the same replay both say 20,652,570, and the status is `verified`. This is the half the stub tests cannot see: the accessors feed the emitter the right numbers in a real run. The Mac Mini result is still pending |
| The golden vector pins **rankability** | **Verified.** `deluxe-table1-partyon-295s.expected` was regenerated and now ends `attract yes`. The diff was that one token plus the column comment. Header, hashes and all 15 frames came out unchanged, which is a good check on its own. Until now the file had seven tokens, and `run.sh` quietly cut its own side down to match |
| Big-endian correctness | **Untested.** The new helpers are host-endian, exactly like the puns they replaced. No regression, but no progress either |

The verifier's **output** is done too, which was the last thing between
here and writing the service. `-verify FILE` emits one JSON object -
status, the recorded-vs-actual comparison, every scored attempt, and
`best`, the highest score that satisfies the eligibility rule - and sets
the exit code from the verdict. The rule is enforced inside the emulator
rather than left to each caller, which is the whole reason the field is
there. See the verdict section of VERIFY.md.

The parser hardening that used to head this list is done (`8da135b`).
What it settled: the specific holes are closed and regression-tested, and
`-strict` exists for a verifier that should refuse what a player may keep.
What it did not settle: memory safety is "no finding in 1M cases under a
hand-rolled mutator", which is not the same thing as fuzzed - a blind
mutator rediscovers shallow structure and stops.
A real campaign wants `make fuzz-clang` on a box with clang, left running
for hours against a corpus of more than one vector - which is another
reason item 1 below matters.

## What to do next, in order

1. **More vectors, on the tables and releases nothing covers.** The single
   lever that moves several things at once: ubsan coverage (`TABLE2`,
   `TABLE4`, the intro and the floppy/A releases are all unexercised), the
   fuzz corpus, and the cross-platform score evidence, which currently rests
   on one complete game. Recording one is cheap now - the capture hash is
   automatic, so no flag has to be remembered, and `mkexpected.sh` does the
   rest.

   **A match-fires vector is the one to grab if it turns up**, and it is no
   longer worth hunting for on its own. VERIFY.md wanted it because the
   end-of-game draw is cycle-derived and amplifies a one-cycle divergence
   into a different final score. The scoreless-ball return in
   `deluxe-table1-partyon-295s` now covers that shape of test - an exact
   equality on the score that flips the rest of the game - and can be
   produced on demand rather than waiting on a 1-in-10 draw. So: play, keep
   what lands, and if a match fires in one of them, that vector is the
   valuable one.

2. **CI: the free half is done, the other half needs a decision.**
   `.github/workflows/ci.yml` runs on every push on `ubuntu-latest`:
   `make`, the 22-case parser regression suite, a 50k-case fuzz run over
   the committed vector, the 17-case verdict suite, and `make ubsan` as a
   compile check. None of it needs an installation.

   *Still open:* running the golden suite in CI at all. `run.sh` needs an
   installation, and the game files are deliberately not in this
   repository - VERIFY.md calls that "the one thing between it and CI".
   Either a self-hosted runner that already has an install, or a tiny
   synthetic guest program committed as a fixture so at least *some*
   vector runs on a stock runner. Same question applies to
   `speed-ab.sh`.

   *Also open, smaller:* there is still no Windows job on push. MSVC is
   the primary build and `release.yml` only exercises it on a tag, so a
   change made on the POSIX side can sit broken until release time -
   the mirror image of the risk the Linux job just closed.

3. **A vector that reaches the PIT and VGA phase math hard.** That is what
   would turn `-ffp-contract=off` from a precaution into a demonstrated
   necessity, or reveal it as unnecessary. Lower priority than 1-2 because
   the flag stays either way.

4. **Optional, low priority:** make the new helpers explicitly little-endian
   instead of host-endian. Correct in principle, unobservable on any host we
   build for, and not something the golden vector can check - so it buys
   nothing measurable today.

## Running the gate

The headless build may be run directly - it opens no window and no audio
device. From the repo root under Git Bash:

    wsl make && wsl sh tests/golden/run.sh          # the gate, a few minutes
    wsl sh tests/golden/speed-ab.sh                 # pacing A/B, ~4 minutes
    wsl make fuzz && wsl ./pfemu-fuzz-pfr -selftest # parser, milliseconds
    wsl make verify-test && wsl ./pfemu-verify-test # verdict, milliseconds
    cmd //c ".\build.bat"                            # MSVC; note //c, Git Bash eats /c

The ubsan pass has its own script, because it must **not** go through
`run.sh`: that deletes its work directory on exit and would take the stderr
you want with it.

    wsl sh tests/golden/ubsan.sh                    # builds, runs every vector

It replays every `.pfr` in `tests/golden/` into `/tmp/pfemu-ubsan/<vector>/`,
keeps the logs, and prints the unique `runtime error` lines across all of
them. It also says whether each vector still reproduced its capture hash
under `-O1` + instrumentation, which is a small extra piece of evidence on
top of the bug hunt.

This used to be a multi-line snippet to paste. Do not go back to that: in
`cmd.exe` it is split line by line, the first fragment is an unterminated
bash command, and the result is that nothing happens and nothing explains
why. The snippet also carried the repo root in `$OLDPWD`, which a loop's own
`cd` overwrites after the first vector.

`make ubsan` builds `pfemu-headless-ubsan` and deletes its object files on the
way out, so the two configurations no longer contaminate each other and no
follow-up `make clean` is needed.

## Traps, all of them paid for once already

- **Never delete `<install>/PFEMU-STATE/`.** It holds `SOUND.CFG`; without it
  the guest prints a DOS error and terminates about 1.3 emulated seconds in,
  which then surfaces as some confusing downstream failure.
- **Always pass `-freezetime`** for any run compared byte-for-byte.
- **`tests/golden/*.pfr` are `-text` in `.gitattributes`.** A `.pfr` is hashed
  over its own raw disk bytes, so end-of-line conversion on checkout refuses
  the file. The first commit of the vector was normalised on the way in and
  was already broken.
- **Line endings are mixed in this repo** and `core.autocrlf` is `true`, and
  not along the line you would guess. The C sources are CRLF. `Makefile`,
  `docs/VERIFY.md`, `docs/REPLAY.md` and this file are LF. `README.md` and
  `docs/EMULATOR.md` are **CRLF** - so "the docs are LF" is wrong, and a
  patch script written on that assumption fails to match anything, which is
  the good outcome. Check each file with `file` before editing it; a
  scripted edit that gets this wrong turns the whole file into the diff.
- **Running the golden suite needs the game files.** `run.sh` takes an
  installation path, or reads `PFEMU_INSTALL`.
- **A wall of undefined `__ubsan_handle_*` at link time is a stale-object
  problem, not a source problem.** Instrumented `.o` files relinked without
  `-fsanitize=undefined` produce it, and it reads like the code is broken when
  it is not. `make clean && make` clears it. The Makefile now keeps the two
  builds apart so it should not recur; if it does, check
  `nm -u src/run.o | grep ubsan` before suspecting the source.
