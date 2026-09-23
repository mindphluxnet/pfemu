#!/usr/bin/env python3
"""Extract the data track of a GOG "game.gog" CD image (or any ISO 9660 image).

GOG ships Pinball Fantasies Deluxe as a raw CD image: `game.gog` is track 1 of
the disc, and `game.inst` is the cue sheet next to it (the audio tracks are
the MUSIC\\TrackNN.ogg files).  The data track is stored with full 2352-byte
raw sectors - sync pattern, header, and for Mode 2 an 8-byte subheader before
the 2048 bytes of user data - which is why tools expecting a plain 2048-byte
ISO refuse it.  This reads the sector format from the first sector and walks
the ISO 9660 directory tree from there.  Plain 2048-byte ISOs work too.

Usage:
  python tools/gogextract.py <game.gog> <output-dir>      extract everything
  python tools/gogextract.py <game.gog>                   list only

Files are written with the names and directory structure on the disc
(version suffix ";1" removed) and their recorded modification times.
Rock Ridge and Joliet are ignored: a 1995 DOS disc has neither, and the
primary descriptor is the one DOS itself reads.
"""
import calendar
import os
import struct
import sys

SYNC = b"\x00" + b"\xff" * 10 + b"\x00"


class Image:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.size = os.path.getsize(path)
        head = self.f.read(16)
        if head[:12] == SYNC and self.size % 2352 == 0:
            self.stride = 2352
            mode = head[15]
            # Mode 1: 16 bytes of sync+header.  Mode 2 Form 1: 8 more bytes
            # of subheader.  Form 2 sectors never carry ISO 9660 structures.
            self.offset = {1: 16, 2: 24}.get(mode)
            if self.offset is None:
                sys.exit("unsupported raw sector mode %d" % mode)
            self.kind = "raw 2352-byte sectors, Mode %d" % mode
        else:
            self.stride, self.offset = 2048, 0
            self.kind = "plain 2048-byte sectors"
        self.sectors = self.size // self.stride

    def read(self, lba, length):
        out = bytearray()
        while len(out) < length:
            self.f.seek(lba * self.stride + self.offset)
            out += self.f.read(min(2048, length - len(out)))
            lba += 1
        return bytes(out)


def record_time(rec):
    y, mo, d, h, mi, s, tz = struct.unpack_from("<6Bb", rec, 18)
    try:
        t = calendar.timegm((1900 + y, mo, d, h, mi, s, 0, 0, 0))
    except (ValueError, OverflowError):
        return None
    return t - tz * 15 * 60


def walk(img, lba, length, path=""):
    data = img.read(lba, length)
    pos = 0
    while pos < len(data):
        n = data[pos]
        if n == 0:                          # records never span sectors
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + n]
        pos += n
        ext = struct.unpack_from("<I", rec, 2)[0]
        size = struct.unpack_from("<I", rec, 10)[0]
        flags = rec[25]
        name = rec[33:33 + rec[32]]
        if name in (b"\x00", b"\x01"):     # "." and ".."
            continue
        name = name.decode("ascii", "replace").split(";")[0].rstrip(".")
        full = path + "/" + name if path else name
        if flags & 2:
            yield full, ext, size, record_time(rec), True
            yield from walk(img, ext, size, full)
        else:
            yield full, ext, size, record_time(rec), False


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__)
    img = Image(sys.argv[1])
    out = sys.argv[2] if len(sys.argv) == 3 else None
    pvd = img.read(16, 2048)
    if pvd[0] != 1 or pvd[1:6] != b"CD001":
        sys.exit("no ISO 9660 primary volume descriptor at sector 16")
    label = pvd[40:72].decode("ascii", "replace").strip()
    print("%s: %d sectors, %s, volume '%s'" % (sys.argv[1], img.sectors, img.kind, label))
    root = pvd[156:156 + 34]
    lba, length = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    for full, ext, size, mtime, is_dir in walk(img, lba, length):
        print("%s %10s  %s" % ("d" if is_dir else "-", "" if is_dir else size, full))
        if out is None:
            continue
        dest = os.path.join(out, *full.split("/"))
        if is_dir:
            os.makedirs(dest, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dest), exist_ok=True)
        with open(dest, "wb") as f:
            f.write(img.read(ext, size))
        if mtime is not None:
            os.utime(dest, (mtime, mtime))


if __name__ == "__main__":
    main()
