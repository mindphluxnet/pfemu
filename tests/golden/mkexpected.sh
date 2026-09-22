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

# Both scripts replay from inside a scratch directory, so every path handed
# to the emulator has to survive a cd.  A relative one does not, and the
# failure is thoroughly misleading: the replay never opens the file, the run
# produces no audio, and the wav check then reports a hash mismatch on a
# vector that is perfectly fine.
abspath() {
    case $1 in
        /*) printf '%s\n' "$1" ;;
        *)  printf '%s/%s\n' "$(pwd)" "$1" ;;
    esac
}

[ $# -ge 1 ] || { echo "usage: $0 <vector.pfr> [binary] [install]"; exit 2; }
PFR=$(abspath "$1")
BIN=$(abspath "${2:-${PFEMU_BIN:-$root/pfemu-headless}}")
INSTALL=$(abspath "${3:-${PFEMU_INSTALL:-$root/FANTASYDX}}")
UNTHROTTLE=${PFEMU_UNTHROTTLE:-0}

[ -f "$PFR" ]     || { echo "no vector at $PFR"; exit 2; }
[ -x "$BIN" ]     || { echo "no binary at $BIN (run make)"; exit 2; }
[ -d "$INSTALL" ] || { echo "no installation at $INSTALL"; exit 2; }

name=$(basename "$PFR" .pfr)
exp="$(dirname "$PFR")/$name.expected"

# Straight out of the .pfr, not out of the replay.
#
# A .pfr is written by the Windows recorder and keeps CRLF - deliberately,
# since it is `-text` in .gitattributes because the file is hashed over its
# own raw bytes. So every field pulled out of one carries a trailing
# carriage return, and a shell that has just built a grep pattern out of it
# will never match a line the emulator wrote on a POSIX host. It fails
# silently and prints the two values side by side looking identical, which
# is exactly as confusing as it sounds. Strip it once, here.
pfrfield() {
    awk -v k="$1:" '$1==k { v=$2; sub(/\r$/, "", v); print v; exit }' "$PFR"
}
f_release=$(pfrfield release)
f_program=$(pfrfield program)
f_emu=$(pfrfield end_emu)
f_cyc=$(pfrfield end_cycles)
f_wav=$(pfrfield wav_hash)
f_smp=$(pfrfield wav_samples)

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
# the untested combination, so the artifact run does not carry it.
#
# Honest history, because the comment here used to imply otherwise: the
# first version of this script did pass -scoredbg with -wav AND failed the
# wav check - but the cause was the relative-path bug above, not the flag.
# The replay never opened the vector at all.  -scoredbg has not been shown
# to move the audio and is not accused of it here.  The runs stay split
# because run.sh splits them for a stated reason and a sibling script
# disagreeing with it silently is worse than one extra replay - and
# because -shotevery has form (dd938f.. with, dfb1427.. without), which is
# why captures no longer clamp the batch.
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
# Two different failures, and conflating them wasted a round trip once:
# a replay that never started looks exactly like a hash mismatch if you
# only test for the MATCH line.
if ! grep -q 'wav hash' "$work/run.log"; then
    echo "REFUSING: the replay did not run to completion - no capture hash"
    echo "  was reported at all, so there is nothing to compare."
    echo
    echo "  --- last 25 lines of the replay log ---"
    tail -25 "$work/run.log" | sed 's/^/  /'
    bail
fi
if ! grep -q 'wav hash MATCH' "$work/run.log"; then
    echo "REFUSING: this replay does not match the recording's capture hash."
    echo "  Generating .expected from it would pin the disagreement as correct."
    echo
    grep -h 'wav' "$work/run.log" | sed 's/^/  /'
    bail
fi
if ! grep -q "actual ${f_emu}s / ${f_cyc} cycles" "$work/run.log"; then
    echo "REFUSING: footer disagrees with the recording."
    echo "  wanted:  actual ${f_emu}s / ${f_cyc} cycles"
    grep -h 'recorded end' "$work/run.log" | sed 's/^/  got:     /'
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
        echo "# -scoredbg: index table score ball_reached launches ended rankable"
        awk '/^\[score\] attempt /{t="";s="";b="";l="";e="";for(i=1;i<=NF;i++){if($i ~ /^table=/)t=substr($i,7);else if($i ~ /^score=/)s=substr($i,7);else if($i ~ /^ball_reached=/)b=substr($i,14);else if($i ~ /^launches=/)l=substr($i,10);else if($i ~ /^ended=/)e=substr($i,7)}r=(index($0,"[RANKABLE]")>0)?"yes":"no";printf "attempt %s %s %s %s %s %s %s\n",$3,t,s,b,l,e,r}' "$work/score.log"
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
