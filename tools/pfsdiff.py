"""Compare two pfemu .pfs snapshots section by section.

Format: magic "PFEMU-SNAP 1\n", then tag[8] + u32le len + payload, ending
with a HASH section. Reports which sections differ and, for the big opaque
ones, where inside them.
"""
import sys, struct

MAGIC = b"PFEMU-SNAP 1\n"

def parse(path):
    d = open(path, 'rb').read()
    assert d.startswith(MAGIC), f"{path}: bad magic"
    pos = len(MAGIC)
    secs = []
    while pos + 12 <= len(d):
        tag = d[pos:pos+8]
        ln, = struct.unpack('<I', d[pos+8:pos+12])
        pos += 12
        secs.append((tag.decode('ascii', 'replace'), d[pos:pos+ln]))
        if tag == b'HASH    ':
            break
        pos += ln
    return secs

def runs(a, b, merge=16):
    """Byte ranges where a and b differ, merging gaps smaller than `merge`."""
    out = []
    n = max(len(a), len(b))
    i = 0
    while i < n:
        if a[i:i+1] != b[i:i+1]:
            j = i
            gap = 0
            k = i
            while k < n:
                if a[k:k+1] != b[k:k+1]:
                    j = k
                    gap = 0
                else:
                    gap += 1
                    if gap > merge:
                        break
                k += 1
            out.append((i, j))
            i = k
        i += 1
    return out

def main(pa, pb):
    A, B = parse(pa), parse(pb)
    ta = [t for t, _ in A]
    tb = [t for t, _ in B]
    if ta != tb:
        print("section layout differs:", ta, tb)
        return
    print(f"{'section':10} {'bytes':>9}  status")
    print("-" * 52)
    for (tag, sa), (_, sb) in zip(A, B):
        if sa == sb:
            print(f"{tag:10} {len(sa):9}  same")
            continue
        r = runs(sa, sb)
        total = sum(1 for x, y in zip(sa, sb) if x != y)
        print(f"{tag:10} {len(sa):9}  DIFFERS  ({total} bytes, {len(r)} runs)")
        for off, end in r[:12]:
            ha = sa[off:end+1][:16].hex()
            hb = sb[off:end+1][:16].hex()
            print(f"    +0x{off:06x}..0x{end:06x}  a={ha}  b={hb}")
        if len(r) > 12:
            print(f"    ... and {len(r)-12} more ranges")

if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
