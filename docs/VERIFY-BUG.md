# False score decrease during new-ball reset

> **Status: fixed in code, not yet confirmed against the recording.**
> `-scoredbg` now hooks both brackets of the clear/restore sequence and
> ignores the live buffer in between, as recommended below. The locators pass
> `tools/scorescan.py` on all twelve ranked programs. Validation plan item 1 -
> replaying `sessions/FANTASYDX_20260918_062237.pfr` and getting a rankable
> 8,826,490 - has not been run yet. See the implementation notes at the end.

## Conclusion

The run did not lose its score. The verifier is applying its monotonicity
check to `SIFFRORNA`, the game's **live working copy** of the score, as if that
buffer were continuously stable. On Table 3 (Billion Dollar Game Show), a
new-ball reset deliberately clears that buffer and then restores it from the
current player's saved score. `fantasies_score_tick()` polls at about 500 Hz,
so it can run in the small interval between the clear and the restore.

That is what happened in this recording. The score locator is correct; the
assumption that every asynchronous read of the located buffer is a logical
score observation is not.

## Evidence from this recording

The relevant sequence in `score.log` is:

```text
t=212.700 ball 3 -> 4 (of 3)
t=221.376 score went DOWN, 7612990 -> 0
t=223.686 launch 4 (ball 4) <- past the last ball
t=224.373 ball=4 ... score=7622990
...
attempt 1 ... score=8826490 ... ended=attract [not rankable: score decreased]
```

This is a match-awarded extra ball. The score was 7,612,990 before the
temporary zero, scoring continued from that value on ball 4, and the clean
attract-mode sample was 8,826,490. The replay contains no input around the
zero; the next events are plunger input at 223.109-223.678 seconds. This rules
out a user action or a new game at 221.376 seconds.

The `.pfr` identifies Deluxe `TABLE3.PRG` by SHA-256
`413e6c70d6a22629f903eaec6c1af10366b68beaedd93a4b6df489044359e0fd`.
The installed file has that exact hash. Static inspection of that exact binary
finds all of the relevant operations, using the located score offset
`DS:3EDE`:

| File offset | Operation |
| --- | --- |
| `0x41934` | per-ball reset: `XOR AX,AX ... MOV DI,3EDE ... REP STOSW` |
| `0x42524` | saved player score -> live `SIFFRORNA` |
| `0x425C3` | live `SIFFRORNA` -> saved player score |
| `0x42349` | the separate final `zeroscore` routine |

The two clearing sites matter. `0x41934` is the temporary reset involved in
this bug. `0x42349` is the intentional clear after returning to attract mode,
which the existing end hook already samples before it executes.

## Source-level explanation

The published DOS source maps table 3 to `SHOW.ASM`
([FANTASIE.ASM lines 90-100](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/FANTASIE.ASM#L90-L100)).
Although this source tree is historical evidence rather than a promise that
every shipped binary is byte-identical, the exact Deluxe binary patterns above
confirm that this behavior is present in the recorded release.

The important sequence is:

1. At the end of the last normal ball, `NO_MORE_BALLS` copies the live score
   into `PLAYER_AREA[player].P_SIFFRORNA`
   ([SHOW.ASM lines 2641-2646](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/SHOW.ASM#L2641-L2646)).
2. The match animation reads the last score digit from the saved per-player
   structure, not from the live buffer
   ([SHOW.ASM lines 2344-2377](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/SHOW.ASM#L2344-L2377)).
3. When the match awards another ball, `WHEN_NEW_BALL_RESET_TABLE` calls
   `RESET_VARS` and then `P_STRUC_2_VARS`
   ([SHOW.ASM lines 1867-1875](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/SHOW.ASM#L1867-L1875)).
4. `RESET_VARS` zeroes `SIFFRORNA`
   ([SHOW.ASM lines 1777-1785](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/SHOW.ASM#L1777-L1785)).
5. `P_STRUC_2_VARS` immediately restores `SIFFRORNA` from the saved player
   structure
   ([SHOW.ASM lines 2801-2811](https://github.com/historicalsource/pinballfantasies/blob/aa2dd368d73886bbd666507bd7341001060700d9/SHOW.ASM#L2801-L2811)).

Tables 2 and 4 use the same clear-then-restore design for normal new-ball
setup (`SDEV.ASM` and `STONES.ASM`). Table 1 organizes its resets differently,
but this should be fixed as a score-sampling model problem rather than as a
Table 3 special case.

## Why the current code rejects it

`sc_read_score()` correctly decodes the 12 bytes at `SIFFRORNA`.
`fantasies_score_tick()` then reads that buffer asynchronously and immediately
sets `sc_cur.decreased` when one poll is lower than the previous poll. There is
no concept of a score update/reset transaction. A single zero observed between
the two guest copy operations therefore permanently makes the attempt
unrankable, even though the next guest instructions restore the same logical
score.

The statement in `docs/VERIFY.md` that a decrease necessarily means a wrong
address (or worse) is therefore too strong. It is true only for stable logical
score observations, not arbitrary instruction-level observations of the
working buffer.

The final-score boundary remains sound: `GO_DEMO_MODE` is called before the
final `zeroscore`, and the hook reads the intact score there. The recording's
8,826,490 terminal sample is consistent with that ordering.

## Recommended fix

Treat the live-buffer clear and restore as one atomic guest operation for
verification purposes. Preserve the existing final sample at the
`GO_DEMO_MODE` hook.

The strongest small change is to locate and hook the two sides of the
clear/restore sequence:

- Enter an `unstable_score` state when the per-ball reset starts clearing the
  located `SIFFRORNA`.
- Do not update `sc_last_score`, emit a decrease, or set `decreased` while the
  score is unstable.
- Leave that state only after the per-player-to-live copy has completed, then
  take an immediate stable sample and apply monotonicity to that value.
- Locate both operations by opcode signature and require their score operands
  to agree with the already independently confirmed `SIFFRORNA` address. As
  with the existing locators, ambiguity should disable scoring rather than
  guess.

Hooking semantic boundaries is preferable to a time delay: it is independent
of `ips`, table complexity, compiler layout, and the poll phase. Because CPU
hooks run before an instruction, the restore-complete hook must be placed on
the instruction after the copy (or use an equivalent explicit completion
site), not on the `REP MOVSW` before it executes.

An alternative, somewhat larger design is to locate the saved
`PLAYER_AREA[player].P_SIFFRORNA` through the two opposite-direction copy sites
and model the logical one-player score from both copies. During active play the
live value is current; during reset the saved value is authoritative. This is
especially useful if later verification needs stable score observations at
arbitrary times, but it requires table-specific structure stride/index handling
and more locator validation.

A debounce is an acceptable minimal mitigation but a weaker fix: hold a lower
sample as pending and invalidate only if it remains lower after a stable
semantic boundary (or, less reliably, for a full frame/several polls). Do not
special-case the value zero, and do not silently clamp all reads to the maximum
seen. Both approaches can hide a real corruption and confuse the working
buffer with the logical score in a different way.

## Validation plan

1. Replay `sessions/FANTASYDX_20260918_062237.pfr` with the fix. Attempt 1
   should be rankable with final score 8,826,490; the transient clear must not
   produce `score went DOWN`.
2. Confirm that replay end time, cycle count, event count, and any enabled
   audio/frame hashes are unchanged with and without `-scoredbg`.
3. Add cases for Tables 2, 3, and 4 that reach an ordinary second ball, an
   earned extra ball, a losing match, and a winning match. Varying `ips` is a
   useful way to vary poll phase, but correctness must not depend on it.
4. Keep a negative test in which a stable score really is reduced; it must
   still invalidate the attempt.
5. Re-run `tools/scorescan.py FANTASY FANTASYA FANTASYDX` and extend its static
   checks for any new clear/restore locators across all ranked binaries.

## Post-game restart after high-score entry is expected

The attempt opened 8 ms after attempt 1 enters attract mode is not evidence of
another verifier bug. The recording's final inputs are the three high-score
initials followed by Enter. After a complete game over, entering the allowed
three characters alone returns the game to attract mode; pressing Enter after
the name instead starts a new round directly at ball 1, player 1.

The verifier should therefore leave this Enter-triggered attempt in its
ordinary open/unlocked state. Its initial `players=1` is provisional: the
player may still use the F-keys to increase the player count, so the count must
be sampled and fixed at the first launch, as it is for the first attempt. If
the recording ends before that launch, reporting the trailing attempt as
unfinished is correct and must not retroactively invalidate an earlier clean,
rankable attempt.

A segmentation regression test should cover both high-score exit paths: three
initials without Enter must return to attract mode without opening another
attempt, while three initials followed by Enter must open a new attempt.

## What was implemented

The recommended fix, semantic brackets rather than a debounce.
`fantasies_find_score()` locates both sides and `cpu_step()` hooks them:

| Bracket | Shape | Hook |
| --- | --- | --- |
| clear | `MOV DI,SIFFRORNA / MOV CX,6 / REP STOSW` | on the `MOV DI`, before the buffer is touched |
| restore | `MOV DI,SIFFRORNA / MOV CX,6 / REP MOVSW` (tables 1, 2, 4) or `MOV DI,SIFFRORNA / PUSH DS / POP ES / MOV CX,12 / REP MOVSB` (table 3) | on the address just past the string op, since a hook fires before its instruction |

Both are anchored on the already confirmed score address, which is what makes
shapes this short safe; neither collides with `zeroscore`, which loads `CX`
before `DI`. Exactly one clear and exactly one restore in each of the twelve
ranked programs - `tools/scorescan.py` checks this and prints `txn=1/1w` or
`txn=1/1b`, so a new release has to pass it too.

Between the brackets the poll does not read the buffer at all: no
monotonicity, no BCD check, no update of the last-seen value. When the restore
bracket fires it takes an immediate sample and compares it against the value
from before the clear, so a restore that genuinely came back lower still sets
`decreased` and still invalidates the attempt. Zero is not special-cased and
nothing is clamped.

One addition not in the recommendation: a watchdog. If a clear is not followed
by a restore within half a second of emulated time - about four orders of
magnitude more than the real gap - the window is closed anyway, the run says
so once, and the exit report counts it. Without that, a single mislocated
bracket would silently disable the monotonicity check for the rest of the
session, which is a worse failure than the false positive this fixes.

Per-attempt output gained a `resets=` count of transactions observed, next to
the existing `springflips=`, on the same principle: the numbers that drove a
decision stay visible in the log.

Not implemented: the larger design that models the logical score from both
copy directions, and the segmentation regression tests for the two high-score
exit paths. The trailing Enter-triggered attempt is already handled the way
this document asks - it opens in the ordinary unlocked state, samples its
player count at the first launch, and reports as unfinished without touching
the earlier attempt's result.
