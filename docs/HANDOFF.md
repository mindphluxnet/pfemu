# Handoff

State as of 2026-09-22, `main` at `3f9b265` plus this file.

Read this with `VERIFY.md`, which is the document this work serves. This file
is the short version plus what to do next.

## Where things stand

**The leaderboard is three repositories.** They sit side by side in
`source/`, and each has its own handoff:

- **pfemu** (this one): the emulator, and `-verify`.
- **pfemu-service**: the validator. It runs on the Mac Mini, is reachable
  only from the web server, and turns a `.pfr` into facts.
- **pfemu-web**: accounts, launcher uploads, the kept recordings, the
  ranking policy and the boards. It runs on another server in the LAN and
  is the only public part.

This repository meets the other two only at the `-verify` object and the
exit code, and the validator pins which pfemu build it runs. This file
covers the emulator side.

The work done before it is finished:

- **Determinism holds.** Two compilers (MSVC, gcc 14.2), two operating
  systems (Windows, Debian), two optimisation levels (`-O2`, `-O1` + ubsan).
  Both golden vectors reproduce a human-played Windows session byte for byte:
  capture hash, every frame and the footer cycle count.
- **The score holds across platforms.** `deluxe-table1-partyon-295s`, a
  complete Party Land game recorded on Windows, replays on Debian to the same
  20,652,570, and the suite pins it.
- **Eligibility is decided and enforced.** An attempt counts only if it ends
  in attract mode on its own, and it must meet the other five conditions in
  `sc_close()` as well. A run that stops mid-table does not count. The vector
  pins `rankable`, not just the number.
- **The verifier's output exists.** `-verify FILE` writes one JSON object:
  a status (`verified` / `mismatch` / `refused`), the recorded-vs-actual
  comparison, every attempt with a reason token, and `best`, the highest
  *rankable* score. It sets the exit code to 0 / 2 / 1 to match. This is
  what the service reads. Nothing else the emulator prints is an interface.
  See the verdict section of VERIFY.md. Since `c24cf6a` it also carries
  `build` (`git describe --dirty` at compile time, via a generated
  `src/build.h`), so each stored verdict names the binary that produced it.
- **The input side is hardened.** The `.pfr` parser bounds every field,
  refuses events stamped past the footer (that used to be an endless run),
  and `-strict` refuses what a verifier should not accept. 22 regression cases
  pass, and 1M mutation cases produced no finding.
- **Replays run 3.3-3.7x real time** with `-unthrottle`, and pacing is
  proven guest-invisible on two hosts.
- **ubsan is clean** on both vectors. It found 32 unaligned guest-RAM
  accesses in `dos.c`/`bios.c` on the first pass, all fixed since. It found
  nothing in `cpu.c`, where the shift-count UB was predicted.

All of it has a test, and CI runs on every push the tests that do not need
game files: build, parser suite, 50k fuzz cases, verdict suite, ubsan
compile.

## Verified, and not

| Claim | Status |
| --- | --- |
| MSVC and gcc 14.2 agree byte-for-byte | **Verified**, on both vectors |
| Two platforms agree on a **score** | **Verified**, on one vector. `deluxe-table1-partyon-295s`: recorded on Windows/MSVC, replayed on Debian/gcc 14.2, same capture hash, all 15 frames, same 1,773,389,028 cycles, same 20,652,570. This is the claim the whole service rests on |
| The suite catches a wrong score **and a wrong eligibility verdict** | **Verified**, on one vector. The attempt line pins `20652570 ... attract yes`. `run.sh` fails the vector if a replay disagrees, or if the ball-counter watchdog fires |
| The verdict object is stable | **Verified** for its own logic. `tests/verify/` has 17 cases against stubs under ASan+UBSan. Every emitted object was also parsed with a real JSON parser, and two deliberate mutations of `verify.c` were caught |
| The verdict matches the report | **Verified** on both hosts. `run.sh` cross-checks the JSON `best` against the `[RANKABLE]` lines the same replay printed: 20,652,570 on both |
| Determinism across optimisation levels | **Verified**, incidentally. Both vectors reproduce their capture hash under `-O1` + ubsan |
| Host pacing is guest-invisible | **Verified**, on one vector, on two hosts (`tests/golden/speed-ab.sh`) |
| A replay can run faster than real time | **Verified**: 3.7x on the server, 3.3x under WSL |
| The `.pfr` parser refuses hostile input | **Verified** for the 22 cases in `tests/fuzz`, 14 of which the previous parser accepted |
| The `.pfr` parser is memory-safe | **No finding**, which is weaker than verified. 1,000,004 mutation cases under ASan+UBSan, no crash. A hand-rolled gcc mutator is not a coverage-guided campaign |
| Shift counts >= width in `cpu.c` | **Looked for properly, not found**, in the code the two vectors execute. `TABLE2`, `TABLE4`, the intro and three other releases are unexercised |
| `-strict -unthrottle -verify` together | **Verified** through the service, on the Mac Mini under bwrap: the Party Land vector came back `verified` and `rankable` at 20,652,570, build `3f9b265d674c` from a clean clone, 78.2 s wall for 295.6 s emulated (3.8x). The install was mounted read-only, so the replay needs no write access to it |
| `-ffp-contract=off` is *necessary* | **Not demonstrated.** A justified precaution: `-ffp-contract=fast` emitted 50 FMAs and changed nothing |
| Big-endian correctness | **Untested.** The memory helpers are host-endian, like the puns they replaced |
| A replay is independent of the verifier's `PFEMU-STATE/` | **False, measured 2026-09-23.** Party Land vector, Deluxe: with `table1.hi` as recorded it verifies at 20,652,570. With every entry at 99,999,999 it mismatches, and the attempt ends at 175.3 s with 17,128,800. With the file missing it mismatches at 7,125,000. Same inputs, a different game. `replay.c` only warns when the overlay hash differs. **Not the name entry at game end:** with the high table the `-scoredbg` trace is identical up to t=67 s, and the first difference is a score event 1 ms early at t=72.58 s, on ball 1 at 4.55M, far from any table entry. With the file missing it diverges at t=6.6 s. The table reaches the game *during* play. A guess, not checked in the game code: a compare against the table whose cycle cost depends on its contents |

## What to do next, in order

0. **Make a ranked recording independent of the player's local state.
   This blocks the service from going live.** The replay sees the
   operator's `PFEMU-STATE/`, the recording saw the player's, and the high
   scores alone change the game (table above). Two ways out, and the
   choice is a design decision, not yet taken:
   - **A canonical state for ranked play.** Record with an isolated,
     factory-fresh overlay: default high scores, a fixed `SOUND.CFG`, the
     recorded options patched in, as replay already does. The validator
     replays against the same state and refuses any `.pfr` whose
     `overlay:` hash is not the canonical one, which a header check can
     do. It is simple and cannot be manipulated, but the player's own
     high-score table does not show during a ranked game.
   - **Carry the state in the `.pfr`.** Embed the small files the guest
     reads (`*.hi`, `*.CFG`), and have the replay rebuild the overlay from
     them. The player keeps their own table, at the price of a format
     change, a larger parser surface, and a hi-score table the player
     chooses.

1. **The service** lives in `pfemu-service` and `pfemu-web`; their HANDOFFs
   have the order. From this repository they need two things:
   - **Leave the `-verify` object alone.** It is an interface: add fields,
     never rename or remove them, and bump `pfemu_verify` if that ever has
     to happen. Push before the Mac Mini builds, because it pins a commit
     that has to exist on `origin`.
   - **The launcher upload**, once both services are deployed: log in or
     register, keep the token, upload a recording, poll it, and show the
     result. This is C in `launch.c` over WinHTTP. Its contract is
     `pfemu-web/docs/API.md`, which also has a table of what to tell the
     player for each `reason`.

2. **A real fuzzing campaign, before the service takes uploads from
   strangers.** This is the only item on the list with a deadline attached.
   `make fuzz-clang` on a host with clang, left running for hours, against a
   corpus of more than one vector. The current "no finding" comes from a blind
   mutator, and that kind of mutator rediscovers shallow structure and stops.
   It does not need to block building the service, only opening it.

3. **More vectors, on the tables and releases nothing covers.** This moves
   several things at once: ubsan coverage (`TABLE2`, `TABLE4`, the intro, the
   floppy/A releases), the fuzz corpus for item 2, and the cross-platform
   score evidence, which rests on one complete game. Recording one is cheap:
   the capture hash is automatic, and `mkexpected.sh` does the rest.
   **A match-fires vector is the one to keep if it turns up**, but it is no
   longer worth hunting for. The PARTY ON return in the Party Land vector
   already covers that shape of test: an exact equality on the score that
   changes the rest of the game.

4. **CI: the golden suite needs a decision.** Everything that runs without
   game files already runs on every push. `run.sh` and `speed-ab.sh` need an
   installation, which is deliberately not in the repository. The options
   are a self-hosted runner that already has an install, or a small
   synthetic guest program committed as a fixture. *Smaller:* there is no
   Windows job on push, so a POSIX-side change can break the MSVC build
   unnoticed until a release tag.

5. **A vector that reaches the PIT and VGA phase math hard**, to turn
   `-ffp-contract=off` from a precaution into a demonstrated necessity, or
   to show it is unneeded. The flag stays either way.

6. **Optional:** make the memory helpers explicitly little-endian. Correct
   in principle, but unobservable on any host we build for.

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
- **A remote checkout is only as new as `origin`.** The Mac Mini once ran
  the gate green on a stale checkout, because the commits under test had
  not been pushed. If a vector with a score shows no `verdict` line, the
  checkout is out of date, and the run is a pass for an older commit, not
  this one. Push, pull, `make`, then run.
- **An `.expected` is generated, never typed.** `mkexpected.sh` refuses to
  overwrite a differing file and leaves `.expected.new` beside it, because
  an expected value that changed on its own is a finding. Read the diff
  and move the file into place by hand only when the change is intended.
- **A WSL build of this checkout says `-dirty`.** `core.autocrlf` is
  `true` on the Windows side, so WSL-git sees every CRLF file as modified,
  and `src/build.h` gets `<hash>-dirty` even on a clean tree. The service
  withholds ranking from a dirty build, correctly. A clean Linux clone,
  which is what the Mac Mini builds from, does not have this problem.
- **Write patch scripts to files, not heredocs**, when an agent edits this
  repo through a shell tool. A `\n` inside a heredoc came out as a literal
  newline or a bare `n` more than once. That produced a broken C string
  once, and once a shell line that `sh -n` accepted but that would have
  handed `n` to grep as a filename.
