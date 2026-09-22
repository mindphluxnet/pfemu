# Handoff

State as of 2026-09-22, `main` at `7194e47`.

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
| Shift counts >= width in `cpu.c` | **Unproven either way.** ubsan instruments only the opcodes a vector executes, and one 200s Table 3 replay does not cover the opcode space |
| Host pacing is guest-invisible | **Verified**, on one vector, on two hosts. `tests/golden/speed-ab.sh` replays paced and unthrottled: footer, wav hash and all 11 frames byte-identical, and both still match the original Windows session |
| A replay can run faster than real time | **Verified** - 3.7x on the server, 3.3x under WSL. It could not before `-unthrottle`: `-speed` is discarded during replay by design |
| The `.pfr` parser refuses hostile input | **Verified** for the cases in `tests/fuzz` - 22 of them, 14 of which the previous parser accepted. `-selftest` is the regression test |
| The `.pfr` parser is memory-safe | **No finding**, which is weaker than verified. 1,000,004 mutation cases under ASan+UBSan across four seeds, ~1.8% of them accepted deep into the parser. No crash, no assertion. A gcc mutation driver is still not a coverage-guided campaign |
| Big-endian correctness | **Untested.** The new helpers are host-endian, exactly like the puns they replaced. No regression, but no progress either |

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

1. **Add a match-fires vector.** VERIFY.md has wanted this since before the
   suite existed, and it is now the single highest-value thing left: the
   end-of-game match draw is cycle-derived, so it amplifies a one-cycle
   divergence into a different final score. It is the most sensitive vector
   the suite can hold, and the suite currently has exactly one vector, which
   is not one.

2. **Re-run `make ubsan` once more vectors exist.** This is what settles the
   shift-count question, and it is nearly free once step 1 is done. A second
   vector exercising different opcodes is the only way to find out whether
   the original prediction was wrong or merely unexercised.

3. **CI: the free half is done, the other half needs a decision.**
   `.github/workflows/ci.yml` runs on every push on `ubuntu-latest`:
   `make`, the 22-case parser regression suite, a 50k-case fuzz run over
   the committed vector, and `make ubsan` as a compile check. None of it
   needs an installation.

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

4. **A vector that reaches the PIT and VGA phase math hard.** That is what
   would turn `-ffp-contract=off` from a precaution into a demonstrated
   necessity, or reveal it as unnecessary. Lower priority than 1-3 because
   the flag stays either way.

5. **Optional, low priority:** make the new helpers explicitly little-endian
   instead of host-endian. Correct in principle, unobservable on any host we
   build for, and not something the golden vector can check - so it buys
   nothing measurable today.

## Running the gate

The headless build may be run directly - it opens no window and no audio
device. From the repo root under Git Bash:

    wsl make && wsl sh tests/golden/run.sh          # the gate, a few minutes
    wsl sh tests/golden/speed-ab.sh                 # pacing A/B, ~4 minutes
    wsl make fuzz && wsl ./pfemu-fuzz-pfr -selftest # parser, milliseconds
    cmd //c ".\build.bat"                            # MSVC; note //c, Git Bash eats /c

The ubsan run must **not** go through `run.sh`, which deletes its work
directory on exit and would take the stderr you want with it:

    wsl make ubsan
    wsl bash -c 'mkdir -p /tmp/ub && cd /tmp/ub && \
        "$OLDPWD/pfemu-headless-ubsan" -d "$OLDPWD/FANTASYDX" -freezetime \
        -replay "$OLDPWD/tests/golden/deluxe-table3-200s.pfr" \
        -wav out.wav -shotevery 20 >run.log 2>&1
      grep -i "runtime error" /tmp/ub/run.log | sort -u'

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
- **Line endings are mixed in this repo** and `core.autocrlf` is `true`: the C
  sources are CRLF in the working tree, `Makefile` and `docs/*.md` are LF.
  Scripted edits must preserve each file's own endings or the diff becomes the
  whole file.
- **Running the golden suite needs the game files.** `run.sh` takes an
  installation path, or reads `PFEMU_INSTALL`.
- **A wall of undefined `__ubsan_handle_*` at link time is a stale-object
  problem, not a source problem.** Instrumented `.o` files relinked without
  `-fsanitize=undefined` produce it, and it reads like the code is broken when
  it is not. `make clean && make` clears it. The Makefile now keeps the two
  builds apart so it should not recur; if it does, check
  `nm -u src/run.o | grep ubsan` before suspecting the source.
