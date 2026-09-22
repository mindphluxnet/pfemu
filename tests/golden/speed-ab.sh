#!/bin/sh
# Is host pacing guest-invisible?  (docs/VERIFY.md, "Open questions")
#
# Replays one vector twice - once wall-clock paced as every replay is today,
# once with -unthrottle - and compares the two runs artifact by artifact:
# footer, -wav hash and every -shotevery frame.  Identical output means the
# pacer cannot reach the guest, which is what verification needs before it
# runs replays flat out.  It also prints the speedup, which is the other
# number the service design depends on (docs/VERIFY.md, "Capacity").
#
# This is NOT the golden gate.  run.sh checks a replay against the original
# human session; this checks two replays against each other, and both are
# additionally checked against the recorded footer hash by the emulator
# itself, so a shared regression would still show up as two wav FAILs.
#
# Usage:  tests/golden/speed-ab.sh [vector.pfr] [path-to-binary] [install]
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

PFR=$(abspath "${1:-$here/deluxe-table3-200s.pfr}")
BIN=$(abspath "${2:-${PFEMU_BIN:-$root/pfemu-headless}}")
INSTALL=$(abspath "${3:-${PFEMU_INSTALL:-$root/FANTASYDX}}")

[ -f "$PFR" ]     || { echo "no vector at $PFR"; exit 2; }
[ -x "$BIN" ]     || { echo "no binary at $BIN (run make)"; exit 2; }
[ -d "$INSTALL" ] || { echo "no installation at $INSTALL"; exit 2; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT

# $1 = label, $2 = extra flags.  -freezetime on both: without it the guest
# folds the wall clock into its own state and the runs differ for a reason
# that has nothing to do with pacing.
run_one() {
    d="$work/$1"; mkdir -p "$d"
    start=$(date +%s)
    # shellcheck disable=SC2086
    ( cd "$d" && "$BIN" -d "$INSTALL" -freezetime -replay "$PFR" \
        -wav out.wav -shotevery 20 $2 >run.log 2>&1 )
    end=$(date +%s)
    echo $((end - start)) > "$d/wall"
}

echo "vector:  $PFR"
echo "binary:  $BIN"
echo
echo "== paced (today's behaviour)"
run_one paced ""
echo "   $(cat "$work/paced/wall")s wall"
echo "== unthrottled"
run_one fast "-unthrottle"
echo "   $(cat "$work/fast/wall")s wall"
echo

bad=0

# The emulator's own check: each run against the footer the ORIGINAL session
# recorded.  A pass here means neither run drifted from the human play-through,
# which is stronger than the two merely agreeing with each other.
for r in paced fast; do
    if grep -q 'wav hash MATCH' "$work/$r/run.log"; then
        echo "wav vs recording   $r  MATCH $(sed -n 's/.*wav hash MATCH: \([0-9a-f]*\).*/\1/p' "$work/$r/run.log")"
    else
        echo "wav vs recording   $r  FAIL  $(grep -h 'wav' "$work/$r/run.log" | head -1)"
        bad=1
    fi
done

fa=$(sed -n 's/.*actual \([0-9.]*s \/ [0-9]* cycles\).*/\1/p' "$work/paced/run.log" | tail -1)
fb=$(sed -n 's/.*actual \([0-9.]*s \/ [0-9]* cycles\).*/\1/p' "$work/fast/run.log" | tail -1)
if [ -n "$fa" ] && [ "$fa" = "$fb" ]; then
    echo "footer paced==fast      ok    $fa"
else
    echo "footer paced==fast      FAIL  paced='$fa' fast='$fb'"
    bad=1
fi

na=$(ls "$work/paced" | grep -c '^seq.*\.ppm$')
nb=$(ls "$work/fast"  | grep -c '^seq.*\.ppm$')
if [ "$na" != "$nb" ]; then
    echo "frames paced==fast      FAIL  $na frames paced, $nb unthrottled"
    bad=1
elif [ "$na" -eq 0 ]; then
    echo "frames paced==fast      FAIL  no frames captured at all"
    bad=1
else
    diffs=0
    for f in "$work/paced"/seq*.ppm; do
        n=$(basename "$f")
        cmp -s "$f" "$work/fast/$n" || diffs=$((diffs+1))
    done
    if [ "$diffs" -eq 0 ]; then
        echo "frames paced==fast      ok    $na/$na byte-identical"
    else
        echo "frames paced==fast      FAIL  $diffs of $na differ"
        bad=1
    fi
fi

wa=$(cat "$work/paced/wall"); wb=$(cat "$work/fast/wall")
echo
echo "wall: ${wa}s paced -> ${wb}s unthrottled"
# The ratio that matters for sizing is emulated seconds per wall second,
# taken from the emulator's own pace line rather than from date(1): whole
# seconds and shell integer division round a 3.7x down to "3x", and this
# number is a capacity input (docs/VERIFY.md, "Capacity").
pace=$(grep -h '^\[pfemu\] pace:' "$work/fast/run.log" | tail -1)
echo "$pace" | sed 's/^/fast /'
echo "$pace" | awk '{
    for (i = 1; i <= NF; i++) {
        if ($i ~ /^wall=/) { w = substr($i, 6); sub(/s$/, "", w) }
        if ($i ~ /^emu=/)  { e = substr($i, 5); sub(/s$/, "", e) }
    }
    if (w > 0) printf "real-time factor: %.2fx  (one gameplay-hour = %.0f min of one core)
", e/w, 60*w/e
}'

echo
if [ "$bad" -eq 0 ]; then
    echo "PASS - host pacing is not guest-observable on this vector"
else
    echo "FAIL - see above.  If only the paced/fast comparisons failed, the"
    echo "pacer IS reaching the guest and -unthrottle cannot be used for"
    echo "verification until that is understood."
fi
exit $bad
