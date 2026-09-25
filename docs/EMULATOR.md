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
| `src/launch.c` | Native launcher and saved options |
| `src/launchcore.c` | The launcher's rules and texts, shared with the Linux one |
| `src/launch_gtk.c` | The launcher on Linux (GTK 3) |
| `src/cdimage.c` | Deluxe CD image import (GOG's `game.gog`) for the launcher |
| `src/replay.c` | Session record/replay |
| `src/main.c` | Window, input, main loop |

Build: `build.bat` (MSVC 2019, `/O2 /GL /LTCG`,
`/SUBSYSTEM:WINDOWS`). The binary reattaches to the parent console so CLI
output still works from a terminal.

## The launch chain

`.PRG` files are ordinary MZ executables with a renamed extension. They can't
start on their own - they depend on services the launcher installs.

**`PINBALL.EXE` / `PF.EXE`** ( ~1.7 KB, fully disassembled)
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
  High resolution (360x350 at ~71 Hz vs 320x240 at 60 Hz) needs ~2x the
  guest CPU, so the default becomes 12 MIPS there unless `-ips` or a
  replay overrides it; otherwise the game runs slow and audio crackles
  even at the lowest sound notch. The exit `[pfemu] pace:` line reports
  wall vs emulated time, the ips in force, fell-behind re-anchors and
  audio drops, so a slowdown can be told apart from game-logic pacing.
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
a host DSP chain (linear resample of the 12-21 kHz mono source to 48 kHz
stereo, DC block, bass/treble shelves, oomph low shelf with soft-clip
limiter, optional headphone pseudo-stereo ambience + crossfeed, then the
square-law volume gain); Win32 `waveOut` stereo sink (blocks dropped, never
queued, so audio stays live) and `-wav` capture upstream of everything,
so enhancement and volume never move a capture or its replay hash. Host DSP
state is output-only: savestates don't store it, they just restart it.

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
deleting `PFEMU-STATE/` resets state. Launcher options live separately, in
`PFEMU-STATE/pfemu.cfg` (one key=value file per install, which the game never
opens), and are applied in memory at boot.

## Starting at a table

`-table N` / the launcher's **Start at** boots into `TABLEn.PRG` without
the intro. What it is not is `-p TABLEn.PRG`: the tables are not
standalone, and booting one that way leaves it with no INT 65h API and no
program loop to return to, which is why quitting used to die.

The boot program stays exactly where it is. Its loop is:

```
again:  EXEC intro                  <- skipped, once
        if (next == 0) exit
        EXEC name_table[next]       <- the wanted table lands here
        goto again                  <- quitting reaches the real intro
```

so the only intervention is skipping the first intro EXEC, reported to the
guest as a child that ran and exited 0. Everything after that is the
game's own control flow, which is why quitting a table returns to the menu
without anything having to arrange it. Redirecting that first EXEC to the
table instead - the obvious first try - ran it twice, once in the intro
slot and again in the table slot, and read as "quitting restarts the
table".

Two locators in the boot program's resident segment make it work, derived
from the loaded image rather than baked in (Deluxe has them at `CS:01F7`
and `CS:0020`; there is no reason for `PF.EXE` to agree):

- the six-byte options blob INT 65h `0100` stashes and `0200` retrieves.
  The tables read their options from here and **never** from
  `PINBALL.CFG` - `-cfgscan` finds no six-byte config transfer in
  `TABLE1.PRG` at all - so skipping the intro would otherwise silently
  drop every launcher setting. pfemu pokes it with what the intro would
  have stashed.
- the one-byte next-program index the loop EXECs through its 12-byte name
  table, set by INT 65h `FFFF`.

Signatures: `MOV DI,imm16 / MOV CX,6 / REP MOVSB` for the stash,
`MOV SI,imm16` for the fetch (both must name the same address), and
`CMP AX,FFFFh / JNZ / MOV CS:[imm16],BL` for the index.

Those operands are **CS-relative**, so the addresses are `CS_base + imm16`,
not `load_base + imm16`. The two are equal only when the EXE header has
`e_cs = 0`, which Deluxe happens to have and the floppy build and `PF.EXE`
do not (`e_cs = 0x28`). Getting this wrong puts every write 0x280 bytes
low, into the PSP, where it does no visible damage: the real index keeps
its own value and the loop quietly runs whatever that already pointed at,
so `-table N` always started table 1 on those two releases while Deluxe
worked perfectly. Scanning still covers the whole loaded image - it is only
the derived data address that is CS-based. A miss or a
disagreement turns the feature off and starts at the menu with a message,
rather than running the table on defaults and discarding the options
silently. `-cfgscan` reports the options-buffer scan for every program
that loads.

The `.pfr` carries `start_table:`, and a replay's value wins over `-table`:
the recorded event stream assumes whichever way that session began.

## Savestates

`F6` freezes the session into `savestates/<install>-<hash>.pfs` (one slot
per install, overwritten each save; the hash of the full install path keeps
two installs with a long common prefix apart); `F8` resumes it. Both are host-only keys
like `F11`: no guest effect, nothing logged. Implemented in
`src/snapshot.c` with per-subsystem save/load pairs (`dev/vga/dos/sound/
fantasies.c`): CPU + A20 gate, low 1 MB RAM, the clock trio
(`emu_time`/`emu_ips`/fold point), PIC/PIT/keyboard, VGA registers + VRAM,
DOS state with open files re-resolved by name and seeked back (never
re-truncated), DSP/DMA, and the resolved game locators. The release id +
code vector travel in the file and are verified on load; `PFEMU-STATE/`
drift only warns, as in replay.

Loading is verify-then-apply. The first pass checks the magic, section
bounds, the FNV seal, completeness and the release identity without writing
any emulator state, so a corrupt, truncated, foreign or wrong-install file
is refused with the running session untouched. Only then does the second
pass apply the sections. A failure in that second pass means the file
passed its own seal but this build no longer matches the one that wrote it;
that is unrecoverable and the message says so rather than reporting a clean
refusal over a half-loaded machine.

Play mode only: saving or loading while recording or replaying is refused
without exception, and so is a trainer-on session. (The removed replay
scrubber used to hold the one bypass, for its own rewind snapshots; with it
gone the refusal is a flat invariant again.) An in-flight `FindFirst` iteration is
reset on load (boot-time op in practice). `-load FILE` boots a snapshot
headless and `-snapsave FILE` writes one at exit.

`-untilemu SEC` stops at an emulated-time point. Like the replay footer
stop, it clamps the instruction batch and the idle clock jump to the target
cycle rather than testing once per wall-clock-paced frame, so two runs stop
on the same instruction instead of a variable distance past it. That is
what makes a round-trip comparable at all: run to a point, `-snapsave`,
`-load` and continue to a later point, and compare against one
uninterrupted run to the same point.

`-unthrottle` removes the wall-clock pacer, so the loop runs batches as
fast as the host manages instead of holding `emu_time` to `wall * speed`.
It is host pacing only and overrides nothing a `.pfr` carries: a replay
still forces its recorded `speed`, this simply stops the pacer acting on
it. That matters because a replay otherwise costs one wall second per
emulated second - `-speed` is discarded during replay by design, so before
this flag there was no way to replay a session faster than it was played.

Where batches begin and end is unchanged: that is set by
`dev_next_deadline()`, `replay_next_deadline()`, `until_cycles()` and the
256-instruction cap, all emulated-clock quantities that cannot see
`plat_time()`. The pacer only ever decided how many batches ran per outer
iteration. `tests/golden/speed-ab.sh` is the measurement rather than the
argument - it replays a vector paced and unthrottled and compares footer,
wav hash and every frame.

Pair it with `-freezetime`. `INT 21h` `AH=2Ah`/`2Ch` are the only
host-clock reads the guest can see (`INT 1Ah` runs off the emulated BDA
tick), and the game folds the result into its own state, so without the
freeze two otherwise identical runs differ - measured as exactly one byte
of guest RAM at linear `0x3A17C` on the deluxe release. `-freezetime`
pins the same constants replay uses. `tools/pfsdiff.py a.pfs b.pfs`
compares two snapshots section by section and locates a difference inside
the opaque ones.

Measured on the deluxe release. Boot through the intro attract loop, no
input: two independent runs to `emu_time` 30.000s (180,000,001 cycles)
produced byte-identical 1.3 MB snapshots, and a run stopped at 15.000s,
written to disk, resumed in a fresh process and continued to 30.000s
produced a snapshot byte-identical to the uninterrupted one - across a
process boundary and a 13h -> 12h mode change.

Mid-table, driven by `-keys` into Partyland with both flippers flapping:
a run split at 64.000s with a ball in flight and rejoined at 80.000s
matched the uninterrupted run in every guest-visible section - CPU, all
1 MB of low RAM, VRAM, PIC/PIT/keyboard, DOS with reopened handles, and
DSP/DMA. Two defects came out of that round:

- The interactive `F8` path never re-anchored the wall clock, while
  `-load` always had. The batch loop only runs while `emu_time < real`, so
  a snapshot from a longer session than the current one left `emu_time`
  ahead and no batch ran again - the window kept painting the restored
  frame, so it read as an instant freeze. Loading a state from earlier in
  the same session moved the clock the other way, where the fell-behind
  re-anchor already covered it, which is why it only ever showed up on a
  cold restore.
- The in-flight ball-draw span (`bg_inside`/`bg_line0`, set by the entry
  hook and consumed by the position hook) did not travel, so a snapshot
  taken between the two lost one `pw_mark()` sample. Render-path only, but
  it made snapshots differ by four bytes across an otherwise exact
  round-trip.

Both fixes confirmed interactively: cold restore resumes, and a snapshot
restores from anywhere in the game - from a different table, from the
loading screen, from the menu. That falls out of what the file holds. A
`.pfs` is the whole machine, not a table-scoped save: low RAM, CPU, DOS
state and the handle table are replaced wholesale, so whatever the guest
was doing is simply overwritten. The release identity check is what keeps
that safe, since the one thing that must still match is the install.

Still not covered: releases other than deluxe.

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
  `memmove` doesn't; that one case is how LZ unpackers expand runs -
  regression-tested in `tools/reptest.c`).
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
| `-video F` / `-videowav F` / `-videoscale N` | Frames and soundtrack for an encoder ([REPLAY.md, Videos](REPLAY.md#videos)) |
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
| `-scoredbg` | Per-attempt score, ball, player count and start/end cycles - Spike A of [Verification](VERIFY.md). Read-only |
| `-verify FILE` | The same information as one JSON object, for a machine rather than a person, plus the recorded-vs-actual comparison and the eligibility verdict. Implies `-scoredbg`. The text above is a report and free to change; this is an interface and is not |
| `-keepoverlay` | Keep a replay's isolated overlay instead of deleting it, so the guest's own writes (e.g. `TABLEn.HI`) can be inspected |
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
