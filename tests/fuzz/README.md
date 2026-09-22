# Fuzzing the `.pfr` parser

`docs/VERIFY.md` gates the verification service on this:

> **Fuzz the `.pfr` parser before writing a line of server code.** It is C
> eating attacker-controlled text, and it - not the emulator - is the real
> attack surface.

A `.pfr` is plain text parsed by `parse_file()` in `src/replay.c`. The moment
one can be uploaded rather than recorded locally, every number in it is
hostile input that the emulator will then act on: it sets the clock, sizes an
allocation, and decides when the run stops.

## Running it

```sh
make fuzz                                  # gcc, ASan + UBSan
./pfemu-fuzz-pfr -selftest                 # the regression suite, instant
./pfemu-fuzz-pfr -n 300000 tests/golden/deluxe-table3-200s.pfr
```

`-selftest` is the one to run in CI. It needs no corpus, finishes in
milliseconds, and exits non-zero if any case gets the wrong verdict.

The mutation driver takes seed files, `-n` iterations and `-s` for the PRNG
seed. It is deterministic: the same `-s` replays the same run exactly, so a
finding can be handed to someone else as a number. `PFR_FUZZ_TMP` moves the
scratch file (point it at a tmpfs to save your disk).

With a clang available, `make fuzz-clang` builds the same code as a libFuzzer
target, which is coverage-guided and reaches much further. The gcc driver
exists because nothing else in this repository needs clang.

## Two things are being tested, and the second is the interesting one

**Memory safety.** ASan and UBSan, with `-fno-sanitize-recover=all` so a
finding is an exit status rather than a note in the log.

**That an accepted file cannot hang the run loop.** This is the part a
sanitizer cannot see. The bug that prompted all of this parsed perfectly and
then ran forever: `replay_should_stop()` returns 0 while `ev_idx < nev`, so an
event stamped past the footer never comes due, the list never empties, and the
footer stop is never consulted. At 6 MIPS a cycle stamp of 2^63 is about 48
million years. Every accepted file is therefore asserted against the
invariants `validate_events()` is supposed to guarantee.

## The integrity line has to be repaired after mutating

This is the thing to understand before trusting a number out of this harness.

A `.pfr` carries an FNV-1a over its own bytes and `parse_file()` refuses a file
whose hash does not match. So a blind mutator is rejected at the door: the
first run of this harness accepted **8 cases out of 20001** and never reached
the event loop at all. After `fixup_hash()` recomputes the line, the same
corpus accepts around 1.8% - a couple of hundred times more work actually
reaching the parser.

Campaign so far: 1,000,004 cases across four seeds, ~18,000 of them accepted
deep into the parser, no crash and no assertion. Worth being clear about what
that is and is not - a blind mutator against a corpus of one vector
rediscovers shallow structure and stops. `make fuzz-clang` on a host with
clang is the real campaign.

It is also the correct threat model. FNV-1a is a checksum, not a MAC -
VERIFY.md says exactly that - and the client holds no key, so an attacker
edits the file and recomputes the hash, which is precisely what `fixup_hash()`
does. Fuzzing without it measures the checksum, not the parser.

Roughly one case in ten is deliberately left unrepaired, so the rejection path
stays covered too.

**If you change the harness, watch the acceptance rate.** The driver fails the
run outright if under 1% of cases are accepted, because a run that parses
nothing proves nothing while still printing a clean summary.

## What `-selftest` covers

22 cases, each a complete file with an expected verdict. Fourteen of them are
accepted by the parser as it stood before the validation pass - that was
checked by building this same suite against the previous `src/replay.c`, which
is the only reason to believe the suite has teeth:

| Case | Why |
| --- | --- |
| event past the footer | the hang above |
| `ips: nan` / `inf` / `0` | NaN defeats `run.c`'s `emu_ips <= 0.0` clamp, because every comparison against NaN is false; `emu_now()` then returns NaN and every deadline test is false forever |
| `speed: 0` | `run.c` divides by it |
| `quality: 260` | truncates through `(uint8_t)` to 4 and is applied |
| `start_table: 99` | out of range; harmless downstream, but not something this program writes |
| `end_cycles: 2^64-1`, `end_emu: 1e308`, negative `end_emu` | an unbounded run |
| events out of order | `replay_inject_due()` walks the list once and never looks back |
| mixed cycle-stamped and legacy events | two clocks in one file |
| bad magic, no release, no footer, empty, magic only | pre-existing refusals, here so an edit cannot quietly drop them |
| legacy events / no integrity line | accepted by default, refused under `-strict` |

## What it does not cover

The parser only. Nothing here runs the emulator, so this says nothing about
what a *valid* file does once simulation starts - that is what
`tests/golden/run.sh` is for.

It also says nothing about whether a well-formed file is an *honest* one.
VERIFY.md is explicit that it cannot be: a `.pfr` is plain text, FNV-1a is a
checksum, and an input stream authored by hand or by a solver will verify
perfectly. That is the shape of the problem, not a defect to engineer around.
