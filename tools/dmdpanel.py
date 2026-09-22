#!/usr/bin/env python3
"""Transcribe the dot-matrix panel out of a pfemu screenshot.

The panel is a regular grid of lit and unlit dots, so thresholding a frame and
quantising to that grid gives an exact rendering of what the player sees -
which is the only way to check `-scoredbg` against the game's own display
without squinting at a scaled screenshot.

    pfemu.exe -replay <file>.pfr -shotevery 5 -untilemu <t> -scoredbg > sweep.log 2>&1
    python tools/dmdpanel.py seq053.ppm

Then read the digits off the ASCII and compare them with the `score=` line in
the log at that moment.  `-shotevery` counts wall time scaled by speed and a
replay pins speed to 1, so frame *n* is about second *n x interval*; pick
frames inside a long gap between `score=` lines, because the panel spends much
of the end of a game on scrolling messages rather than digits.

Reading the digits is left to a person on purpose.  The score field is right
aligned, so where it starts depends on how many digits it has, and a fixed
glyph grid mis-slices as soon as that changes; a wrong transcription that
looks confident is worse than none.  Seven rows of ASCII are unambiguous to a
human, and this is a validation tool that runs a handful of times per table.

Defaults cover the 320x240 table view.  Pass an explicit region as
`y0 y1 x0 x1` for another mode or to crop to the score field alone.
"""
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("this needs Pillow: python -m pip install pillow")


def lit(px):
    """A panel dot is lit: orange on near-black, whatever the palette rotation."""
    r, g, b = px[:3]
    return r > 120 and g > 60 and r > b + 60


def pitch(vals):
    """The dot spacing, as the most common gap between lit rows or columns."""
    gaps = {}
    prev = vals[0]
    for v in vals[1:]:
        d = v - prev
        if d > 1:
            gaps[d] = gaps.get(d, 0) + 1
        prev = v
    return max(gaps, key=gaps.get) if gaps else 1


def panel(path, y0=200, y1=240, x0=0, x1=320):
    im = Image.open(path).convert('RGB')
    w, h = im.size
    p = im.load()
    y1, x1 = min(y1, h), min(x1, w)

    cols = [x for x in range(x0, x1) if any(lit(p[x, y]) for y in range(y0, y1))]
    rows = [y for y in range(y0, y1) if any(lit(p[x, y]) for x in range(x0, x1))]
    if not cols or not rows:
        print("%s: no lit dots in y %d..%d x %d..%d" % (path, y0, y1, x0, x1))
        return

    px_, py_ = pitch(cols), pitch(rows)
    print("%s: lit x %d..%d y %d..%d, dot pitch %dx%d"
          % (path, cols[0], cols[-1], rows[0], rows[-1], px_, py_))

    y = rows[0]
    while y <= rows[-1]:
        line = []
        x = cols[0]
        while x <= cols[-1]:
            line.append('#' if any(lit(p[xx, yy])
                                   for xx in range(x, min(x + px_, x1))
                                   for yy in range(y, min(y + py_, y1))) else '.')
            x += px_
        print("   " + "".join(line))
        y += py_


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    paths, nums = [], []
    for a in argv[1:]:
        (nums if a.lstrip('-').isdigit() else paths).append(a)
    region = [int(n) for n in nums]
    for path in paths:
        panel(path, *region)
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
