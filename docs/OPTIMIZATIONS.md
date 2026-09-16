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
original Fantasies §5.13 protection (since superseded by a DOS-layer flag
forge - WRITEUP-PHASE2.md §5.13.1 - once it turned out Fantasies' own check
could be defeated without touching the loaded image at all):

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

Follow-ups, all measured: driver quality index costs 25.0–30.5 MIPS across
Q0–Q4 (Q3 fastest; default stays Q0, which is ear-verified as
original-sounding — switching defaults on perf grounds alone would trade
fidelity for ~15%). Batch 256 vs 64 is neutral-to-positive. The table-phase
game tick agrees within normal PLL lock variation (~58 Hz both), and the
boot-phase PLL traces are bit-identical, so pacing is unchanged.

## 16. Present timing: vsync-locking tried and reverted (`src/main.c`)

Presents ran on a wall 1/60 s timer while the game renders at 59.71 Hz, so
an experiment sampled one host frame per emulated retrace (`vsync_edges`)
for exact 1:1 mapping. Result: ball flicker got *worse* (constant instead
of occasional), scrolling unchanged — reverted to the wall timer, verified
byte-identical. Mechanism: the game's frame contains an erase-redraw
hazard for the ball; the drifting wall phase only lands in it occasionally,
while the locked phase parked inside it permanently. Lesson: phase-locked
presentation is only safe once the game's draw/flip timing is measured, not
assumed. The `vga_dirty` consume/clear went back with it (it was
write-only before and nobody reads it).

## 17. Present-phase instruments: `-flipdbg`, `-vscan N` (`src/vga.c`)

To place presents correctly, first measure where flips and draws land in
the emulated frame: `-flipdbg` logs every CRTC start-address write with
emulated time, scan-line phase and caller; `-vscan N` hashes VRAM in 64
4 KB chunks every N instructions and logs time, phase and change magnitude
(1–2 chunks = sprite-scale, dozens = blit/fill), auto-disabling after
100k events. Both off by default (one branch per batch when off). Also:
stderr is now only reattached to the parent console when it isn't
redirected, so `2>file` captures logs from the windowed binary.

## 19. Smooth scrolling via start-address interpolation (`src/vga.c`)

The §18 timeline settled it: the engine re-asserts the start address every
other frame (30 Hz flip cadence — authentic, also on period hardware), which
reads as stepping. Since gameplay itself is off-limits but presentation may
be improved, presents now interpolate: `vga_render` uses a host-side blend
of the last two published start positions by emulated time instead of the
raw register. No game state changes — physics, logic and timing are
untouched; only displayed rows shift smoothly between the game's own
positions. Details: hi/lo register pairs arriving microseconds apart extend
one flip event instead of starting a new one (no torn origins); jumps over
half the address space are treated as wraps; history resets on mode set;
`-nosmooth` restores raw sampling. DMD text steps are deliberately *not*
smoothed — those are game-redrawn content pacing, i.e. gameplay, verified
game-driven by the mask timeline (writes appear as the game makes them, not
pipeline-batched). The scan
also logs the 64-bit changed-chunk mask (`m=`), which localizes writes:
low chunks are the split-screen dot-matrix region, higher chunks the
scrolling playfield, so their timelines separate game-driven DMD updates
from playfield draws.

## 18. Flip/vscan timeline verdict: duplicate suppression, not phase lock

Measured with §17 (`-flipdbg -vscan 8192`, table play): flips land every
~33 ms at scan line ~228 (mid-frame), re-asserting the same start when not
scrolling and stepping it down while the camera follows the ball — healthy
engine behavior, all from table code. VRAM mutates in sprite-scale chunks
at *every* frame phase with no quiet window: background scrolls via page
flips while sprites erase/redraw directly, so the ball hazard is structural
and authentic on real hardware too (phase-dependent occasional flicker).
Consequences: no sampling phase avoids the gap (the failed §16 experiment
parked inside it); wall drift is load-bearing. The one real fix available
without changing game code is suppressing duplicate presents — the 60 Hz
wall timer re-showed every ~204th frame against the 59.71 Hz game — so the
present block is now gated on the emulated frame index
(`emu_time / frame period`, via the cached timing) advancing. Drops only
occur under CPU starvation, which headroom prevents; drift (and its
authentic occasional flicker) is preserved.

## 20. DMD step-cadence logger: `-dmd` (`src/vga.c`)

Follow-up to §19: the DMD panel ignores the CRTC start address (the
split-screen region below line compare renders from fixed VRAM,
`vga.c` split branch), and its text scrolls via game VRAM redraws — so
viewport interpolation structurally cannot change it, and re-timing those
redraws would be altering gameplay, which is off-limits. What remains
legitimate is measuring: `-dmd` watches VRAM chunks 44-49 (DMD writes
showed up as mask `0000C00000000000` in `dmd.log`) every 4096 instructions
and logs `[dmd] t=... dt=... n=...`, where `dt` is the time since the
previous DMD-region change — i.e. the text-step cadence. Samples touching
more than 2 chunks are bulk fills (loader, fades), not text steps, and
skip the 20k-event budget. Headless probe: 120M instructions, 1166 frames,
155 events all in emu-sec 0-1 (boot sprite upload), then silence through
the intro — so the budget survives boot easily. To use: run with `-dmd`,
trigger scrolling DMD text, share the log; `dt` then decides whether the
choppiness is slow game pacing (design, leave alone) or something
pathological (port defect, fair game). If scrolling text produces no
`[dmd]` lines at all, the chunk premise is wrong and we revisit.

Verdict (user-captured `dmd2.log`, ~296 emu-sec, 1507 events): during
active scrolls `dt` clusters at 0.033-0.034 s — 79 of 104 step events in
the t=280-297 window — i.e. exactly every 2nd emulated frame at 59.71 Hz
(2/59.71 = 0.0335 s), with only occasional single skipped steps (0.067 s).
That rock-steady, frame-phase-locked cadence is the signature of
intentional timer-driven game pacing, not a port defect (a stalled or
broken mechanism would drift or jitter). It matches the engine's own
30 Hz flip cadence: the game logic ticks at half frame rate throughout.
Conclusion: DMD text choppiness is authentic behavior in the same class
as the 30 Hz page flips — smoothing it would mean synthesizing glyph
positions the game never drew, i.e. altering gameplay presentation, which
is off-limits. Left alone; no code change.

## 21. Table-select palette flash: majority-duration AR14 bank (`src/vga.c`)

Symptom: on the table-select menu, fullscreen red→bright-green flashes,
far more often than every few seconds; the hi-scores/credits screens are
stable.

Mechanism: the menu flips the VGA Color Select bank (AR14, 0↔1) every
30 Hz game tick around a small glyph redraw — 2,458 toggles over ~100
emu-sec measured headless, in strict ~7 ms / ~33 ms pairs (22% bank-1
duty), all from the two menu-tick routines, CLI-protected and balanced.
The window is meant to hide inside vertical blanking; at 6 MIPS it lands
mid-frame instead, and presents sampling the instantaneous bank alias the
transient to a fullscreen strobe. The P54S/Color-Select mapping itself was
rechecked against the FreeVGA spec (P54S=1: DAC[5:4] from AR14[1:0],
DAC[7:6] from AR14[3:2]) — rendering is faithful, so the defect is in
presentation sampling, not palette decoding.

Credit where due: the mechanism was pinned down with the reconstructed
MS-DOS port source (`historicalsource/pinballfantasies`, `INTRO.ASM`) —
`julius` ("Ceasar sätter en palett!!") loading both 16-palettes into DAC
0–15/16–31, `CHANGE16PAL` with "set 2 palette modes (on rasterint)", and
the `dumretf` (VBLANK, bank 1) / `creatretf` (RASTERINT, bank 0) driver
callbacks ordered via INT 66h. It is a third-party reconstruction used as
reference, not the original code — but the port traffic it predicts
(AR14 pairs from the tick routines, dual-bank DAC contents with entry 8
red vs green) matches the emulation trace exactly. Filed for future
menu/raster work under §7 of WRITEUP-PHASE2.md.

Fix, presentation-only, no game state touched: `vga_io_w` keeps a
timestamped history of the last 16 AR14 changes, and the planar renderer
paints the frame with whichever bank covered the majority of the last
*complete* frame. Completeness matters: presents fire mid-frame, and a
switch window straddling the frame start otherwise reads as a transient
majority (first version of this fix still flashed for exactly that
reason). No switches in a frame means no behavior change at all, so the
tables (256-colour), text mode and the single-bank hi-scores page are
untouched by construction; history resets on mode set.

Verification: 28 consecutive headless menu screenshots pixel-stable
(correct green PartyLand, red Speed Devils car); remaining swaps coincide
with genuine page changes/fades only.

## 22. Menu half-height letterbox: doubled planar rows (`src/vga.c`)

For the record — the earlier half of the same transition complaint shipped
without a write-up. The sidebar scrolled in filling the window height,
then shrank to a half-height letterboxed menu. Cause: the menu is planar
640×240 timing doubled to 480 scanlines, rendered as 240 square-pixel
rows, while the Mode-X 320×240 intro is already square at 240 rows — but
a period CRT shows 480 scanlines for both. Fix: the planar branch emits
each row twice when scan-doubling is set (split-screen logic kept on
logical rows), so 640×240 presents as full-height 640×480 like the
hardware. `-nodbl` restores the old sampling.

## 23. Launcher Scrolling choice ignored: a second clobber, fixed by removing it (`src/fantasies.c`, `src/dos.c`)

Fallout from §5.13.1/the launcher-options poke (`WRITEUP-PHASE2.md`
§5.13.1, `fantasies_intercept_cfg_open()`): every option except Scrolling
reached the table correctly; Scrolling always came back Medium, in both
the F5 menu and actual gameplay, regardless of what the launcher was set
to.

Cause, found by reading the reconstructed `INTRO.ASM`
(`historicalsource/pinballfantasies`) against the same buffer the launcher
pokes: right after the boot-time load of `PINBALL.CFG` -

```
CALL LOAD_TOGGLAREN
JNC  TOGGLAREN_READY
MOV  TOGGLAREN.S_SCROLLING,1      ; only this one field
TOGGLAREN_READY:
```

`fantasies_intercept_cfg_open()` always makes that load fail (deliberately
- an existing `PINBALL.CFG` is what wedges the sound driver's PLL
calibration, per §5.13.1), and `OPENFILE`/`READFILE` never touch flags, so
the real `INT 21h` carry reaches this `JNC` untouched and it always takes
the "no config file" branch - the same branch a genuine fresh install
takes. That branch only defaults `S_SCROLLING` (the game trusts zeroed
memory for the other five fields), so it silently overwrites whatever
Scrolling value was just poked, every boot. It also runs long before the
F5 menu is ever drawn, which is why the menu displayed the wrong value too,
not just the table.

A first attempt corrected the byte only at the intro-to-table handoff (the
one point `PINBALL.CFG` is written back to disk, immediately before
`BEFORE_STARTING` relays the buffer to the table over `INT 65h` - see
§5.13.1/§2.2 of `WRITEUP-PHASE2.md` for that relay). That fixed what the
table received but left the menu showing Medium the whole session, since
the menu reads the buffer long before that write happens - confirmed
wrong by the user testing it live. Corrected at the source instead: static
byte-scan of the shipped `INTRO.PRG` found

```
73 05 C6 06 A5 49 01   ; JNC +5 / MOV byte ptr [49A5h],1
```

as a unique match (one occurrence in the 345 KB image), target `49A5h`
being the already-confirmed `TOGGLAREN` base `49A3h` + 2 = `S_SCROLLING`.
`fantasies_patch_intro()` (called from `load_mz()` in `dos.c`, gated to
`INTRO.PRG` only) NOPs the 5-byte `MOV`; the `JNC` is left alone, since
either branch now falls into the same do-nothing bytes. Memory-only,
signature-checked like the existing SDR patches, `-nopatch` disables it.
Scrolling is then never touched again after the launcher's initial poke, so
it shows correctly in the menu and reaches the table unchanged. Verified
live: launcher set to Soft, F5 menu confirmed showing Soft.

## 24. Table-select palette flash, part 2: one frame of history wasn't enough (`src/vga.c`)

The §21 fix (majority-duration AR14 bank) regressed - reported live with
NOSOUND, after none of the intervening commits (§9/§23 launcher work, the
§5.13.1 flag forge, the flipper-modifier reconciliation) had touched
`vga.c` at all. Since the fix itself was provably unchanged, three exit-time
counters were added first rather than guessing: `AR14 switches` (writes that
changed the bank), `overrides` (frames where the majority pick differed from
the instantaneous register - i.e. the fix doing something), `mode-resets`
(`apply_regs()` calls, which wipe the switch history). User-supplied A/B
output, same menu, same build:

| | switches | overrides | override rate |
|---|---|---|---|
| SoundBlaster on | 1100 / 28.2 s (~39/s) | 135 | 12.3% |
| NOSOUND | 268 / 10.0 s (~27/s) | 105 | 39.2% |

Both nonzero, so the fix was firing in both cases, not disabled or reset out
from under itself (`mode-resets` was 3 in both - once at boot, not per-tick).
But the override rate more than triples with sound off, meaning whatever
paces the menu's AR14 flip - almost certainly the audio driver's own
interrupt chain, given `dumretf`/`creatretf` are driver callbacks per §21's
`INTRO.ASM` credit - runs at a different rate and duty cycle when
SoundBlaster isn't the one timing it. §21 only ever scored the single
*previous complete* frame (`fstart - per`): correct when the flip is a short
pulse deep inside one 16.7 ms frame, but not guaranteed when the pacing
changes and a frame can land mostly on the minority bank's side even though
it is still the minority over a couple of ticks.

Fix: score the last 4 complete frames (`fstart - 4*per`) instead of 1 -
still far under a human-visible delay, but enough to average out a single
frame's local phase against whichever driver ends up pacing the flip,
without needing to know its rate up front. `PALSW_N` (retained AR14-switch
history) doubled 16 -> 32 so the wider window can't run out of slots at any
plausible switch rate. Everything else about the mechanism - presentation
only, `pal_sw_n` reset on real mode sets, tables (256-colour) and the
single-bank hi-scores page untouched by construction - is unchanged from
§21. The three counters stay in the exit report as a standing diagnostic:
if this ever regresses again, `overrides` vs `switches` says immediately
whether the fix is engaging at all before anyone has to reason about timing
from scratch.

## 25. Table-select palette flash, part 3: it was never a flash (`src/vga.c`)

§21's diagnosis was wrong. Not the traffic - the *interpretation* of it.

Symptom that reopened this: comparing pfemu's table-select menu against real
screenshots of the DOS CD-ROM release, one of the two stacked table graphics
per page (the upper one) was a plainly wrong palette - not flickering, a
static wrong colour, every time. §21/§24's majority-duration AR14 render
was, by construction, incapable of ever showing anything else: it always
collapses the whole frame to one bank.

Re-reading `INTRO.ASM` with that question in mind (not "why does it flash"
but "why does the render need two banks") found the part §21 skipped over:
`julius` doesn't just load one 16-colour palette - the menu loads *two*, one
into DAC 0-15 and one into DAC 16-31, one per table graphic on screen
(`movpal ...,nuvaranden` / `movpal ...,nuvaranden+16*3`), immediately before
`MOV CS:CHANGE16PAL,TRUE`, under a comment §21 didn't quote: `set 2 palette
modes (on rasterint)`. And the raster callback isn't paced by guesswork
either - it's ordered with an explicit target line: `int 66h, ax=12h,
cx=220+10`. Two banks, one hardware-timed split. Not a redraw glitch that
occasionally escapes vertical blanking.

Confirmed live with a new diagnostic (`-paldbg`, logs every AR14 write with
its frame-relative scanline): the VBLANK write (bank 1) lands at line
490.6-492.0 of 527 every time, and the raster write (bank 0) at line
193.2-195.9 every time - a two-line spread on each, far tighter than
anything CPU-speed jitter would produce, and matching the disassembly's
`cx=230` target almost exactly. This is a hardware-precise split at a fixed
scanline, not a transient with a fuzzy landing zone.

It also explains why only the *upper* graphic ever looked wrong: switch-pair
intervals in the same trace are mostly ~33.5 ms (twice the 59.71 Hz frame
period) with occasional 16.7 ms ones - the driver's tick redraws the split
roughly every other video frame, not every one. On the frames it skips, AR14
never leaves bank 0 for the whole frame. The lower graphic is drawn from
bank 0 in *both* cases, so it is rock-solid. The upper graphic is drawn from
bank 1 only on the frames the split fires, and from bank 0 (the wrong bank
for it) on the frames it doesn't - a real, register-level alternation, not
renderer noise. That is also exactly the "flicker" reported when a first
attempt at fixing this rendered each row from a precise single-frame lookup
instead of a frame-wide vote: it wasn't a bug in that attempt, it was real
behaviour finally reaching the screen instead of being permanently masked.

First fix tried: per-row resolution, not per-frame. For each output row,
convert its logical row to a physical scanline (`yl * rowh`) and an absolute
time within the most recently complete guest frame, then look up whichever
AR14 value was in effect at that instant from the existing switch history.
This is bit-exact - it reproduces the real register timeline, including the
every-other-frame alternation, faithfully. Live-tested with the sound driver
disabled (`NOSOUND.SDR`) to rule out a Sound-Blaster-specific timing bug:
the same alternation reproduced identically, which would be a remarkable
coincidence if it weren't a real property of the driver-paced raster-split
technique itself (confirmed authentic, matching the engine's
independently-documented 30 Hz tick for other systems, e.g. the DMD, §20)
rather than a pfemu defect in one specific sound path.

Shipped fix: given the choice between that bit-exact alternation and a
steady picture, cosmetic stability was chosen over accuracy. Rather than
resolving AR14 from the current frame's own switch timing, the render now
derives the split - which bank is "upper", which is "lower", and the
scanline between them - from the most recently observed switch pair in
history (walk `pal_sw_val[]` backward from the end for the last entry that
differs from `ar[0x14]`; convert the latest switch's own timestamp to a
scanline the same way `-paldbg` does), and applies that split to *every*
frame, whether or not that frame's own driver tick actually rewrote AR14.
A real change (paging to the other two tables, their palette and split
point) still reaches the screen within one driver tick, since it's read
fresh from the same rolling history every frame. Screens that only ever use
one bank (tables, text, the single-bank hi-scores page) are untouched,
since no second value ever appears in their history - unchanged from §21.
`split_log` (the existing line-compare address split, §22) and this AR14
split are independent and combine correctly since both key off the same
physical row. The three §24 counters keep their meaning, `overrides` now
counting rows instead of frames.

Caveat carried forward from §21: `INTRO.ASM` is a third-party
reconstruction, not the original binary, so its comments and constants are
a map, not ground truth - but here the live trace confirms the map exactly
(the two write-lines, the two-frame pacing), which is as much verification
as is practical without the original source.

Follow-up, measured from screenshots: the seam was landing 16 rows up
*inside* the upper graphic. Screenshot geometry puts the upper picture at
rows 20-209, a black letterbox gap at 210-269, and the lower picture at
270-459 - so a bank switch is only invisible if it lands in that 60-line
gap. The raw switch line is 194, i.e. 16 rows into the upper picture, which
is exactly the band that came out recoloured.

Cause: the write arrives on a countdown the driver starts at vertical
retrace, but the raster line the game asks for is numbered from the top of
the active display. The two are `vtotal - vrs` apart - 527 - 490 = 37 lines
here. Converting the observed line to active-display coordinates puts the
switch at 230, which is simultaneously the `cx` (`220+10`) that INTRO.ASM
passes to its ORDER RASTER call and the middle of the letterbox gap, 20
lines clear of either picture - comfortably outside the ~3-line jitter. Two
independent sources agreeing on 230 is what makes this a coordinate
conversion rather than a fudge factor.

Note this correction is applied where the seam is *drawn*, not to the guest
timing that produced it: the callback chain really is anchored 37 lines
early in emulation. Chasing that to its source is a driver/PIT question
(and `vga_status1`'s bit-0 semantics are deliberately non-hardware per
§5.8 of `WRITEUP-PHASE2.md`, so that is not a safe thing to go changing to
find out).

Open question, not resolved: *why* does the driver only redraw the split on
roughly every other frame rather than every one - a deliberate 1994 design
choice (piggybacking on a slower tick, as the DMD does) or some other
authentic property of the real port that pfemu is now reproducing
correctly? Either way it's no longer this file's concern: it stopped being
a `vga.c` question the moment the alternation was confirmed to come from
the register timeline itself rather than from how that timeline gets
rendered.

## 26. Ball flicker: the engine races the beam, our present ignores it (`-balldbg`)

The reconstructed MS-DOS port source settles what §16/§18 could only infer
from VRAM hashes. `FANTASIE.ASM`'s `PUTTHEBALL` is commented
`FN: DELETES AND PUTS THE BALL`: it restores the saved background at
`OLDPOS` (`DELBALL`) and then re-saves and redraws at the new position
(`PUTBALL`). No sprite double-buffer exists - between those calls the ball
is absent from the visible page. The window is much wider than the 16-line
ball, because the caller sets `PUTITBETWEEN`/`PUTF3BETWEEN` and a full
`DOFLIPPER` and `DOFLIPPER3` blit run *inside* it, and any interrupt taken
mid-routine (the MOD mixer above all) stretches it further.

What §18 missed is that the engine does not leave that gap exposed - it
races the beam instead of buffering. `VBLANK_INT` compares `SC_Y` against
`START_RASTER + MIDDLE_RASTER_LO/HI`: a **lower-half** ball is redrawn
immediately in the vblank handler, with the beam still above it, while an
**upper-half** ball sets `LATEGFX=TRUE` ("PUT NEXT SYNC LAST") to defer the
redraw to `LATE_RASTER_INTERRUPT`, which fires mid-screen after the beam has
already passed it. Either way the erase/redraw happens in the half the beam
is not painting, and a CRT never sees it.

So §18's "the ball hazard is structural and authentic on real hardware too"
is half right. The erase-redraw is structural; the *flicker* is not
authentic. Period hardware flickered only when the scheme broke down, and
the handlers are full of paths where it does: `RETTEF`/`RETAF` on
`SLOWCNT`, the `INSIDE_BALLHANDLER` and `INSIDE_RASTINT` reentrancy guards,
the `LAST_WAS_VB` ordering requirement, and the `TIME_LEFT` "music crisis"
flag the mixer sets. When the mixer overran, the redraw landed under the
beam.

pfemu defeats the scheme outright: `vga_render` snapshots all of VRAM at one
instant and has no beam at all, so a present landing inside the gap drops the
ball from the *whole* frame - a full-frame dropout, more visible than the
partial tear hardware would have shown.

This also re-reads §16. Locking presents to vsync failed not because
phase-locking is wrong but because vsync is precisely when a lower-half ball
is being redrawn - the worst available phase. The source says where the safe
phase is: between the end of the vblank handler and the start of the
mid-frame raster interrupt the ball is quiescent by construction, and a
snapshot there also matches what a CRT displays (upper half as already
painted, lower half as about to be). Caveat: §18's "no quiet window" holds
for the dot-matrix, lights and animations, which keep writing through that
span. The quiet window is the ball's alone.

Measure before locking anything - hence `-balldbg` (`src/fantasies.c`),
which times the gap instead of estimating it. It locates `PUTTHEBALL` by
signature, not fixed offset, the way `fantasies_patch_pause()`/`_balls()`
locate theirs: the prologue (`PUSHA` / `PUSH 0A000h` / `POP ES` /
`CMP VERYFIRSTPUT,TRUE` / `JE`) is unique in all four shipped `TABLE1-4.PRG`
of both the floppy and Deluxe releases, as is the epilogue (`CALL PUTBALL` /
`SET_DS DATA` / `MOV OLDPOS,SI` / `MOV OLDSHIFT,DX` / `POPA` / `RETN`); the
body is 134 bytes in all eight, only the DS-relative operands differing, and
the two signatures are cross-checked against each other. A two-address
compare in `cpu_step()` logs entry/exit emulated time and frame-relative
scan line; the exit report gives min/mean/max gap, scan-line histograms for
entry and exit, and the count of presents that landed inside the gap - the
last being the direct measure of how often we lose the ball. Auto-off after
20k redraws (~11 min of play).

Usage: `pfemu.exe -d FANTASYDX -balldbg > ball.log 2>&1`, play a ball, quit.

### Measured, 1305 redraws over ~27 s of play

```
gap  min=96.7us  mean=304.9us  max=968.7us   orphans=0
presents=1606  inside-gap=16  (1.00% - these frames lost the ball)
band A (raster int, upper-half ball)  lines 231-260 / 527   930 redraws
band B (vblank,     lower-half ball)  lines 491-521 / 527   375 redraws
```

Two clean bands exactly where the source says they should be, no smearing,
and zero orphans - every entry paired with its exit, so the reentrancy
guards hold. The bands leave two quiet spans of 231 and 237 lines (~7.4 ms
each) against a 0.97 ms worst-case gap: a ~7x margin either side. The 1.00%
of presents landing in the gap is the flicker, and it matches the predicted
order (1305 x 304.9us over 26.9 s = 1.5% if presents were uniformly
distributed; the wall timer's drift accounts for the rest).

Note both bands fire in the same frame when a ball crosses into the upper
half - VBLANK_INT redraws it *and* sets LATEGFX, so LATE_RASTER_INTERRUPT
redraws it again - which is why 1305 redraws appear in ~27 s rather than the
30 Hz tick rate.

### The fix (`src/main.c`)

Presents now sample at a fixed emulated-frame phase instead of on the
drifting wall timer: the window is lines 60-220 of 527 (`PRESENT_PHASE_LO`/
`_HI`), inside the early-frame quiet span and ~60 lines clear of either
band. The frame-index gate from §18 stays, so each emulated frame still
presents at most once; if emulation falls more than one frame behind, the
phase test is bypassed rather than dropping the frame.

The early span is the CRT-faithful one of the two. An upper-half ball is
painted by the beam *before* the mid-frame redraw, so showing the pre-redraw
state is what hardware displayed; a lower-half ball is identical in both
spans. Sampling the late span instead would show an upper-half ball one tick
early, and - because the age of the displayed position then jumps from 0.25
to 0.75 of a frame as the ball crosses mid-screen - make it appear to step
*backwards* at the crossing. The early span's jump goes the other way (the
ball catches up), which is both faithful and the less objectionable
artifact. The half-frame discontinuity at the crossing itself is inherent to
the game's two-schedule design and is present on real hardware.

`-nophaselock` restores the old drifting wall timer for A/B comparison.
This does not disturb the AR14 palette latch (§21/§24/§25), which was
deliberately built phase-independent - it keys off switch history, not the
instantaneous register.

### Why the first attempt only halved it: the present check was in the wrong loop

Locking the phase dropped the loss rate from 1.00% to 0.49% and no further.
`-balldbg`'s fallback counter found the reason: **55.38% of presents were
bypassing the phase test entirely.** The decision sat *after* the inner
catch-up loop, which advances emulated time in ~10 us batches - but a single
outer iteration can cover most of a frame, because `plat_sleep_ms(1)` is
coarse on Windows. The window was overshot constantly, the fell-behind
fallback took the frame at an arbitrary phase, and the presents profile
smeared across the whole frame.

Moving the decision *inside* the catch-up loop - breaking out on the batch
that enters the window, at the cost of one deferred batch per frame - fixed
it outright:

```
before:  presents=2882  inside-gap=14 (0.49%)   fallback 1596 (55.38%)
after:   presents=5110  inside-gap= 0 (0.00%)   fallback    1 ( 0.02%)

redraws   |                                       #@#..               ==-..|
presents  |       @     ..   .                                          .  |
```

Lesson worth keeping: a phase-locked present is only as good as the
granularity of the clock you test the phase against. The window was correct
from the first attempt; nothing sampled it often enough to land in it.

### Band positions move per table

Band A sits at lines 231-260, 225-258 and 296-362 on the three tables
measured so far - it tracks each table's own raster-interrupt line, so a
single hardcoded window is on borrowed time. All three leave lines 60-200
clear, which is what `PRESENT_PHASE_LO/_HI` now use, but if a table is found
whose band A starts before line 200 this needs to derive the window at
runtime from the observed bands instead of using constants.

### Still open: the ball is drawn a few pixels off its path

A second, distinct symptom - the ball sitting slightly beside where its
trajectory says it should be, even rolling freely. Two candidate causes,
both measured rather than assumed:

* **Camera/ball skew (ours).** `LATE_RASTER_INTERRUPT` calls
  `SETSCREENSTART` *before* `PUTTHEBALL`, so between them the CRTC start is
  one camera step newer than the ball drawn under it; a present sampled
  there displaces the ball. pfemu compounds this by reading the start
  address live in `vga_render`, where real VGA latches it at vertical
  retrace - so a mid-frame camera write applies immediately for us and only
  next frame on hardware. The phase lock may already avoid this window;
  `-balldbg` now reports camera/ball skew at present time to settle it.
* **The game's own stepping (authentic).** The redraw cadence is clean
  (1633 of 2421 intervals at 33.49 ms) but 28 are 16.75 ms - the double-tick
  the source predicts, where `VBLANK_INT` redraws the ball *and* sets
  LATEGFX so `LATE_RASTER_INTERRUPT` redraws it again, giving the ball two
  physics steps on the frame it crosses mid-screen. Vertical steps under
  gravity also wobble non-monotonically (3, 3, 5, 4, 6) in the guest's own
  fixed-point arithmetic. Both are in the original code and off-limits.

### §19 smooth scrolling appears to be inert

Reading `smooth_start()`: `r = (now - prev_t) / (last_t - prev_t)` with
`now` always *after* `last_t`, so in steady state (publishes ~33 ms apart,
presents ~16.7 ms apart) r lands between 1 and 2 and the function returns
the raw register every time. A blind A/B of `-nosmooth` against the default
was reported as showing no noticeable difference, which is consistent.
`smooth_calls`/`smooth_interp`/`smooth_late` counters now print at exit to
confirm it outright. If it is inert, §19's claimed benefit was never real
and the 30 Hz scroll stepping it set out to smooth is still there.

## 27. Ball/camera pairing and a real start-address latch (`src/vga.c`)

Follow-up to §26. With the blink-out gone, a second symptom was left: the
ball sitting a few pixels off its path, invisible when rolling slowly,
clearly visible when fast, and reading as a doubled image or "trail" on a
really fast ball. `-balldbg` now measures it as camera/ball skew - a present
where the CRTC start address is not the one the ball was drawn against.

### What it is

```
camera/ball skew at present: 339 of 7037 (4.82%) mean=4.78 rows max=37.00 rows
[skew] t=20.617199 rows=37.00 age_ball=19.72ms age_write=8.341ms
```

`age_ball`/`age_write` are rock constant across every event, so this is a
fixed structural offset rather than jitter. Converted to frame lines (our
presents land at line ~58):

| event | frame line | handler |
|---|---|---|
| ball drawn | ~494, two frames back | `VBLANK_INT` |
| camera written | ~323, one frame back | `SETSCREENSTART` in `LATE_RASTER_INTERRUPT` |

The engine commits the camera in the raster interrupt but a **lower-half**
ball in the vblank handler - ~11 ms and one 30 Hz tick apart. For an upper-half
ball both happen in the same handler back to back, so it is consistent, and
that is 79% of redraws; the remaining 21% are drawn against a camera one tick
newer than themselves. The displacement *is* the camera step, which is why it
scales with ball speed and alternates every other frame.

Skipped redraws are not the cause: of 2038 intervals, 1977 are exactly two
frames, 47 are the mid-screen double-tick, and only 14 are skips.

### It is authentic, and smoothing it is a deliberate choice

Real VGA latches the start address at vertical retrace, so on hardware the
new camera takes effect at the vblank where the ball is *not* redrawn -
producing the same alternating displacement. By request, traded for a steady
picture the same way the AR14 palette latch was (§21/§24/§25): `vga_render`
now displays the start address that was in force when the ball was last
drawn, keeping ball and playfield locked together. The background then steps
at the ball's own 30 Hz cadence, which is the rate it already stepped at.
Falls back to the latched value when no ball has been drawn for ~3 ticks
(intro, menu, between balls). `-noballsync` disables.

The pairing is fed by a third hook on `PUTTHEBALL`'s epilogue - the
`MOV [OLDPOS],SI` store, located by the same signature as §26 - which is live
in normal play, not just under `-balldbg`, and costs one compare per
instruction while a table is loaded.

### Start address now latches at vertical retrace (accuracy, not cosmetic)

Independently: pfemu read `cr[0x0C]/cr[0x0D]` live in `vga_render`, so a
mid-frame camera write took effect a frame earlier than on hardware and could
be sampled torn between the hi and lo byte. A 32-entry history of writes plus
the time of the last retrace now yields the value hardware would be
displaying. This does not change what we show at the locked present phase
(camera writes land at line ~323, presents at ~58), but it was wrong, and it
matters for `-nophaselock` and any future change to the present phase.
`-nolatch` disables.

### §19 smooth scrolling is inert - confirmed, still unfixed

```
[pfemu] smooth-scroll: 7733 presents, 0 interpolated (0.00%), 7385 past-last
```

`smooth_start()` computes `r = (now - prev_t) / (last_t - prev_t)` with `now`
always after `last_t`, so r lands between 1 and 2 in steady state and the raw
register is returned every time. A blind A/B of `-nosmooth` was reported as
showing no difference, consistent with the counters. So §19 never did
anything and the 30 Hz scroll stepping it set out to smooth is still there.
Left as-is for now; note that interpolating the viewport under a 30 Hz sprite
is exactly what would desynchronise the ball again, so any revival has to be
done downstream of the pairing above.

## 28. Present window derived at runtime, not hardcoded (`src/fantasies.c`)

§26 placed the present window with constants read off one table's `-balldbg`
trace. That was always luck: the redraw bands sit at each table's own raster
line, measured at lines 222.7, 225.5, 231.4 and ~296 across the four shipped
tables, and the several releases carry slightly different table binaries. All
four happened to clear the hardcoded window, table 4 by only 23 lines.

The bands are now learned from the running game. Every redraw marks the frame
buckets it covered - `PUTTHEBALL`'s entry hook to its epilogue hook, both live
in normal play - into a 64-bucket occupancy histogram. The window is placed in
the quiet span that **follows the vblank band**, found by walking forward from
vertical retrace to the first empty bucket, with `PW_MARGIN` (4 buckets, ~33
lines) kept clear of either side.

Deliberately that span and not merely the largest one: sampling before the
mid-frame redraw is what matches the CRT, because an upper-half ball is
painted by the beam before that redraw, so the pre-redraw state is what
hardware showed. The later span would show it one tick early and make it step
backwards as it crosses mid-screen (§26).

Checked against the four measured tables, the derived window beats the
constants everywhere, and widens the tightest margin by 45%:

| table | derived window (lines of 527) | margin to band A | hardcoded |
|---|---|---|---|
| 4 | 32.9-189.4 | 33.3 | 23.0 |
| 1 | 32.9-197.6 | 33.8 | 31.7 |
| 2 | 24.7-189.4 | 36.1 | 25.8 |
| 3 | 32.9-255.3 | 40.7 | 96 |

Table 2's quiet span starts at bucket 63 and runs through 0, so the
wrap-around path is exercised by a real table; `main.c` tests the window with
`(lo <= hi) ? (f >= lo && f <= hi) : (f >= lo || f <= hi)` for the same
reason.

The constants in `main.c` remain as a seed for the first `PW_WARMUP` (90)
redraws, ~3 s of play, and are known good on all four shipped tables. The
histogram resets when a new table image loads or when the CRTC timing changes,
so a hi-res toggle - which moves `MIDDLE_RASTER`, and with it the bands -
re-learns instead of pinning the window to stale geometry. `-balldbg` reports
the derived window at exit.
