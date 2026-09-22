#!/bin/sh
# Run every golden vector under the UndefinedBehaviorSanitizer build.
#
# This used to be a multi-line shell snippet in docs/HANDOFF.md, which is a
# bad way to ship a command: pasted into cmd.exe it is chopped up line by
# line and the first fragment is an unterminated bash command, so nothing
# happens and nothing says why.  It also passed the repo root around in
# $OLDPWD, which the loop's own `cd` overwrites after the first vector.
#
# Usage:  tests/golden/ubsan.sh [install]
#
# Needs the game files, like run.sh.  Set PFEMU_INSTALL or pass the path.
# PFEMU_UNTHROTTLE=1 drops the wall-clock pacer.  Instrumented runs are slow
# and these vectors are eight emulated minutes between them, so that is worth
# considering - but it does make the artifact comparison below less of a
# clean measurement, since the pacer has only been proven guest-invisible on
# the uninstrumented build.
#
# The work directories are deliberately NOT deleted.  run.sh removes its own,
# which is right for a pass/fail gate and exactly wrong here: the output being
# hunted for is on stderr, and HANDOFF.md documents somebody losing it that
# way once already.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)

INSTALL=${1:-${PFEMU_INSTALL:-$root/FANTASYDX}}
case $INSTALL in /*) ;; *) INSTALL="$(pwd)/$INSTALL" ;; esac
[ -d "$INSTALL" ] || { echo "no installation at $INSTALL"; exit 2; }

BIN=$root/pfemu-headless-ubsan
UNTHROTTLE=${PFEMU_UNTHROTTLE:-0}
work=${PFEMU_UBSAN_DIR:-/tmp/pfemu-ubsan}

# `make ubsan` cleans first and drops its objects afterwards, so the two
# configurations cannot contaminate each other (see the Makefile).  Building
# here rather than asking for it separately keeps this to one command.
echo "== building $BIN"
( cd "$root" && make ubsan ) >"$work.build.log" 2>&1 || {
    echo "build failed; see $work.build.log"
    tail -20 "$work.build.log" | sed 's/^/  /'
    exit 2
}
[ -x "$BIN" ] || { echo "no $BIN after make ubsan"; exit 2; }

rm -rf "$work"
mkdir -p "$work" || exit 2

flags="-freezetime"
[ "$UNTHROTTLE" = 1 ] && flags="$flags -unthrottle"

nvec=0
for pfr in "$here"/*.pfr; do
    name=$(basename "$pfr" .pfr)
    nvec=$((nvec + 1))
    d="$work/$name"
    mkdir -p "$d"
    echo "== $name"
    # shellcheck disable=SC2086
    ( cd "$d" && "$BIN" -d "$INSTALL" $flags -replay "$pfr" \
        -wav out.wav -shotevery 20 >run.log 2>&1 )

    n=$(grep -ic 'runtime error' "$d/run.log" 2>/dev/null || true)
    [ -n "$n" ] || n=0
    echo "   runtime errors: $n"

    # An instrumented build is -O1 where the normal one is -O2, so a matching
    # capture hash here is a small extra piece of evidence: the run survives a
    # different optimisation level unchanged.  A mismatch is not automatically
    # a bug in the emulator - ubsan changes codegen - but it is worth seeing.
    if grep -q 'wav hash MATCH' "$d/run.log"; then
        echo "   capture hash:   MATCH (same as the uninstrumented -O2 build)"
    elif grep -q 'wav hash' "$d/run.log"; then
        echo "   capture hash:   differs - $(grep -h 'wav hash' "$d/run.log" | head -1)"
    else
        echo "   capture hash:   no capture reported - did the replay run?"
        tail -5 "$d/run.log" | sed 's/^/     /'
    fi
done

echo
echo "== unique runtime errors across $nvec vector(s)"
found=0
# Locations kept deliberately: the previous pass could only conclude
# "all in dos.c and bios.c, none in cpu.c" because it had them, and that
# distinction was the whole finding.
if grep -ih 'runtime error' "$work"/*/run.log 2>/dev/null | sort -u | grep . ; then
    found=1
else
    echo "  none"
fi

echo
echo "Logs under $work/ (kept on purpose)."
# `make ubsan` starts with `make clean`, which takes pfemu-headless and
# pfemu-fuzz-pfr with it.  Leaving without saying so means the next run.sh
# fails with "no binary" for a reason that has nothing to do with run.sh.
echo "Note: 'make ubsan' removed pfemu-headless and pfemu-fuzz-pfr."
echo "      Run 'make' before the next tests/golden/run.sh."
exit $found
