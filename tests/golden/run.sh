#!/bin/sh
# Golden-vector suite (docs/VERIFY.md, "Cross-platform determinism").
#
# Replays each .pfr beside this script and checks the footer, the -wav hash and
# the -shotevery frame hashes against the recorded .expected file.  A vector's
# expected values come from the session that produced it, so this is not two
# replays agreeing with each other - it is a replay agreeing with the original
# run, on whatever host and compiler are in front of it.
#
# Usage:  tests/golden/run.sh [path-to-binary] [path-to-install]
#
# Needs the game files, which are not in this repository.  Point the second
# argument at an installation of the matching release, or set PFEMU_INSTALL.
#
# Exit status is 0 only if every vector matched.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
BIN=${1:-${PFEMU_BIN:-$root/pfemu-headless}}
INSTALL=${2:-${PFEMU_INSTALL:-$root/FANTASYDX}}

[ -x "$BIN" ]     || { echo "no binary at $BIN (run make)"; exit 2; }
[ -d "$INSTALL" ] || { echo "no installation at $INSTALL"; exit 2; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
fail=0

for pfr in "$here"/*.pfr; do
    name=$(basename "$pfr" .pfr)
    exp="$here/$name.expected"
    [ -f "$exp" ] || { echo "SKIP $name (no .expected)"; continue; }

    echo "== $name"
    d="$work/$name"; mkdir -p "$d"
    ( cd "$d" && "$BIN" -d "$INSTALL" -freezetime -replay "$pfr" \
         -wav out.wav -shotevery 20 >run.log 2>&1 )

    bad=0

    # The emulator compares the footer's hash against this run's itself, which
    # is the check that matters: it is the original session's number.
    if grep -q 'wav hash MATCH' "$d/run.log"; then
        echo "   wav      MATCH $(sed -n 's/.*wav hash MATCH: \([0-9a-f]*\).*/\1/p' "$d/run.log")"
    else
        echo "   wav      FAIL  $(grep -h 'wav' "$d/run.log" | head -1)"
        bad=1
    fi

    want_emu=$(awk '$1=="end_emu"{print $2}' "$exp")
    want_cyc=$(awk '$1=="end_cycles"{print $2}' "$exp")
    if grep -q "actual ${want_emu}s / ${want_cyc} cycles" "$d/run.log"; then
        echo "   footer   ok    ${want_emu}s / ${want_cyc} cycles"
    else
        echo "   footer   FAIL  $(grep -h 'recorded end' "$d/run.log")"
        bad=1
    fi

    nf=0; badf=0
    while read -r kw idx want; do
        [ "$kw" = "frame" ] || continue
        f="$d/seq$idx.ppm"
        nf=$((nf+1))
        if [ ! -f "$f" ]; then badf=$((badf+1)); continue; fi
        got=$(md5sum < "$f" | cut -d' ' -f1)
        [ "$got" = "$want" ] || badf=$((badf+1))
    done < "$exp"
    if [ "$badf" -eq 0 ]; then
        echo "   frames   ok    $nf/$nf"
    else
        echo "   frames   FAIL  $badf of $nf differ or are missing"
        bad=1
    fi

    # The score, for vectors that contain a complete game.  This is the
    # number a verification service would publish, so it is the one the
    # suite most needs to pin - a cross-platform divergence that changed it
    # while leaving the audio alone would otherwise pass everything above.
    #
    # It costs a second replay rather than adding -scoredbg to the first.
    # VERIFY.md argues the flag is observation-only and demonstrates it on
    # one vector, but the artifacts checked above are the suite's whole
    # reason to exist, and they are not the place to take that on trust.
    # Vectors with no attempt lines - mid-game excerpts - skip this and pay
    # nothing.
    if grep -q '^attempt ' "$exp"; then
        ( cd "$d" && "$BIN" -d "$INSTALL" -freezetime -replay "$pfr" \
             -scoredbg >score.log 2>&1 )
        got=$(sed -n 's/^\[score\] attempt \([0-9]*\) table=\([0-9]*\).*score=\([0-9]*\).*ball_reached=\([0-9]*\) launches=\([0-9]*\).*ended=\([a-z]*\).*/attempt \1 \2 \3 \4 \5 \6/p' "$d/score.log")
        want=$(grep '^attempt ' "$exp")
        if [ "$got" = "$want" ]; then
            echo "   score    ok    $(echo "$want" | awk '{print $4}' | tr '\n' ' ')"
        else
            echo "   score    FAIL"
            echo "$want" | sed 's/^/     want /'
            echo "$got"  | sed 's/^/     got  /'
            bad=1
        fi
        # The watchdog added with the ball-return invariant: if it fires,
        # something returned a ball by a route the segmenter does not model,
        # and the score above should not be believed even if it matched.
        if grep -q 'went BACKWARDS' "$d/score.log"; then
            echo "   invariant FAIL $(grep -h 'went BACKWARDS' "$d/score.log" | head -1)"
            bad=1
        fi
    fi

    [ "$bad" -eq 0 ] || fail=1
done

if [ "$fail" -eq 0 ]; then
    echo "all vectors matched"
else
    echo "FAILED - see above; tools/pfsdiff.py bisects a divergence from snapshots"
fi
exit $fail
