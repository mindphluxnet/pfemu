# Floppy archive format and extractor

How the original floppy installer stores its files, and how `tools/pfx.py`
recovers them. Derived from static analysis of `INSTALL.EXE` and by running
its 8086 decompressor in a small interpreter. No outside references used.

Result: all 25 files from `FANTASY.ONE` / `FANTASY.TWO` extracted and verified
against the CRCs in the archives, 25/25.

## Container

`INSTALL.SYS` identifies the engine as Knowledge Dynamics INSTALL 3.20.12.
`FANTASY.ONE` and `FANTASY.TWO` are chains of records starting with the ASCII
magic `RR`.

Record header, 41 bytes, little-endian:

| Offset | Size | Field |
|---|---|---|
| `0x00` | 2 | Magic `"RR"` |
| `0x03` | 2 | Header size, always `0x29` |
| `0x08` | 4 | Compressed size, raw bytes on disk |
| `0x0C` | 4 | Uncompressed size |
| `0x12` | 4 | CRC-16/CCITT-FALSE of the extracted file |
| `0x18` | 2 | Compression technique, `11` |
| `0x1A` | 15 | Filename, NUL-padded |

The header size pins the record stride: `0x29 + compressed size` lands on the
next `RR`, 25 times across both files, ending at EOF.

## Compression

Technique 11 is LHA `-lh5-`. Following the `"internal error make_table"`
string in `INSTALL.EXE` (an unpacked Borland C++ binary) leads to the table
builder and its parameters: 19/5/3 prefix lengths, 510/9 literal lengths,
14/4 distances, 8 KiB window, 16-bit MSB-first bit reader. The stored checksum
is CRC-16/CCITT-FALSE (poly `0x1021`, init `0xFFFF`), not LHA's own
polynomial.

## The interleaved CRC layer

A plain `-lh5-` decoder recovered 3 files and failed the other 22 - always on
files over 4096 bytes. Running the original 8086 decompressor in an
interpreter reproduced the same failures on the same bytes, so the decoder
wasn't at fault; something altered the bytes before they reached it.

The installer's byte reader (`0x1C5DA`, refilling through `0x1C760`) strips a
2-byte CRC-16 after every 4096-byte chunk before the decompressor sees it:

```
on disk:     [ 4094 bytes of stream | CRC ] [ 1078 bytes | CRC ] ...
to decoder:  [ 4094 bytes ................... 1078 bytes ]  (contiguous)
```

The record's compressed size counts raw bytes including checksums, so the
record chain parses cleanly and gives no hint anything is interleaved. Streams
shorter than one chunk carry their checksum past the end of the data the
decoder needs, which is why the 3 smallest files decoded by accident. All 122
chunks across both archives verify, so the archives were never damaged.

## Verification

With the CRC layer stripped, all 25 files decode and match their header CRCs.
Since a matching CRC only proves the installer's intent was reproduced, the
output was also checked against formats the container knows nothing about:

| Check | Result |
|---|---|
| `SETSOUND.EXE` LZEXE 0.91 entry lands on the header's `cs:ip` | `0149:000e` |
| Six `.MOD` files carry ProTracker `M.K.` magic at offset 1080 | 6/6 |
| Five `.MOD` files satisfy `1084 + 1024*patterns + samples = size` | exact |
| MZ header page counts match byte lengths | exact |
| `INTRO.PRG` yields readable game strings | yes |

Module titles: `pinball2-table1`...`table4`, `Steelchambers2`, `adrenaline`.

## Installed layout

`INSTALL.SYS` only places files - no config, no `AUTOEXEC.BAT` edits. Copying
the 25 files into one directory *is* the install, and it is path-independent:
2 executables (`PINBALL.EXE`, `SETSOUND.EXE`), `INTRO.PRG`, `TABLE1-4.PRG`,
6 `.MOD` music files, 11 `.SDR` sound drivers, `TIMER.BIN`. Per the original
script, `SETSOUND.EXE` picks the sound card, then `PINBALL` starts the game.
