#!/usr/bin/env python3
"""
pfx.py -- extractor for the Knowledge Dynamics "INSTALL 3.20" archives
used by Pinball Fantasies (PC, 1993): FANTASY.ONE / FANTASY.TWO.

Container layout (reverse engineered from the data files):
  0x00  2   "RR" magic
  0x02  6   constant 01 29 00 60 2a 1c   (installer version / stamp)
  0x08  4   compressed size
  0x0C  4   uncompressed size
  0x10  2   0xFFFF
  0x12  4   CRC-16/CCITT-FALSE of the decompressed data (poly 0x1021, init 0xFFFF)
  0x16  2   0x0001
  0x18  2   0x000B  = compression technique 11
  0x1A  15  file name, NUL padded
  0x29  ..  compressed stream, "compressed size" bytes; next record follows

The word at 0x03 (0x0029) is the header size, which is how the record
stride was pinned down: 0x29 + csize lands exactly on the next "RR".

Technique 11 is LHA "-lh5-": 8 KiB sliding window, static Huffman,
NC=510/CBIT=9, NT=19/TBIT=5/special=3, NP=14/PBIT=4, 12-bit code lookup.
Confirmed by disassembling make_table / read_pt_len / read_c_len / decode
in INSTALL.EXE (code segments 0x1809 and 0x16ca).
"""
import struct, sys, os

DICBIT, DICSIZ = 13, 1 << 13
THRESHOLD = 3
NC, CBIT = 510, 9
NT, TBIT = 19, 5
NP, PBIT = 14, 4

# INSTALL's "updcrc": CRC-16/CCITT-FALSE, poly 0x1021, init 0xFFFF, MSB first.
CRCPOLY = 0x1021
_crctab = []
for _i in range(256):
    _r = _i << 8
    for _ in range(8):
        _r = ((_r << 1) ^ CRCPOLY) & 0xFFFF if _r & 0x8000 else (_r << 1) & 0xFFFF
    _crctab.append(_r)


def install_crc16(data):
    c = 0xFFFF
    for b in data:
        c = ((c << 8) & 0xFFFF) ^ _crctab[((c >> 8) ^ b) & 0xFF]
    return c


class BitIn:
    """LHA getbits()/fillbuf(): 16-bit MSB-first window, zero fill past EOF."""

    def __init__(self, data):
        self.d, self.p = data, 0
        self.bitbuf = 0
        self.subbitbuf = 0
        self.bitcount = 0
        self.fillbuf(16)

    def _getc(self):
        if self.p < len(self.d):
            b = self.d[self.p]
            self.p += 1
            return b
        return 0

    def fillbuf(self, n):
        self.bitbuf = 0 if n >= 16 else ((self.bitbuf << n) & 0xFFFF)
        while n > self.bitcount:
            n -= self.bitcount
            if n < 16:
                self.bitbuf = (self.bitbuf | (self.subbitbuf << n)) & 0xFFFF
            self.subbitbuf = self._getc()
            self.bitcount = 8
        self.bitcount -= n
        self.bitbuf = (self.bitbuf | (self.subbitbuf >> self.bitcount)) & 0xFFFF

    def getbits(self, n):
        v = (self.bitbuf >> (16 - n)) & 0xFFFF if n > 0 else 0
        self.fillbuf(n)
        return v

    def peek(self, n):
        return (self.bitbuf >> (16 - n)) & 0xFFFF


class Lh5:
    def __init__(self, bi):
        self.bi = bi
        self.left = [0] * (2 * NC - 1)
        self.right = [0] * (2 * NC - 1)
        self.c_len = bytearray(NC)
        self.pt_len = bytearray(max(NT, NP))
        self.c_table = [0] * 4096
        self.pt_table = [0] * 256
        self.blocksize = 0

    # ---- huf.c: make_table ------------------------------------------------
    def make_table(self, nchar, bitlen, tablebits, table):
        count = [0] * 17
        weight = [0] * 17
        start = [0] * 18
        avail = nchar

        for i in range(nchar):
            count[bitlen[i]] += 1

        total = 0
        for i in range(1, 17):
            start[i] = total
            total += count[i] << (16 - i)
        if total & 0xFFFF:
            raise ValueError("bad Huffman table (total=%#x)" % total)
        start[17] = total & 0xFFFF

        jutbits = 16 - tablebits
        for i in range(1, tablebits + 1):
            start[i] >>= jutbits
            weight[i] = 1 << (tablebits - i)
        for i in range(tablebits + 1, 17):
            weight[i] = 1 << (16 - i)

        i = start[tablebits + 1] >> jutbits
        if i != 0:
            for x in range(i, 1 << tablebits):
                table[x] = 0

        for ch in range(nchar):
            k = bitlen[ch]
            if k == 0:
                continue
            nextcode = start[k] + weight[k]
            if k <= tablebits:
                for x in range(start[k], min(nextcode, 1 << tablebits)):
                    table[x] = ch
            else:
                bitpos = start[k]
                idx = bitpos >> jutbits
                if table[idx] == 0:
                    self.left[avail] = 0
                    self.right[avail] = 0
                    table[idx] = avail
                    avail += 1
                node = table[idx]
                mask = 1 << (15 - tablebits)
                depth = k - tablebits
                while depth > 1:
                    if bitpos & mask:
                        if self.right[node] == 0:
                            self.left[avail] = 0
                            self.right[avail] = 0
                            self.right[node] = avail
                            avail += 1
                        node = self.right[node]
                    else:
                        if self.left[node] == 0:
                            self.left[avail] = 0
                            self.right[avail] = 0
                            self.left[node] = avail
                            avail += 1
                        node = self.left[node]
                    mask >>= 1
                    depth -= 1
                if bitpos & mask:
                    self.right[node] = ch
                else:
                    self.left[node] = ch
            start[k] = nextcode

    # ---- huf.c: read_pt_len -----------------------------------------------
    def read_pt_len(self, nn, nbit, i_special):
        bi = self.bi
        n = bi.getbits(nbit)
        if n == 0:
            c = bi.getbits(nbit)
            for i in range(nn):
                self.pt_len[i] = 0
            for i in range(256):
                self.pt_table[i] = c
            return
        i = 0
        while i < n:
            c = bi.bitbuf >> 13
            if c == 7:
                mask = 1 << 12
                while mask & bi.bitbuf:
                    mask >>= 1
                    c += 1
            bi.fillbuf(3 if c < 7 else c - 3)
            self.pt_len[i] = c
            i += 1
            if i == i_special:
                c = bi.getbits(2) - 1
                while c >= 0:
                    self.pt_len[i] = 0
                    i += 1
                    c -= 1
        while i < nn:
            self.pt_len[i] = 0
            i += 1
        self.make_table(nn, self.pt_len, 8, self.pt_table)

    # ---- huf.c: read_c_len ------------------------------------------------
    def read_c_len(self):
        bi = self.bi
        n = bi.getbits(CBIT)
        if n == 0:
            c = bi.getbits(CBIT)
            for i in range(NC):
                self.c_len[i] = 0
            for i in range(4096):
                self.c_table[i] = c
            return
        i = 0
        while i < n:
            c = self.pt_table[bi.peek(8)]
            if c >= NT:
                mask = 1 << 7
                while True:
                    c = self.right[c] if (bi.bitbuf & mask) else self.left[c]
                    mask >>= 1
                    if c < NT:
                        break
            bi.fillbuf(self.pt_len[c])
            if c <= 2:
                if c == 0:
                    c = 1
                elif c == 1:
                    c = bi.getbits(4) + 3
                else:
                    c = bi.getbits(CBIT) + 20
                while c > 0:
                    c -= 1
                    self.c_len[i] = 0
                    i += 1
            else:
                self.c_len[i] = c - 2
                i += 1
        while i < NC:
            self.c_len[i] = 0
            i += 1
        self.make_table(NC, self.c_len, 12, self.c_table)

    # ---- huf.c: decode_c / decode_p ---------------------------------------
    def decode_c(self):
        bi = self.bi
        if self.blocksize == 0:
            self.blocksize = bi.getbits(16)
            self.read_pt_len(NT, TBIT, 3)
            self.read_c_len()
            self.read_pt_len(NP, PBIT, -1)
        self.blocksize -= 1
        j = self.c_table[bi.peek(12)]
        if j >= NC:
            mask = 1 << 3
            while True:
                j = self.right[j] if (bi.bitbuf & mask) else self.left[j]
                mask >>= 1
                if j < NC:
                    break
        bi.fillbuf(self.c_len[j])
        return j

    def decode_p(self):
        bi = self.bi
        j = self.pt_table[bi.peek(8)]
        if j >= NP:
            mask = 1 << 7
            while True:
                j = self.right[j] if (bi.bitbuf & mask) else self.left[j]
                mask >>= 1
                if j < NP:
                    break
        bi.fillbuf(self.pt_len[j])
        if j != 0:
            j = (1 << (j - 1)) + bi.getbits(j - 1)
        return j


def lh5_decode(comp, origsize):
    bi = BitIn(comp)
    h = Lh5(bi)
    out = bytearray()
    win = bytearray(DICSIZ)
    r = 0
    while len(out) < origsize:
        c = h.decode_c()
        if c <= 255:
            win[r] = c
            out.append(c)
            r = (r + 1) & (DICSIZ - 1)
        else:
            length = c - 256 + THRESHOLD
            i = (r - h.decode_p() - 1) & (DICSIZ - 1)
            for _ in range(length):
                if len(out) >= origsize:
                    break
                b = win[i]
                win[r] = b
                out.append(b)
                i = (i + 1) & (DICSIZ - 1)
                r = (r + 1) & (DICSIZ - 1)
    return bytes(out)


CHUNK = 4096          # raw chunk size of the stored stream


def unchunk(raw, verify=True):
    """The stored payload is a series of 4096-byte chunks, each ending in a
    2-byte CRC-16/CCITT-FALSE (so the CRC over the whole chunk is 0).  The
    installer's buffered reader strips those 2 bytes per chunk before handing
    the bytes to the decompressor, so the logical LZH stream is the chunks
    with their CRCs removed.  `compressed size` counts the raw bytes."""
    out = bytearray()
    for k in range(0, len(raw), CHUNK):
        c = raw[k:k + CHUNK]
        if verify and install_crc16(c) != 0:
            raise ValueError("chunk CRC failure at raw offset %d" % k)
        out += c[:-2]
    return bytes(out)


def parse(path):
    d = open(path, 'rb').read()
    off = 0
    while off < len(d):
        if d[off:off + 2] != b'RR':
            raise ValueError("bad magic at %#x in %s" % (off, path))
        csize, usize = struct.unpack_from('<II', d, off + 8)
        crc = struct.unpack_from('<I', d, off + 0x12)[0]
        tech = struct.unpack_from('<H', d, off + 0x18)[0]
        name = d[off + 0x1A:off + 0x29].split(b'\x00')[0].decode('ascii')
        yield name, tech, csize, usize, crc, unchunk(d[off + 0x29:off + 0x29 + csize])
        off += 0x29 + csize


def main(argv):
    srcdir = argv[1] if len(argv) > 1 else '.'
    outdir = argv[2] if len(argv) > 2 else 'out'
    os.makedirs(outdir, exist_ok=True)
    ok = bad = 0
    for src in ('FANTASY.ONE', 'FANTASY.TWO'):
        print("== %s" % src)
        for name, tech, csize, usize, crc, data in parse(os.path.join(srcdir, src)):
            if tech != 0x0B:
                print("  %-13s SKIP: unknown technique %d" % (name, tech))
                bad += 1
                continue
            raw = lh5_decode(data, usize)
            got = install_crc16(raw)
            good = (len(raw) == usize and got == crc)
            print("  %-13s %7d -> %7d  crc %04x/%04x  %s"
                  % (name, csize, len(raw), got, crc, "OK" if good else "*** MISMATCH ***"))
            with open(os.path.join(outdir, name), 'wb') as f:
                f.write(raw)
            ok += good
            bad += (not good)
    print("\n%d file(s) verified, %d failed" % (ok, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
