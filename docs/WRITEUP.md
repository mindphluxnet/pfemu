# Installing Pinball Fantasies without the DOS installer

Recovered by static analysis of `INSTALL.EXE` and emulation of its 8086
decompressor. No external references were consulted.

**Result:** all 25 files extracted from `FANTASY.ONE` / `FANTASY.TWO` and
verified against the CRCs stored in the archives — 25/25. Installed to
`FANTASY\` (3.72 MB). Extractor saved as `pfx.py`.

---

## 1. The container

`INSTALL.SYS` identifies the engine as **Knowledge Dynamics INSTALL 3.20.12**
(script by Stewart J. Gilray). `FANTASY.ONE` and `FANTASY.TWO` are chains of
records, each starting with the ASCII magic `RR`.

Record header, 41 bytes, little-endian:

| Offset | Size | Field                                        |
|--------|------|----------------------------------------------|
| `0x00` | 2    | Magic `"RR"`                                 |
| `0x03` | 2    | Header size — always `0x29`                  |
| `0x08` | 4    | Compressed size, **raw** bytes               |
| `0x0C` | 4    | Uncompressed size                            |
| `0x12` | 4    | CRC-16/CCITT-FALSE of the extracted file     |
| `0x18` | 2    | Compression technique — `11`                 |
| `0x1A` | 15   | Filename, NUL-padded                         |

The word at `0x03` is the header size, which pins the record stride:
`0x29 + compressed size` lands exactly on the next `RR`, 25 times across both
files, ending precisely at EOF.

## 2. The compression

Technique 11 is a number, not a format. Disassembling `INSTALL.EXE` (a plain,
unpacked Borland C++ binary; DGROUP at image offset `0x23830`) and following
the string `"internal error make_table"` back to its caller identified it as
LHA-derived. The parameters were read off the instructions:

| Address   | Routine            | Established                        |
|-----------|--------------------|------------------------------------|
| `0x18095` | `make_table`       | LHA static-Huffman table builder   |
| `0x12685` | `read_pt_len`      | NT=19, TBIT=5, i_special=3         |
| `0x12787` | `read_c_len`       | NC=510, CBIT=9                     |
| `0x124F5` | `decode_p`         | NP=14, PBIT=4                      |
| `0x128EB` | `decode`           | `and ax,0x1fff` → 8 KiB window     |
| `0x16CAC` | `fillbuf`/`getbits`| 16-bit MSB-first bit reader        |

Those constants are exactly LHA **`-lh5-`**. The stored checksum is
CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`), not LHA's own reflected
polynomial.

## 3. The wall

A faithful `-lh5-` decoder recovered 3 files perfectly. The other 22 either
died on a malformed Huffman table or decoded to the full correct length and
still failed their checksum. The split correlated perfectly with compressed
size:

- **under 4096 bytes** — `PINBALL.EXE` (558), `TIMER.BIN` (174),
  `NOSOUND.SDR` (1702) — all byte-perfect
- **over 4096 bytes** — the remaining 22, from `SETSOUND.EXE` (5176) to
  `INTRO.PRG` (160544) — not one intact

## 4. Emulating the original

To rule out my own decoder, I wrote a small 16-bit x86 interpreter (`emu.py`,
kept in scratch) and ran the *original* 8086 decompressor directly out of
`INSTALL.EXE`, feeding the compressed bytes in through a hook on the
installer's byte-reader at `0x1C5DA`.

It reproduced the verified file exactly, then produced output identical to mine
on every failing file — including hitting the installer's own `"Bad table"`
error on the same byte.

**The decompressor was not wrong. Something was altering the bytes before they
reached it.**

## 5. The interleaved CRC layer

The reader at `0x1C5DA` refills through `0x1C760`, which does more than read.
After each buffer fill it walks the data in fixed-size chunks, checks each one,
then `memmove`s the tail down to close a 2-byte gap per chunk.

The stored payload is **not** one continuous LZH stream. It is a run of
**4096-byte chunks, each ending in its own 2-byte CRC-16**, stripped by the
reader before the decompressor sees a bit:

```
in the file   [ 4094 bytes of stream |CRC] [ 1078 bytes |CRC]
                        |                        |
                        v                        v
into decoder  [ 4094 bytes .................. 1078 bytes ]   = 5172 bytes
```

The record's compressed size counts the raw bytes, checksums included — which
is exactly why the record chain parsed perfectly and gave no hint anything was
interleaved.

This explains the 4 KiB threshold. A stream shorter than one chunk carries its
checksum only at the very end, past everything the decoder needs, so it decodes
correctly by accident. The moment a stream crosses its first chunk boundary,
two checksum bytes land in the middle of the Huffman data.

Confirmation: **all 122 chunks across both archives checksum to zero.** The
archives were never damaged.

## 6. Verification

With the CRC layer stripped, all 25 files decode and match the CRCs in their
headers. Since a matching CRC only proves the installer's intent was
reproduced, the output was also checked against formats the container knows
nothing about:

| Artefact              | Check                                                   | Result       |
|-----------------------|---------------------------------------------------------|--------------|
| `SETSOUND.EXE`        | LZEXE 0.91 entry point lands on the header's `cs:ip`    | `0149:000e` ✓|
| Six `.MOD` files      | ProTracker `M.K.` magic at offset 1080                  | 6/6 ✓        |
| Five `.MOD` files     | `1084 + 1024·patterns + samples` = file size            | exact ✓      |
| MZ executables        | Header page count matches byte length                   | exact ✓      |
| `INTRO.PRG`           | Readable game strings recovered                         | ✓            |

Module titles recovered: `pinball2-table1`…`table4`, `Steelchambers2`,
`adrenaline`.

## 7. What was installed

`INSTALL.SYS` does nothing beyond placing files — no config files, no
`AUTOEXEC.BAT` edits, no registry. Copying the 25 extracted files into one
directory *is* the installation, and it is path-independent.

`FANTASY\` — 2 executables (`PINBALL.EXE`, `SETSOUND.EXE`), `INTRO.PRG`,
`TABLE1–4.PRG`, 6 `.MOD` music files, 11 `.SDR` sound drivers, `TIMER.BIN`.

Per the original script, `SETSOUND.EXE` picks the sound card, then `PINBALL`
starts the game.

## Caveat

This machine is AMD64, and 64-bit Windows ships no NTVDM, so Windows 11 cannot
launch a 16-bit real-mode binary at all. The extraction and installation are
complete and verified, but actually starting `PINBALL.EXE` still requires some
DOS environment. That is an OS limit, not an unfinished step.

`CRACK.COM` in this directory is the release's patch for the game's
manual-lookup check. It is also a DOS `.COM`; it was left unapplied.
