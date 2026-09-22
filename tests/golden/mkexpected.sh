#!/bin/sh
# Generate the .expected file for a golden vector.
#
# Adding a vector used to mean assembling this by hand, which is both tedious
# and a good way to pin a typo as the expected value.  There will be more
# vectors - the suite is meant to grow - so this exists.
#
# Usage:  tests/golden/mkexpected.sh <vector.pfr> [binary] [install]
#
# Needs the game files, like run.sh.  Set PFEMU_INSTALL or pass the path.
# PFEMU_UNTHROTTLE=1 drops the wall-clock pacer (3.7x faster; measured
# guest-invisible on one vector by speed-ab.sh - run that on a new vector
# before trusting it on that one).
#
# WHERE EACH VALUE COMES FROM, because it is the point of the whole format:
#
#   end_emu, end_cycles, wav_hash, wav_samples
#       Copied from the .pfr FOOTER, never from this replay.  They are what
#       the original human session produced, so a replay that reproduces them
#       agrees with the recording rather than merely with another replay.
#       That distinction is the reason the suite is worth anything.
#
#   frame NNN <md5>, attempt ...
#       These DO come from this replay, because the recording never captured
#       frames or a score.  So they say "every other machine agrees with the
#       machine that generated this file", which is weaker.  Generate them on
#       a machine whose footer and wav hash already match the recording - the
#       script checks that and refuses otherwise, so the weaker values are at
#       least anchored to a run that passed the stronger check.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

[ $# -ge 1 ] || { echo "usage: $0 <vector.pfr> [binary] [install]"; exit 2; }
PFR=$1
BIN=${2:-${PFEMU_BIN:-$root/pfemu-headless}}
INSTALL=${3:-${PFEMU_INSTALL:-$root/FANTASYDX}}
UNTHROTTLE=${PFEMU_UNTHROTTLE:-0}

[ -f "$PFR" ]     || { echo "no vector at $PFR"; exit 2; }
[ -x "$BIN" ]     || { echo "no binary at $BIN (run make)"; exit 2; }
[ -d "$INSTALL" ] || { echo "no installation at $INSTALL"; exit 2; }

name=$(basename "$PFR" .pfr)
exp="$(dirname "$PFR")/$name.expected"

# Straight out of the .pfr, not out of the replay.
f_release=$(awk '$1=="release:"{print $2; exit}' "$PFR")
f_program=$(awk '$1=="program:"{print $2; exit}' "$PFR")
f_emu=$(awk '$1=="end_emu:"{print $2; exit}' "$PFR")
f_cyc=$(awk '$1=="end_cycles:"{print $2; exit}' "$PFR")
f_wav=$(awk '$1=="wav_hash:"{print $2; exit}' "$PFR")
f_smp=$(awk '$1=="wav_samples:"{print $2; exit}' "$PFR")

[ -n "$f_emu" ] && [ -n "$f_cyc" ] || { echo "$name: no footer in the .pfr"; exit 2; }
if [ -z "$f_wav" ] || [ "$f_wav" = none ]; then
    echo "$name: the recording carries no capture hash."
    echo "  Either it was made with sound off, or by a build from before the"
    echo "  hash was decoupled from -wav (it used to need the flag).  Without"
    echo "  it a vector cannot check a replay against the original session,"
    echo "  only against another replay, so re-record it."
    exit 2
fi

work=$(mktemp -d) || exit 2
# Only cleaned up on success.  A refusal that deletes the log it is telling
# you to read is the trap HANDOFF.md already warns about for run.sh, and
# this script walked straight into it the first time it was used.
keep=0
trap '[ "$keep" = 1 ] || rm -rf "$work"' EXIT
bail() { keep=1; echo; echo "log kept at $work/"; exit 1; }

flags="-freezetime"
[ "$UNTHROTTLE" = 1 ] && flags="$flags -unthrottle"

# The artifact run carries NO -scoredbg, and that is not fussiness.
# VERIFY.md shows the flag costs the guest nothing observable - but says in
# the same breath that "only the wav hash is still uncovered, because
# neither run captured audio".  -scoredbg together with -wav is precisely
# the untested combination.  The first version of this script used it and
# the wav check failed; whether -scoredbg caused that is NOT established -
# it may be something else entirely about this vector - but the artifact
# run has no reason to carry the risk while the question is open.  There
# is prior form: -shotevery once moved the audio the same way (dd938f..
# with, dfb1427.. without), which is why captures no longer clamp the
# batch.  Separating the runs settles it either way: if the artifacts now
# match, the flag was the cause and VERIFY.md gains a measurement.
#
# So: artifacts first, from a run with nothing extra attached.
echo "== replaying $name (artifacts)"
# shellcheck disable=SC2086
( cd "$work" && "$BIN" -d "$INSTALL" $flags -replay "$PFR" \
    -wav out.wav -shotevery 20 >run.log 2>&1 )

# Refuse to pin frames from a run that does not already agree with the
# recording: the frame hashes would then encode this machine's divergence as
# the expected answer, which is the worst possible failure mode for a suite
# whose whole job is catching divergence.
if ! grep -q 'wav hash MATCH' "$work/run.log"; then
    echo "REFUSING: this replay does not match the recording's wav hash."
    echo "  Generating .expected from it would pin the disagreement as correct."
    echo
    echo "  --- last 25 lines of the replay log ---"
    tail -25 "$work/run.log" | sed 's/^/  /'
    bail
fi
if ! grep -q "actual ${f_emu}s / ${f_cyc} cycles" "$work/run.log"; then
    echo "REFUSING: footer disagrees with the recording."
    grep -h 'recorded end' "$work/run.log" | sed 's/^/  /'
    bail
fi
echo "   wav and footer match the recording"

# Only now, in its own run, the score.
echo "== replaying $name (score)"
# shellcheck disable=SC2086
( cd "$work" && "$BIN" -d "$INSTALL" $flags -replay "$PFR" \
    -scoredbg >score.log 2>&1 )
if grep -q 'went BACKWARDS' "$work/score.log"; then
    echo "REFUSING: the ball-counter watchdog fired."
    grep -h 'went BACKWARDS' "$work/score.log" | sed 's/^/  /'
    echo "  A ball was returned by a route the segmenter does not model, so"
    echo "  the score from this run is not worth pinning."
    bail
fi

events=$(sed -n 's/.*: [a-z]*, \([0-9]*\) events,.*/\1/p' "$work/run.log" | head -1)
[ -n "$events" ] || events=0

{
    echo "# Golden vector: $name"
    echo "#"
    echo "# Generated by tests/golden/mkexpected.sh.  end_emu, end_cycles,"
    echo "# wav_hash and wav_samples are the ORIGINAL session's, copied from the"
    echo "# .pfr footer - so a replay checks itself against what a person"
    echo "# actually played, not against another replay.  The frame hashes and"
    echo "# the attempt lines could only come from a replay, since the recording"
    echo "# captured neither; they were generated on a run that reproduced the"
    echo "# footer and the wav hash exactly."
    echo "release      $f_release"
    echo "program      $f_program"
    echo "events       $events"
    echo "end_emu      $f_emu"
    echo "end_cycles   $f_cyc"
    echo "wav_hash     $f_wav"
    echo "wav_samples  $f_smp"
    echo "# -shotevery 20, md5 of each seqNNN.ppm"
    for f in "$work"/seq*.ppm; do
        [ -f "$f" ] || continue
        b=$(basename "$f" .ppm)
        printf 'frame %s %s\n' "${b#seq}" "$(md5sum < "$f" | cut -d' ' -f1)"
    done
    # The score, which is the number a verification service would publish and
    # the one thing the suite did not previously pin.  Only emitted when
    # -scoredbg found an attempt, so vectors that are mid-game excerpts simply
    # have no attempt lines and run.sh skips the check.
    if grep -q '^\[score\] attempt ' "$work/score.log"; then
        echo "# -scoredbg, one line per attempt (from a separate replay)"
        sed -n 's/^\[score\] attempt \([0-9]*\) table=\([0-9]*\).*score=\([0-9]*\).*ball_reached=\([0-9]*\) launches=\([0-9]*\).*ended=\([a-z]*\).*/attempt \1 \2 \3 \4 \5 \6/p' \
            "$work/score.log"
    fi
} > "$exp.new"

if [ -f "$exp" ]; then
    if cmp -s "$exp" "$exp.new"; then
        rm -f "$exp.new"
        echo "unchanged: $exp"
        exit 0
    fi
    echo "DIFFERS from the existing $exp:"
    diff -u "$exp" "$exp.new" | head -40
    echo
    echo "Left at $exp.new - move it into place yourself if the change is"
    echo "intended.  An expected value changing by itself is a finding."
    exit 1
fi

mv "$exp.new" "$exp"
echo "wrote $exp"
grep -c '^frame ' "$exp" | sed 's/^/  frames:   /'
grep -c '^attempt ' "$exp" | sed 's/^/  attempts: /'
