#!/usr/bin/env python3
"""Decode the game's own high-score file (TABLEn.HI).

An independent check on `-scoredbg`: the number pfemu reads out of guest
memory and the number the game itself writes to disk share nothing but the
game, so agreement between them is worth more than either alone.  Replay a
recording with `-keepoverlay`, then point this at the overlay it leaves
behind:

    pfemu.exe -replay sessions\\FANTASYDX_...pfr -scoredbg -keepoverlay > log 2>&1
    python tools/hiscore.py "%TEMP%\\pfemu_replay_12345_678\\table3.hi"

Format, from FANTASIE.ASM's INIT_HIGHS/SAVE_HIGHS (`16*4` bytes) and the
compare loop in CHECKHIGHSCORE (12 bytes per entry): four 16-byte records,
each 12 unpacked BCD digits most significant first - the same encoding as the
live SIFFRORNA buffer - then three ASCII initials and a NUL.

A factory-fresh file reads 50,000,000 TSP / 25,000,000 ANY / 10,000,000 J L /
5,000,000 ICE, which is the quickest way to tell "the run never made the
table" from "the file was not written at all".
"""
import sys

ENTRY = 16
DIGITS = 12


def decode(blob):
    out = []
    for i in range(0, len(blob) - ENTRY + 1, ENTRY):
        rec = blob[i:i + ENTRY]
        digits = rec[:DIGITS]
        if any(d > 9 for d in digits):
            out.append((None, None, digits))
            continue
        score = 0
        for d in digits:
            score = score * 10 + d
        name = rec[DIGITS:].split(b'\0')[0].decode('latin-1')
        out.append((score, name, digits))
    return out


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    rc = 0
    for path in argv[1:]:
        blob = open(path, 'rb').read()
        print("== %s (%d bytes)" % (path, len(blob)))
        if len(blob) != ENTRY * 4:
            print("   warning: expected %d bytes, got %d" % (ENTRY * 4, len(blob)))
        for n, (score, name, digits) in enumerate(decode(blob), 1):
            raw = " ".join("%02X" % d for d in digits)
            if score is None:
                print("   %d. <not BCD>  %s" % (n, raw))
                rc = 1
            else:
                print("   %d. %15s  %-3s  [%s]" % (n, "{:,}".format(score), name, raw))
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv))
