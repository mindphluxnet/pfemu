# Handoff

State as of 2026-09-22, `main` at `35d1794`, pushed to `origin`.

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
| Big-endian correctness | **Untested.** The new helpers are host-endian, exactly like the puns they replaced. No regression, but no progress either |

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

3. **CI, in two halves - the first is free.** `.github/workflows/release.yml`
   already builds with MSVC on `windows-latest`, tag-triggered, so the
   infrastructure works and only has no test job.

   - *Do now:* a `ubuntu-latest` job running `make`. It needs no game files
     at all and would catch POSIX-host compile breakage on every push, which
     nothing currently does - this session's refactor touched shared headers
     and only a local WSL build stood between it and a broken Linux tree.
     Worth adding `make ubsan` as a compile check too.
   - *Needs a decision:* running the golden suite in CI at all. `run.sh`
     needs an installation, and the game files are deliberately not in this
     repository - VERIFY.md calls that "the one thing between it and CI".
     Either a self-hosted runner that already has an install, or a tiny
     synthetic guest program committed as a fixture so at least *some* vector
     runs on a stock runner.

4. **A vector that reaches the PIT and VGA phase math hard.** That is what
   would turn `-ffp-contract=off` from a precaution into a demonstrated
   necessity, or reveal it as unnecessary. Lower priority than 1-3 because
   the flag stays either way.

5. **Small, still open:** `tests/golden/run.sh` is mode `100644`, so it needs
   `sh run.sh` rather than `./run.sh`. `git update-index --chmod=+x
   tests/golden/run.sh` fixes that if wanted; it was offered and not decided.

6. **Optional, low priority:** make the new helpers explicitly little-endian
   instead of host-endian. Correct in principle, unobservable on any host we
   build for, and not something the golden vector can check - so it buys
   nothing measurable today.

## Running the gate

The headless build may be run directly - it opens no window and no audio
device. From the repo root under Git Bash:

    wsl make && wsl sh tests/golden/run.sh          # the gate, a few minutes
    cmd //c ".\build.bat"                            # MSVC; note //c, Git Bash eats /c

The ubsan run must **not** go through `run.sh`, which deletes its work
directory on exit and would take the stderr you want with it:

    wsl make ubsan
    wsl bash -c 'mkdir -p /tmp/ub && cd /tmp/ub && \
        "$OLDPWD/pfemu-headless" -d "$OLDPWD/FANTASYDX" -freezetime \
        -replay "$OLDPWD/tests/golden/deluxe-table3-200s.pfr" \
        -wav out.wav -shotevery 20 >run.log 2>&1
      grep -i "runtime error" /tmp/ub/run.log | sort -u'

`make ubsan` runs `make clean` first and overwrites `pfemu-headless` with an
`-O1` sanitizer build, so `make clean && make` afterwards to get the fast one
back.

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
