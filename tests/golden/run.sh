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

    # sub(/\r$/) because core.autocrlf is true here and a fresh Windows
    # clone can produce a CRLF .expected; a pattern built from it then
    # matches nothing while looking correct on screen.
    want_emu=$(awk '$1=="end_emu"{v=$2; sub(/\r$/,"",v); print v}' "$exp")
    want_cyc=$(awk '$1=="end_cycles"{v=$2; sub(/\r$/,"",v); print v}' "$exp")
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
             -scoredbg -verify verdict.json >score.log 2>&1 )
        got=$(awk '/^\[score\] attempt /{t="";s="";b="";l="";e="";for(i=1;i<=NF;i++){if($i ~ /^table=/)t=substr($i,7);else if($i ~ /^score=/)s=substr($i,7);else if($i ~ /^ball_reached=/)b=substr($i,14);else if($i ~ /^launches=/)l=substr($i,10);else if($i ~ /^ended=/)e=substr($i,7)}r=(index($0,"[RANKABLE]")>0)?"yes":"no";printf "attempt %s %s %s %s %s %s %s\n",$3,t,s,b,l,e,r}' "$d/score.log")
        want=$(grep '^attempt ' "$exp")
        # .expected files written before rankable was pinned carry one value
        # fewer. Compare what they actually claim rather than failing every
        # older vector on a format change.
        #
        # "attempt" is itself field 1, so the old six-value format is SEVEN
        # tokens and the new seven-value one is EIGHT. Getting that backwards
        # truncated `ended` off as well, which a test caught and reading did
        # not.
        if [ "$(printf '%s\n' "$want" | head -1 | awk '{print NF}')" != 8 ]; then
            got=$(printf '%s\n' "$got" | cut -d' ' -f1-7)
        fi
        if [ "$got" = "$want" ]; then
            echo "   score    ok    $(echo "$want" | awk '{print $4}' | tr '\n' ' ')"
        else
            echo "   score    FAIL"
            echo "$want" | sed 's/^/     want /'
            echo "$got"  | sed 's/^/     got  /'
            bad=1
        fi
        # The -verify verdict (src/verify.c), which is what a service reads.
        # Checked against the report the SAME run just printed, not against
        # .expected, and deliberately so: .expected already pins the score
        # above, and a second copy of the same number would only pin it
        # twice. What is unchecked until here is whether the JSON path and
        # the text path agree - two readers of the same state, one of them
        # new. If they ever disagree, the number a human reads out of the
        # log is not the number the service published, which is the worst
        # shape this failure could take.
        #
        # It costs no extra replay: -verify rides along on the -scoredbg
        # one, writes a file at exit, and changes nothing the guest sees.
        if [ -f "$d/verdict.json" ]; then
            vstat=$(sed -n 's/.*"status": "\([a-z_]*\)".*/\1/p' "$d/verdict.json" | head -1)
            # "best" is an object on the line after the key, or the literal
            # null - in which case the next line has no score and this is
            # empty, which is what the log side produces too.
            vbest=$(awk '/"best":/{getline;if(match($0,/"score": [0-9]+/))print substr($0,RSTART+9,RLENGTH-9);exit}' "$d/verdict.json")
            sbest=$(awk '/^\[score\] attempt /&&index($0,"[RANKABLE]")>0{for(i=1;i<=NF;i++)if($i ~ /^score=/){v=substr($i,7)+0;if(v>m)m=v}}END{if(m)print m}' "$d/score.log")
            if [ "$vstat" = verified ] && [ "$vbest" = "$sbest" ]; then
                echo "   verdict  ok    ${vbest:-verified, nothing rankable}"
            else
                echo "   verdict  FAIL  status=$vstat best=${vbest:-null}" \
                     "but score.log's best rankable is ${sbest:-none}"
                bad=1
            fi
        else
            echo "   verdict  FAIL  -verify wrote no file"
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
