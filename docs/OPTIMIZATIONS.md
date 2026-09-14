# pfemu performance optimizations

All changes are behavior-preserving: same guest-visible semantics, less host
work per emulated instruction. Nothing in the DOS/VGA/timing model (§5 of
`WRITEUP-PHASE2.md`) was redefined; the game-visible pulse trains, memory
map and interrupt timing are unchanged.

Bottleneck evidence: a 5 s headless run (`-secs 5 -speed 100`) showed
~315 M reads of port 3DAh — i.e. `vga_status1()` ran at ~63 M calls/s, each
doing `vga_timing()` + `fmod()` + a 16-entry histogram search. Memory
accessors (`mem_r8`, ~1 call per fetch and per data byte, plus 2–4× for
16/32-bit) and a double division in `emu_now()`/`emu_advance()` were the
other hot spots.

## 1. Time base: multiply by the reciprocal (`src/dev.c:11,17,27`, `src/main.c:9,233`)

`emu_advance()` and `emu_now()` divided by `emu_ips` on every call.
`emu_now()` runs on every polled-register read (3DAh, PIT, port 61h), so the
division executed tens of millions of times per second.

- Added `emu_inv_ips = 1.0 / emu_ips`, refreshed once in `main()` after
  argument parsing (`-ips` is startup-only, so no staleness risk).
- Hot path is now one multiply: `emu_time + (cycles - last) * emu_inv_ips`.
- `dev_next_deadline()` (`src/dev.c:420`) still multiplies by `emu_ips`
  (already a multiply, untouched).

Correctness: identical result up to one-ulp FP rounding; all downstream
uses are threshold compares, unaffected.

## 2. CRT status: cached geometry, no `fmod()`, cheap histogram (`src/vga.c`, `src/dev.c:228-270`)

`vga_status1()` is the hottest function in the emulator (see above).

- `vga_timing_cached()` (`src/vga.c:436`): caches frame period, inverse
  period, totals and blanking fraction; recomputed only when a register it
  derives from changes. Invalidation points: misc output (`src/vga.c:199`),
  sequencer (`src/vga.c:201`), CRTC (`src/vga.c:214`), BIOS mode set via
  `apply_regs()` (`src/vga.c:346`). Registers change on mode switches only,
  so ~63 M recomputes/s become ~63 M cache hits/s.
- Phase math uses multiply + truncate instead of `fmod()`/`floor()`:
  `q = now * inv_per; q -= (int64_t)q; line = q * vtotal; frac = line - (int)line`.
  Same for the `-oldtiming` branch (`70.086 Hz` constant). No libm calls left
  on this path. `now` is always ≥ 0, so truncation equals `floor()`.
- `st1_note()` (`src/dev.c:204`): was a 16-entry linear search with aging on
  every read; now direct-mapped (`(lin >> 2) & 15`), one compare per read.
  Collisions overwrite a slot — acceptable for a profiling histogram; the
  exit report format is unchanged.

Correctness: the bit-0/bit-3 pulse trains are mathematically the same modulo
FP rounding far below the half-line thresholds the driver's PLL depends on
(§5.8c: strictly-monotone staircase preserved). vsync-edge counting and the
`st1_calls/bit0/bit3` counters are untouched.

## 3. PIT counters without `fmod()` (`src/dev.c:100-155,334`)

`pit_count()` had the same `fmod(now, per)/per` shape on the PIT-poll path.

- Each channel caches `inv_per = PIT_HZ / reload` (refreshed in `pit_write()`,
  initialized in `pit_init()`; reload 0 = 65536 as before).
- `pit_count()` and the port-61h timer-2 bit (`src/dev.c:334`) use
  multiply + truncate. One division per reload write replaces one
  `fmod()` + one division per read.

## 4. Memory access (`src/cpu.c:44-80`, `src/vga.c:488-522`)

- `src/cpu.c`: new `cpu_ld8/16/32`, `cpu_st8/16/32` (`src/cpu.c:44`) —
  same semantics as `mem_r8/w8` (A20 + 16 MB wrap, VGA dispatch, ROM
  write-ignore), but `static inline` in the interpreter's TU, so fetch,
  ModR/M, stack, string and far-pointer paths pay no cross-TU call.
  16/32-bit forms do one range check instead of 2–4 nested `mem_r8` calls.
  All `mem_*` uses inside `cpu.c` were switched over (fetch at `src/cpu.c:82`,
  stack, `rdE`/`wrE` at `src/cpu.c:140`, string ops, `0xA0`–`0xA3`, `0xC4`,
  `0xC5`, `0xD7`, far `call`/`jmp`, IVT fetch in `cpu_interrupt`).
  Fetch keeps the original non-wrapping `cs_base + eip` semantics
  (bug-compatible at the 64 KiB segment edge).
- `src/vga.c`: `mem_r16/r32`, `mem_w16/w32` now do a single range check with
  a direct `ram[]` fast path; the slow path (VGA window for reads,
  VGA-or-ROM window for writes) falls back to the byte helpers. Old code
  also treated `VGA_LO-1`/`VGA_LO-3` sloppily; the new `a + 1u >= VGA_LO`
  form is exact and overflow-safe (`a` ≤ `0xFFFFFF`).
- Other TUs (`bios.c`, `dos.c`, `sound.c`, `dev.c`) keep calling `mem_*`
  (cold paths: device init, file/IVT/DTA handling, one byte per DMA sample).

## 5. REP MOVS/STOS bulk path (`src/cpu.c:268-316`)

`strop()` looped per byte with full helper + flag/register overhead per
iteration. Added a fast path for `REP MOVS`/`REP STOS`, forward (`DF=0`),
`cnt >= 16`, entirely inside plain RAM (`< 0xA0000`, no segment wrap):

- MOVS → `memmove()`, byte STOS → `memset()`, word/dword STOS → tight fill loop.
- `ESI`/`EDI`/`ECX` updates and `cpu.cycles += cnt` match the slow loop
  exactly (`ECX` → 0, 16-bit pointer wrap preserved via `(uint16_t)` cast).
- Anything else (backwards, VGA/ROM touch, `CMPS`/`SCAS` early-exit,
  `LODS`, `INS`/`OUTS`) keeps the original loop. VRAM-bound copies
  (chain-4/planar, latches) therefore still go through `vga_mem_w()`.

## 6. Bigger main-loop batch (`src/main.c:272-287`, `src/dev.c:420-426`)

Batch 64 → 256 instructions; `guard` 40000 → 10000 (same 2.56 M-instruction
cap per present/ pump iteration, so UI latency is unchanged). The
`dev_next_deadline()` clamp is unchanged, so IRQ0 still lands on the exact
instruction; worst-case IRQ latency (~43 µs at 6 MIPS) is orders of magnitude
below anything observable (PIT tick ≈ 55 ms). The `+64` no-deadline fallback
in `dev_next_deadline()` became `+256` to match.

## 7. Build: whole-program optimization (`build.bat:3`)

`/GL` + `/link /LTCG` added to the existing `/O2` line. Lets the optimizer
inline across TUs (e.g. `vga_mem_r/w`, `io_r8/w8`, `pic_*` into callers).
Deliberately *not* enabled: `/fp:fast` (would loosen the FP comparisons the
PLL lock depends on), `/arch:AVX2` (portability).

## What was intentionally left alone

- CPU dispatch itself (giant `switch` in `cpu_step`): a threaded/dynarec
  core would be faster but is a rewrite with correctness risk; the changes
  above remove the overhead *around* dispatch instead.
- `vga_render()`: 60 Hz host-side work, negligible next to 6 MIPS of
  interpretation; palette rebuild per frame is trivial.
- `sb_tick()` audio path: already early-outs when silent; per-sample cost
  (~12 kHz) is inherent.

## Verification

- `build.bat` compiles clean under MSVC 14.29 (`/W3`, no warnings).
- No gameplay run was performed from this session (no GUI launched, per
  request). Suggested checks before merging:
  - `pfemu.exe -d game -secs 5 -speed 100` exits by itself and still reports
    `CRT refresh ≈ 59.71 Hz`, `page flips`, `mode=12h 640x240`.
  - PLL lock trace (`-pll N`) still settles near reload 19921 as in §5.8.
  - Screenshot diff of intro/menu/table frames vs. pre-change build.
  - Higher `emu_time`/instruction count for the same wall seconds = speedup.

## 8. Setup shortcut: `-setup` (`src/main.c:209`)

Boots `SETSOUND.EXE` instead of `PINBALL.EXE` (alias for
`-p SETSOUND.EXE`). The game ships with `SOUND.CFG` set to `NOSOUND.SDR`,
so this is the quick path to sound: pick SoundBlaster (base port 220h,
IRQ 7) and the utility writes `SOUND.CFG`. The write lands in
`PFEMU-STATE/` through the DOS write overlay (`src/dos.c`), so installed
files stay pristine, and the game picks the driver up on its next boot.

Deliberately *not* a flag that forges `SOUND.CFG` bytes directly.
Static analysis of `SBLASTER.SDR` (unpacked MZ, disassembled the config
parse at image `0x1870`: open, `lseek` to `0x0E`/`0x11`/`0x14`, one answer
byte each through `& 7` lookup tables for base port, IRQ and quality)
shows the driver only reads 3 of the 25 bytes; the remaining gap bytes are
written by `SETSOUND.EXE`, which is LZEXE-packed, so their exact content
was not established. Running the real utility keeps the one verified
code path (§9: menu, keyboard and `SOUND.CFG` write all work under pfemu)
instead of guessing at its output format.

## 9. Startup launcher + sound toggle (`src/launch.c`, `src/main.c`, `src/pfemu.h`)

`pfemu.exe` used to boot straight into Pinball Fantasies. It now shows a
small Win32 picker first (same C/MSVC toolchain, no new dependencies):

- Radio buttons: Pinball Fantasies (`FANTASY/PINBALL.EXE`), Pinball Dreams
  (`DREAMS/PD.EXE` — booted directly; `DREAMS.COM` is only a BAT2EXEC memory
  gate via `CHKMEM`, meaningless under emulation), Pinball Illusions
  (listed but disabled — boot support is not there yet, see §10).
- Sound checkbox (Fantasies only; greyed out for Dreams, whose sound is
  chosen in its in-game F1/F2 menu). On Launch it calls `write_sound_cfg()`:
  on = 25-byte `SBLASTER.SDR` config (name + port index 1 → 220h, IRQ index
  3 → IRQ 7, quality 0; gap bytes zero, unread by the driver), off = the
  known-good 16-byte `NOSOUND.SDR` config. The file goes to
  `<game>/PFEMU-STATE/SOUND.CFG` through the same overlay idea as `src/dos.c`,
  so installed files stay pristine. The checkbox initialises from the
  effective config (`read_sound_is_sb()`: overlay first, then installed file;
  SBPRO/SB16/SB20 count as "on").
- Launch validates the game directory exists, Quit/close exits without booting.

Skip rules (`src/main.c`): the dialog is bypassed by `-nolauncher` (new),
by an explicit `-p`/`-setup`, and by `-secs` (headless/benchmark runs stay
scriptable). In picker mode `-d` is ignored — the directory comes from the
selected game. Closing the window quits instead of booting (behavior change
vs. the old double-click boots).

## 10. Multi-game status (exploration, no core changes yet)

- Pinball Dreams (`DREAMS/`): 16-bit real-mode (`PD.EXE` + BAT2EXEC launcher),
  VGA/PIT/speaker hardware already covered by the core; built-in Zero Hour /
  Miles sound (no `.SDR` model), `.RMC` MIDI + `.MOD` music, manual-lookup
  protection. Bring-up recipe is the Fantasies one (boot under `-dosdbg` /
  `-xring`, fix, patch protection in memory).
- Pinball Illusions (`ILLUSION/illusion.exe` + `start.bat`): the 3.7 MB file
  is a real-mode loader (ANSI intro, XMS/VCPI/DPMI detection) around an
  embedded NLZW resource archive holding the 32-bit protected-mode game
  (`illusion.000` + pMAX `illusion.386` driver, CauseWay-style `INT 90h`
  services). Sound reuses the `.SDR`/`SOUND.CFG` family, so the §9 toggle
  carries over. Still needed: full pmode CPU + DPMI-server surface in the
  core; first step is a stub-observability run of the real-mode part under
  the current core to confirm the archive resolves internally.

## 11. Launcher follow-ups: no console window; INT 21h AH=29h (Dreams)

- Double-clicking opened a console ("shell") window next to the UI because
  the link defaulted to `/SUBSYSTEM:CONSOLE`. `build.bat` now links
  `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` (keeping `main()`), and
  `main()` reattaches to the invoker's console via
  `AttachConsole(ATTACH_PARENT_PROCESS)` so CLI output (`-secs` stats,
  traces) still works when run from a terminal.
- `INT 21h AH=29h` (parse filename into FCB) implemented in `src/dos.c`:
  `DREAMS.COM` is a BAT2EXEC-compiled batch that parses every program name
  through `AX=2903` right before `EXEC` (`src` offsets `0x314`/`0x31C` → EXEC
  `0x336` → exit-code `0x351`), and the old "unimplemented" return
  (`AX=1`, `CF=1`) broke that path. Implemented per RBIL (separator set,
  drive/name/ext handling, `*`→`?` fill, lowercase→uppercase, `AL` =
  0 plain / 1 wildcards / FF bad drive, `SI` to terminator).
  (Kept: harmless and correct, but no longer on the boot path since the
  launcher now starts `PD.EXE` directly.)
- Dreams boot itself is still unverified — pending a user test run. `PD.EXE`'s
  own DOS usage was statically scanned and is fully covered (`02h 09h 0Ah`
  console, `25h/35h` vectors, `3Ch-42h` file I/O, `4Ch` exit; no EXEC/FCB/
  exotic calls), and its entry (`cs:ip=0:0` = image start) is sane init code.

## 12. Dreams: missing `\install.sys` caused the silent exit

Symptom: black window closing by itself; exit stats pristine (text mode 03h,
untouched VRAM/PIT/vectors). A `-t -dosdbg` trace showed the real story —
not a hang at all: `PD.EXE` hooks `INT 9`, probes the keyboard, opens
`\install.sys`, gets "open FAILED", restores the vector and takes
`INT 21h 4C00`. (The vsync-wait loop seen in an earlier `-xring` was normal
frame pacing sampled at timeout, a red herring.)

`\install.sys` is installer-written personalization: the literals
`Serial No:` / `User Name:` sit next to the filename in the binary, the
game loads the whole file first thing (loader at image `0x15C7`: open →
size check <256 KB → chunked read → close) and bails through a silent
cleanup+exit (`0x1B13`: mode 3, `4C00`) when the load fails. Downloads
typically lack the file.

Fix (`src/launch.c:ensure_install_sys()`): on Dreams launch, the GUI's User
name / Serial fields (enabled for Dreams only, capped at 20/8 chars like
the installer) are written as the exact 29-byte layout into the overlay:
byte 0 = install count (1), bytes 1-20 = username NUL-padded, bytes 21-28
= serial — then every byte `xor 0FFh`, matching `INSTALL.COM`'s write path
(`xor al,0FFh` loop at `0x20A5`, decoded on read at `0x20B3`). A real
installer file always wins; anything else (missing, or a stale plaintext
guess, detected by decoding byte 0 and expecting install count 1-3) is
rewritten. (The file stays obfuscated on disk — hand-editing means
NOT-encoding; use the GUI fields.)
The GUI fields initialise from the existing file (`read_install_sys()`:
decode, printable-only, widths enforced), so relaunching keeps the user's
name/serial instead of resetting them.

## 13. Dreams post-intro stall (in diagnosis)

Symptom: intro screens (`company`→`spider`→`presents`→`digital`→`pinball.vga`,
all loading fine per `-dosdbg`) play, then black screen; the game never opens
the next file. Exit state: mode 13h, dark palette, spinning in the CX-counted
vsync wait (`0071:2A66`, helper `0x2A5E`), page flips 0. Ruled out: sound
choice (No Sound hangs identically — the fancy driver-flag wait at
`0x2A76` runs regardless), DOS buffered input (both `B4 0Ah` hits are `mul
ah` arithmetic; the manual-word entry uses the game's own keyboard hook),
missing files up to `pinball.vga`. The wait protocol itself is understood
(simple direct-`3DAh` waits + driver-flag `0xB`/`0xA` pair around a cached
byte filled by the sound ISR's `in al,3DAh` / `or [0x1C26],al`, with `INT
90h`/CauseWay-style far calls into segment `0x2146`). Next: identify the
stuck call site — the exit dump now prints `ss:sp` plus 16 stack words, so a
hang inside a helper still names its near-call return address.
(`-trap` exists but disarms on first hit, i.e. early init, so it can't catch
a late stall; the stack dump fills that gap.)

## 14. Dreams post-intro exit: manual protection (fixed)

The post-intro black screen ended in the silent cleanup+exit (`mov ax,3;
INT 10h; 4C00` at image `0x1B13`), reached from the entry init's failure
branch. The xring named it: PD's manual-lookup protection (routine at image
`0x6FB3`: prints page/line/word prompts, reads the word via `INT 21h
AH=0Ah`, uppercases and checksums it, `JE` to pass at image+`0x7020`,
retries then silent exit on failure). Two fixes, same pattern as the
Fantasies §5.13 protection:

- `INT 21h AH=0Ah` (buffered line input) is now properly implemented in
  `src/dos.c` (was an instant-empty stub, which guaranteed failure): reads
  the BIOS type-ahead queue with DOS echo, Backspace erase, extended-key
  `0x00`+scancode pairs; blocks exactly like `INT 16h AH=00` when empty
  (stub rewind, idle, resume on IRQ) with partial input kept in the guest
  buffer (`oa_active`, reset on `EXEC`). Needs the exported
  `bios_kbuf_get/peek` (`src/bios.c`, `src/pfemu.h`).
- The check itself is patched in the loaded image (30-byte signature at
  image+`0x7004`, two one-byte fixes: length-`JNE` retargeted fail→pass,
  checksum-`JE`→`JMP`), memory-only, `-nopatch` restores the original
  prompt-and-answer behavior (which now works, thanks to the `0Ah`
  implementation, for anyone holding the manual). The two-gate form
  matters: checksumming alone still retries on a wrong-length word, which
  is exactly the observed three-prompts-then-quit.
- Verified by playing: with both gates bypassed the game proceeds through
  the manual check into the menu and the tables are playable. Remaining
  known gaps for Dreams: no audio (PD uses MPU-401 MIDI/speaker paths, never
  touches the SB ports — silent until that audio work lands) and the
  in-game sound selection, which is cosmetic until then.

## 15. Fantasies "slower" report: measured, not regressed

A windowless harness (fixed instruction budget, same batch/deadline/IRQ
discipline as the main loop, no GUI) compared the pre-optimization core
against this one, best-of-3, in `FANTASY/` with scripted keys into table
play where noted:

| workload | base | this tree |
|---|---|---|
| intro, NOSOUND, batch 64 | 33.0 MIPS | 44.8 MIPS |
| intro, NOSOUND, batch 256 | — | 45.8 MIPS |
| intro, SoundBlaster, batch 64 | — | 35.4 MIPS |
| gameplay (table, scripted keys), SB, batch 256 | 22.0 MIPS | 31.5 MIPS |
| gameplay + `vga_render` per frame, SB | 19.5 MIPS | 27.1 MIPS |

So the core is **36–43% faster** everywhere, including the exact phase
complained about, and the render path shows no pathology (same ~12%
proportion both sides). Timing is equivalent too: the driver's PLL
calibration traces are bit-identical (`-pll` sequences match to the
microsecond), and table-phase tick counts agree within normal lock
variation (~58 Hz both).

Two real costs that are *not* regressions, but explain the feel: enabling
sound spends ~23% of cycles in the driver's in-guest MOD mixer plus its
DMA/3DA sync polling (NOSOUND runs never paid it), and Fantasies is simply
heavier per frame than Dreams (mode-X planar blits + split screen +
mixer — all guest work). Audio stays perfect because it is SB-clocked while
graphics are game-logic-paced. If graphics still feel slow on a given
machine, the remaining suspect is the host present path (`StretchDIBits`
each 1/60 s, identical code both builds): test whether sluggishness scales
with window size — bigger window slower means GDI-bound, in which case the
fix is presenting less (frame skip / dirty-only present), not emulating
faster.
Pending user test: name/serial should now display correctly and boot
should proceed into video/sound/menu (manual protection and sound-card
selection expected next).
