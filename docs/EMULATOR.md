# How pfemu works

A 386 real-mode PC emulator written in C, just wide enough to run Pinball
Fantasies: CPU, VGA, PIC/PIT/keyboard, BIOS and DOS services, Sound Blaster
audio, and the game-specific fixes in `src/fantasies.c`. One native Win32
binary, no runtime dependencies beyond Windows and the game data.

| File | Responsibility |
|---|---|
| `src/cpu.c` | 386 real-mode interpreter |
| `src/vga.c` | Text, planar, chain-4, Mode X, DAC, timing, presentation |
| `src/dev.c` | PIC, PIT, keyboard controller, ports, CRT timing |
| `src/bios.c` | BIOS interrupts, vector setup |
| `src/dos.c` | Processes, memory, files, EXEC, TSR, write overlay |
| `src/sound.c` | DMA, Sound Blaster DSP, Windows audio, WAV capture |
| `src/fantasies.c` | Game-specific fixes and trainer |
| `src/release.c` | Release detection by SHA-256 (`src/reltable.h` generated) |
| `src/lzexe.c` | LZEXE 0.91 unpacking at load |
| `src/launch.c` | Native launcher and saved options |
| `src/replay.c` | Session record/replay |
| `src/main.c` | Window, input, main loop |

Build: `build.bat` (MSVC 2019, `/O2 /GL /LTCG`,
`/SUBSYSTEM:WINDOWS`). The binary reattaches to the parent console so CLI
output still works from a terminal.

## The launch chain

`.PRG` files are ordinary MZ executables with a renamed extension. They can't
start on their own - they depend on services the launcher installs.

**`PINBALL.EXE` / `PF.EXE` / `PFDEMO.EXE`** ( ~1.7 KB, fully disassembled)
shrinks its own memory block, hooks INT 9 (keyboard) and INT 24h (critical
error), installs an API on INT 65h, EXECs the intro, then loops EXECing
`Table<n>.Prg` (0 = quit). Filenames live in a 12-byte-stride table where each
entry's `$` prefix doubles as the INT 21h AH=09h error string.

**INT 65h** (the launcher API): `0000` reports status, `FFFF` sets the next
program, `0100`/`0200` stash and retrieve a 6-byte state blob that survives
process switches (also persisted as `PINBALL.CFG` at the intro-to-table
handoff). Other values return and clear the last scancode.

**Sound drivers** (`.SDR`) are TSRs: each game program EXECs the one named in
`SOUND.CFG`, and it installs itself on INT 66h + INT 8 via INT 21h AH=31h.
Every program loads its own copy - intro and each table. The driver owns
machine time: INT 8 drives the scheduled callbacks the game registers through
INT 66h. `TIMER.BIN` is a raw code blob that sets mode 13h and measures
machine speed.

**Why emulate.** Static recompilation fails on computed jumps, self-modifying
protection code, and interleaved data; a DOS-API shim fails because the game
barely uses DOS - it programs the CRTC directly, reprograms the PIT, hooks
IRQs, and phase-locks its frame timer by polling port 3DAh. Emulating the
machine is the bounded job: one program, one graphics family, one timer, one
keyboard.

Native entry: BIOS/DOS handlers are C code reached through an invalid opcode
(`0F FF nn` + `CF`) in stubs at `F000:(0x1000 + n*4)`; the core performs the
IRET unless the handler opts out (EXEC, terminate, blocking INT 16h). INT 8 is
real 8086 code in the ROM image (at `F000:0E00`) because the driver hooks and
chains it.

## Time, CRT, and interrupts

- `emu_time` advances at a fixed instruction rate (6 MIPS default, `-ips`).
  The main loop runs the guest until emulated time catches up with wall
  time (times `-speed`), never running past the next timer deadline so IRQ 0 lands on
  the due instruction.
- Polled registers (3DAh, PIT counters, port 61h) read through `emu_now()`,
  which folds outstanding instructions into the clock so short pulses can't
  fall between samples.
- CRT timing is derived from the registers (dot clock, character width, CRTC
  totals): ~70.09 Hz for text/mode 13h, ~59.71 Hz for 480-line Mode X. Nothing
  is hardcoded.

Two hardware behaviors the game depends on:

- **3DAh bit 0 pulses once per scan line, including through vertical
  blanking.** The driver's calibration loop is a phase-locked loop that needs
  a strictly monotone edge count; treating bit 0 as "display inactive"
  creates a flat step where it has to land and the loop never locks. It
  settles near reload 19921 (59.9 Hz tick vs 59.71 Hz refresh).
- **PIT channel 0 mode 0 is a one-shot** - one interrupt per count, then the
  output stays high until reloaded. Treating it as periodic delivered extra
  interrupts that walked the driver's event list off its end. Counter reads in
  mode 0 count down from the armed `next_irq` and keep decrementing past
  terminal count (never reload), which is the shape the driver's
  latency-compensating ISR (`bx = delta + cx - 10`) is written for. Getting
  this right doubled the game tick to its intended ~60 Hz; see
  [Dot-matrix and game tick](#dot-matrix-and-game-tick).

Two DOS behaviors the game depends on:

- **Resident children are released with their parent** (`mcb_free_children`).
  Strict DOS keeps a TSR forever, but the intro's 238 KB shrink + resident
  driver leaves only 394 KB free while `TABLE1.PRG` needs 524 KB contiguous -
  no table could ever load. Releasing also restores any IVT entries pointing
  into the child (the driver's own uninstall would have done the same).
- **Registers survive EXEC.** The table sets `DX` for its INT 66h module-name
  call and trusts `DS` to have survived EXECing its `.SDR` eight bytes
  earlier. `dos_exec`/`dos_terminate2` now carry the register set across, so
  the driver opens `TABLE1.MOD` instead of a filename read from its own
  period table.

Instruction timing is uniform (1/ips per instruction); the game times itself
against the CRT and PIT so it doesn't care. The handful of x87 escapes in the
intro are logged and skipped.

## Game-specific fixes (`-nopatch` disables all of them)

Applied in memory at load/run time, signature-checked, never written to disk:

- **Copy protection.** The manual-lookup screen ("enter word 12 on line 7")
  is bypassed by forging its own "already answered" flag: reads of 2 bytes at
  `Intro.Mod` offset 252868 get the sentinel `20 01` before reaching the
  guest, so the screen never draws and nothing is written back. (An earlier
  JNC->JMP image patch did the same job less cleanly and is removed.)
- **Pause race.** Clears an interrupt-handshake race after unpausing that
  could leave the ball stuck - a bug noted in the original developers' own
  source comments.
- **Flippers.** Repairs shared flipper state when multiple flipper keys
  overlap, and recovers when Windows loses a modifier-key release.
- **Options.** Makes the original (otherwise ignored) In-game Music setting
  work; pokes the six launcher options into the release's options buffer and
  NOPs the intro's boot-time default (one-field on floppy, all-fields on
  Power Pack/Deluxe) so the poke survives.
- **Trainer.** Signature-located hotkeys from two 1994 trainers (infinite
  balls, ball control, infinite tilts). Only when enabled in the launcher.

## Display

Modes seen: text, 16-color planar 640x240 doubled to 480 scanlines (menu),
unchained Mode X 320x240 with 480-line timing + CRTC scan doubling (tables),
mode 13h, and the amber dot-matrix score panel via the CRTC split-screen
(line compare) register.

Three presentation fixes, all display-side - game state untouched:

- **Table-select palette split.** The menu loads two 16-color palettes (DAC
  0-15 and 16-31, one per table graphic) and flips the Color Select bank
  (AR14) at a driver-ordered raster line (~line 230 in active coordinates,
  `cx = 220+10`). The renderer derives the split (upper bank, lower bank,
  seam) from recent switch history and applies it every frame, blanking the
  seam into the letterbox gap. Single-bank screens are unaffected.
  `-paldbg` logs switch lines for diagnosis.
- **Ball flicker.** The engine erases and redraws the ball with no sprite
  buffer, racing the beam instead: lower-half balls redraw in the vblank
  handler, upper-half balls in a mid-frame raster interrupt. pfemu snapshots
  VRAM with no beam, so a present inside the gap dropped the whole ball.
  Presents are now sampled in the quiet span after vblank (learned at runtime
  from a 64-bucket redraw histogram; seed constants cover the first ~3 s),
  which also matches what a CRT showed. `-nophaselock` restores the old
  drifting wall timer; `-balldbg` times the gap and reports skew.
- **Ball/camera pairing.** For lower-half balls the camera commit (raster
  interrupt) and the ball draw (vblank) are a tick apart, so pairing a fresh
  camera with a stale ball displaces fast balls by the camera step. The
  renderer shows the start address in force when the ball was last drawn
  (falling back after ~3 quiet ticks), and latches the start address at
  retrace like hardware instead of reading it live. `-noballsync` /
  `-nolatch` disable each.

Earlier ideas that didn't survive: vsync-locked presents (parked the sample
inside the ball gap - worse, reverted), viewport interpolation for 30 Hz
scroll stepping (computed a ratio >= 1 on every frame and returned the raw
register - inert, removed), and "DMD choppiness is intentional pacing" (wrong;
see next section).

## Sound

`SETSOUND.EXE` runs under pfemu; picking Sound Blaster writes a 25-byte
`SOUND.CFG` (port, IRQ, quality) with no autodetection. The driver then does
plain 8-bit auto-init DMA playback: mask channel 1, program page/offset/count
(10,800-byte ring), DSP reset -> `AA`, speaker on, time constant (e.g. `ADh` =
12,048 Hz), `14h` output, unmask. Its software MOD mixer fills one half-ring
while the card plays the other, tracking the card by reading the DMA current
address.

`src/sound.c` provides: 8237 with flip-flop, modes, masks, pages, and
auto-init reload; DSP reset/command state machine; `sb_tick()` pacing that
converts emulated time to samples due and raises the IRQ at block boundaries;
Win32 `waveOut` sink (blocks dropped, never queued, so audio stays live) and
`-wav` capture upstream of the volume gain.

## Dot-matrix and game tick

The panel update is guarded by a `TIME_LEFT` flag the sound driver's ISR
passes in - set when mixer headroom runs low (a table runs a 2-block ring
with a 1-block threshold, so this trips a few percent of the time). That
costs ~3% of updates, not half: the halved tick was a PIT bug. Mode-0 counter
reads were returned as free-running rate-generator phase instead of counting
down from the armed one-shot, corrupting the driver's per-interrupt delay
(`+cx` added up to 11 ms instead of subtracting microseconds) so the second
of its two timer events per frame never fired. With exact mode-0 reads plus a
main-loop deadline fix (a truncated remainder serviced every IRQ ~one batch
late) and smaller batches in the unarmed window, the tick went 29.7/s ->
59.3/s against 59.71 Hz CRT, one-shots 113/s -> ~120/s (two per frame =
119.4), IRQ latency 99 us -> 2.4 us. Music tempo didn't change - the MOD player
runs off the audio clock at 50 Hz. `-matdbg` counts ticks/updates/crises and
latency; `-nopitm0` restores the old reading for comparison.

## Files stay clean

The game writes to its own files (two sentinel bytes near the end of
`Intro.Mod`, `.hi` scores, `PINBALL.CFG`, `SOUND.CFG`). Every write is
redirected to a per-install `PFEMU-STATE/` copy-on-first-write overlay; reads
prefer the copy once it exists. Installed files are never modified, and
deleting `PFEMU-STATE/` resets state. Launcher options live separately and
are applied in memory at boot.

## Performance

Same guest semantics, less host work. Measured ~36-43% faster (e.g. 33 ->
45 MIPS intro, 22 -> 32 MIPS gameplay at 6 MIPS default):

- Time base multiplies by a cached reciprocal instead of dividing per
  polled-register read.
- `vga_status1()` (the hottest path, ~63 M calls/s headless) uses cached CRT
  geometry, multiply+truncate phase math instead of `fmod()`, and a
  direct-mapped status histogram.
- PIT counters cache inverse period per reload; same `fmod()` removal.
- `cpu_ld/st` are `static inline` in the interpreter TU with single range
  checks; `REP MOVS/STOS` has a bulk `memmove`/`memset` path for plain-RAM,
  forward, >=16-count copies - except the overlapping forward case, which must
  stay on the per-element loop (`REP MOVS` propagates written bytes;
  `memmove` doesn't; that one case is how LZ unpackers expand runs - found
  via the demo's PKLITE drivers, regression-tested in `tools/reptest.c`).
- Main-loop batch 64 -> 256 with the timer-deadline clamp unchanged; build
  with `/GL /LTCG`.

Deliberately untouched: CPU dispatch (a dynarec would be a rewrite),
`vga_render()` (60 Hz host-side, negligible), `sb_tick()` (already
early-outs; 12 kHz is inherent). `/fp:fast` stays off (PLL lock needs exact
comparisons).

## Copy protection

Covered in [Game-specific fixes](#game-specific-fixes-src-nopatch-disables-all-of-them):
the `INTRO.MOD`-sentinel forge. Neither `INTRO.PRG` nor `INTRO.MOD` on disk
is ever modified; `-nopatch` restores the original prompt.

Keep `INTRO.PRG` pristine. A cracked copy has a different hash
and fails release detection, so it will not launch. The crack is also
pointless: the forge answers the check before the screen ever draws.

## Debugging and tracing

Most effort went into locating faults, so the instrument panel is generous.
Numeric addresses are hex; key scripts are `time:scancode:state` triples
(`1` = down).

| Flag | Purpose |
|---|---|
| `-secs N` + `-speed X` | Headless run: N wall seconds at X times realtime |
| `-shot F` / `-shotevery N` | PPM screenshot(s) |
| `-keys "..."` | Scripted keyboard input at emulated times |
| `-wav FILE` | Capture audio |
| `-xring` | Last 8,192 executed addresses at exit (runs collapsed) - the fastest way to find where a guest went off into data |
| `-trap LO HI` / `-trapexit` | Stop on entering an address range / on child exit |
| `-mem LIN` | Dump 256 guest bytes at exit |
| `-dumpseg SEG` / `-undefdump` | Write a 64 KB segment at exit / the faulting segment on bad opcode |
| `-prof` | `CS:IP` samples every 4096 instructions + 1K buckets |
| `-memwatch LIN` | First/last 32 writes to a linear address + total (CPU store path) |
| `-intwatch NN` / `-intstat NN` | Trace / count INT NN calls |
| `-dosdbg` / `-iotrace N` / `-snddbg` | DOS INT 21h calls / DMA+SB+OPL port traffic / DSP transfers with gaps |
| `-pll N` | Timer-0 reloads with guest DI/BP/CX and CRT geometry |
| `-flipdbg` / `-vscan N` / `-dmd` | CRTC start writes with phase+caller / VRAM-change hashes / DMD-region write cadence |
| `-balldbg` / `-matdbg` | Ball erase/redraw gap vs present phase / game ticks vs panel updates vs crises vs IRQ latency |
| `-paldbg` | AR14 bank writes with frame-relative scanline |
| `-vgastate` | Video mode, select registers, DAC, pointed-to memory at exit |
| `-force256`, `-nodbl`, `-oldtiming`, `-noballsync`, `-nolatch`, `-nophaselock`, `-nopitm0`, `-dmairq` | Disable one behavior to bisect display/timing faults |

Reproducible headless runs (`-speed 8 -secs 3 -keys ...` + PPM diff) beat
interactive debugging for rendering and timing. Static helpers: `re/mz.py`
(MZ parser), `re/d16.py` (Capstone 16-bit wrapper), `re/scan.py` (INT/IN/OUT
scanner). Note the `.SDR` trap: the resident block is copied down over init
code at runtime (file offset + `0x72`), so disassemble live dumps (`-mem`),
not the file.

Deviations from hardware, stated plainly: resident children released with
their parent (+ IVT restore); 3DAh bit 0 follows horizontal retrace through
blanking; uniform instruction timing; no x87; protection bypassed by forging
its flag. Each is the cheapest behavior the game's code accepts.
