#!/bin/sh
# Render a replay to a video file: pfemu-headless -video into ffmpeg.
#
# pfemu writes raw frames into a FIFO that ffmpeg encodes as they come, and
# a soundtrack on the side (src/video.c).  When the replay ends, the two are
# muxed.  pfemu never encodes, so nothing here needs a codec library in the
# emulator, and the encoder is whatever ffmpeg this machine has.
#
# It also prints what the render cost, which is the number that says
# whether the validator machine can afford videos at all.
#
# Usage:  tools/render-video.sh REPLAY.pfr OUT.mp4 [pfemu options...]
#   e.g.  tools/render-video.sh tests/golden/deluxe-table1-ranked-644s.pfr \
#             /tmp/party.mp4 -d FANTASYDX
#
# Environment:
#   PFEMU_BIN   the headless binary (default: ./pfemu-headless)
#   SCALE       output is 320*SCALE x 240*SCALE (default 2: 640x480, where
#               both the 320x240 tables and the 640x480 menu scale exactly)
#   PRESET      x264 preset (default veryfast)
#   CRF         x264 quality (default 20; lower is better and bigger)
#   NOVIDEO=1   run the same replay with no -video, for the baseline time
#
# Extra options go to pfemu as they are: -d for the install, -verify, -strict.
set -u

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)

[ $# -ge 2 ] || { sed -n '12,14p' "$0"; exit 2; }
PFR=$1; OUT=$2; shift 2
BIN=${PFEMU_BIN:-$root/pfemu-headless}
SCALE=${SCALE:-2}
PRESET=${PRESET:-veryfast}
CRF=${CRF:-20}
W=$((320 * SCALE)); H=$((240 * SCALE))

[ -f "$PFR" ] || { echo "no replay at $PFR"; exit 2; }
[ -x "$BIN" ] || { echo "no binary at $BIN (run make)"; exit 2; }
command -v ffmpeg >/dev/null || { echo "no ffmpeg on PATH"; exit 2; }

work=$(mktemp -d) || exit 2
trap 'rm -rf "$work"' EXIT
LOG=$OUT.log

now() { date +%s.%N; }
since() { awk -v a="$1" -v b="$(now)" 'BEGIN { printf "%.1f", b - a }'; }

if [ "${NOVIDEO:-}" = 1 ]; then
    t0=$(now)
    "$BIN" -replay "$PFR" -unthrottle "$@" >"$LOG" 2>&1
    rc=$?
    echo "baseline: pfemu exit $rc, $(since "$t0") s wall, no video (log: $LOG)"
    exit $rc
fi

mkfifo "$work/frames" || exit 2

# The rate is the table's CRT rate as an exact fraction, the same one
# src/video.c spaces its frames by.
ffmpeg -nostdin -loglevel error -y \
    -f rawvideo -pix_fmt bgr0 -s "${W}x${H}" -framerate 25175000/421600 \
    -i "$work/frames" \
    -c:v libx264 -preset "$PRESET" -crf "$CRF" -pix_fmt yuv420p \
    "$work/video.mkv" &
ff=$!

t0=$(now)
"$BIN" -replay "$PFR" -unthrottle -video "$work/frames" \
    -videowav "$work/sound.wav" -videoscale "$SCALE" "$@" >"$LOG" 2>&1
rc=$?
t_pfemu=$(since "$t0")

# pfemu opens the FIFO before anything else can fail, so its exit is
# ffmpeg's end of input.  If it died before that, ffmpeg is still waiting
# for a writer; do not wait for it forever.
( sleep 30; kill "$ff" 2>/dev/null ) &
dog=$!
wait "$ff"; frc=$?
kill "$dog" 2>/dev/null
t_enc=$(since "$t0")

grep -h '^\[video\]' "$LOG"
# exit 2 is a -verify mismatch, which still wrote a whole video
if [ $rc -ne 0 ] && [ $rc -ne 2 ]; then
    echo "pfemu exit $rc; see $LOG"; exit 1
fi
[ $frc -eq 0 ] || { echo "ffmpeg failed (exit $frc)"; exit 1; }

ffmpeg -nostdin -loglevel error -y -i "$work/video.mkv" -i "$work/sound.wav" \
    -c:v copy -c:a aac -b:a 128k -movflags +faststart "$OUT" || exit 1

echo "pfemu exit $rc; ${t_pfemu} s in pfemu, ${t_enc} s until the encoder" \
     "finished, $(since "$t0") s in all"
echo "wrote $OUT ($(du -h "$OUT" | cut -f1)); log: $LOG"
