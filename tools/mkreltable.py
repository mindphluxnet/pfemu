#!/usr/bin/env python3
"""Regenerate src/reltable.h from collected Pinball Fantasies installations.

Identity for a release is the SHA-256 of its program files; this script
records the complete top-level manifest of each collected directory so the
detector in src/release.c can also report completeness and list the files it
is deliberately ignoring.  See docs/RELEASES.md for what each release is and
why the directory name is never used as identity.

Which programs a distribution ships is itself part of its shape: the full
game has INTRO.PRG and TABLE1-4.PRG, the 1993 demo has DEMO.PRG and PLAND.PRG.
That is the LAYOUTS table below, and src/release.c carries the same two.

Adding a newly collected release:
  1. put its files directly in a directory of their own;
  2. add a (stable_id, directory, boot_basename, layout) row to RELEASES
     below, and a new layout if its programs are named differently again;
  3. run this from the repo root:  python tools/mkreltable.py > src/reltable.h
  4. add the matching descriptor (label, layout, options-buffer offset, which
     intro fallback it uses, cd.nfo check) to `releases[]` in src/release.c.

Never broaden an existing hash to make an unknown build launch.
"""
import hashlib
import os
import sys

# The program filenames a distribution ships, anchor (the intro) first.
LAYOUTS = {
    "full": ["INTRO.PRG", "TABLE1.PRG", "TABLE2.PRG", "TABLE3.PRG", "TABLE4.PRG"],
    "demo": ["DEMO.PRG", "PLAND.PRG"],
}

# stable id, directory holding one unmodified installation, boot program, layout
RELEASES = [
    ("floppy",     "FANTASY",   "PINBALL.EXE", "full"),
    ("power_pack", "FANTASYA",  "PF.EXE",      "full"),
    ("deluxe",     "FANTASYDX", "PINBALL.EXE", "full"),
    ("demo",       "FANTDEMO",  "PFDEMO.EXE",  "demo"),
]

# Needed for an ordinary launch, beyond the programs and the boot file.  The
# sound drivers listed are only the two pfemu itself selects (SoundBlaster, or
# silence); the rest ship with the game but are never chosen, so a copy
# missing them still plays.  The demo has one table's music and no MOD2.MOD.
EXTRA_REQUIRED = {
    "full": {"INTRO.MOD", "MOD2.MOD",
             "TABLE1.MOD", "TABLE2.MOD", "TABLE3.MOD", "TABLE4.MOD",
             "SBLASTER.SDR", "NOSOUND.SDR"},
    "demo": {"INTRO.MOD", "TABLE1.MOD", "SBLASTER.SDR", "NOSOUND.SDR"},
}

# Written by the game or an installer: recorded, never part of identity.
MUTABLE = {"SOUND.CFG"}

# Packaging/wrapper files: recorded, never required.  The demo's four .PCX
# screens belong here - its own INSTALL.BAT copies *.sdr/*.mod/*.prg/*.exe/
# *.bin and not them, and no program in the release names one.
META = {"PINBALL.BAT", "21STINFO.DAT", "INSTALL.BAT",
        "BILLION.PCX", "PARTY.PCX", "SPEED.PCX", "STONE.PCX"}

# INTRO.MOD's last two bytes are the game's own manual-check sentinel, so a
# played installation legitimately differs there.  Hash the rest.
PREFIX = {"INTRO.MOD": 252868}

HEADER = """\
/* Generated from the collected installations by tools/mkreltable.py -
 * do not edit by hand.  Every hash here was cross-checked against the
 * manifests published in docs/RELEASES.md.
 *
 * flags: RF_CODE    part of the identity vector (this release's programs)
 *        RF_BOOT    the program pfemu executes to start this release
 *        RF_REQ     must be present for the game to run
 *        RF_PREFIX  hash covers only the first `prefix` bytes
 *        RF_MUTABLE the game or an installer rewrites it; never identity
 *        RF_META    packaging metadata, not a runtime dependency
 */
"""


def emit(out, rid, directory, boot, layout):
    code = set(LAYOUTS[layout])
    required = code | EXTRA_REQUIRED[layout]
    names = sorted(n for n in os.listdir(directory)
                   if os.path.isfile(os.path.join(directory, n)))
    out.write("\nstatic const RelFile files_%s[] = {\n" % rid)
    for name in names:
        path = os.path.join(directory, name)
        size = os.path.getsize(path)
        data = open(path, "rb").read()
        flags = []
        if name.upper() in code:
            flags.append("RF_CODE")
        if name == boot:
            flags.append("RF_BOOT")
        if name.upper() in required or name == boot:
            flags.append("RF_REQ")
        if name in MUTABLE:
            flags.append("RF_MUTABLE")
        if name in META:
            flags.append("RF_META")
        plen = PREFIX.get(name, 0)
        if plen:
            flags.append("RF_PREFIX")
            data = data[:plen]
        digest = hashlib.sha256(data).hexdigest()
        out.write('  { "%s", %d, %d, %s,\n' % (name, size, plen, "|".join(flags) or "0"))
        out.write('    {%s} },\n' %
                  ",".join("0x" + digest[i:i + 2] for i in range(0, 64, 2)))
    out.write("};\n")


def main():
    out = sys.stdout
    out.write(HEADER)
    for rid, directory, boot, layout in RELEASES:
        if not os.path.isdir(directory):
            sys.stderr.write("missing installation directory: %s\n" % directory)
            return 1
        emit(out, rid, directory, boot, layout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
