# Running Pinball Fantasies without DOSBox

Phase two: take the 25 files recovered in phase one and make the game run on
64-bit Windows 11, with no DOSBox and no pre-existing emulator of any kind.

**Result:** `pfemu`, a ~3,500-line 386 real-mode PC emulator written from
scratch in C. It boots the game's own launcher and runs the intro, the table
menu and Party Land itself — several times faster than real time, with the
keyboard and the game's own timing model working. Music plays too, all the
way through: an emulated 8237 DMA channel and Sound Blaster DSP carry the
driver's own `.MOD` mixer output through the intro, the menu and in-game on
all four tables (§5.12).

No external references about the game, its file formats or its protection
were consulted. Everything below was established by reading the binaries or
by watching them execute.

---

## 1. What was on disk

```
PINBALL.EXE     1,742   launcher
INTRO.PRG     345,678   intro + table-select menu
TABLE1..4.PRG 504,758 - 536,822  the four tables
*.SDR          2,883 - 11,886    sound drivers (11 of them)
SETSOUND.EXE    5,652   sound configuration utility
INTRO.MOD     252,870   music
TABLE1..4.MOD 210,760 - 219,668  music
MOD2.MOD       55,394   music
TIMER.BIN         253   raw code blob
SOUND.CFG          16   written by SETSOUND
CRACK.COM         673   shipped with the disks
```

`.PRG` is not a format — they are ordinary MZ executables with the extension
changed, presumably so that `DIR *.EXE` does not tempt anyone to start them
directly. They cannot be started directly: they depend on services the
launcher installs.

---

## 2. The launch chain

### 2.1 `PINBALL.EXE` is a shell, not the game

1,742 bytes, fully disassembled. It:

* shrinks its own memory block to `0x9D` paragraphs (INT 21h AH=4Ah);
* hooks **INT 9** with its own keyboard ISR — reads port 60h, acknowledges via
  port 61h, sends EOI to the PIC, and stores the last make-code at `cs:[0x2E]`;
* hooks **INT 24h** (critical error) so a missing file cannot produce the DOS
  "Abort, Retry, Fail?" prompt;
* installs its own API on **INT 65h**;
* EXECs `Intro.prg`;
* then loops: EXEC `Table<n>.Prg`, where *n* is a byte at `cs:[0x1C]`. *n* = 0
  means quit.

The filenames live in a 12-byte-stride table at `ds:0x3E`, each one prefixed
with `'$'` so that the same pointer doubles as the argument to INT 21h AH=09h
for the *"…not loaded! Reinstall program."* error message. A nice economy.

### 2.2 INT 65h — the launcher's API

| AX | effect |
|----|--------|
| `0000` | returns AH = flag at `cs:[0x1AC]`, BL = current table index |
| `FFFF` | set the next program to run: `cs:[0x1C] = BL` |
| `0100` | copy 6 bytes from `ES:BX` into the launcher |
| `0200` | copy those 6 bytes back out to `ES:BX` |
| other | return AL = last scancode seen by the launcher's INT 9, then clear it |

The 6-byte blob is how state survives a process switch: a table program stores
it, terminates, and the next program started by the launcher reads it back.
`PINBALL.CFG` is that same blob written to disk.

### 2.3 The sound drivers are TSRs

`SETSOUND.EXE` enumerates `*.SDR` with INT 21h 4Eh/4Fh, shows a text-mode menu,
and writes the chosen name into `SOUND.CFG` — 16 bytes for a driver with no
questions (`"NOSOUND.SDR\0\0\0d\0"`), longer when the driver has some: choosing
SoundBlaster adds the answers to "Base port address?" and "IRQ number?" and
produces 25 bytes.

Each game program then reads `SOUND.CFG`, EXECs the named `.SDR`, and the
driver installs itself on **INT 66h** and **INT 8** and returns via INT 21h
**AH=31h**, staying resident. Every program does this for itself — the intro
loads a driver, and so does each table.

The driver is far more than a sound device. It owns the machine's *time*:
INT 8 is its interrupt, and it drives a list of scheduled callbacks that the
game registers through INT 66h. This turns out to matter enormously (§5).

`TIMER.BIN` is a raw code blob — loaded and called, not an MZ. It sets mode
13h, gates the speaker off, and runs a calibration loop writing to
`A000:FFF0`: a machine-speed measurement.

---

## 3. Why an emulator

The alternatives were considered and rejected:

* **Static recompilation to C.** The tables are ~500 KB of 16-bit code with
  computed jumps, self-modifying protection code and data interleaved with
  code. Recovering a correct control-flow graph statically is not realistic.
* **A DOS-API shim on top of a Win32 process** (WINE-style). Useless here: the
  game does almost nothing through DOS. It programs the VGA CRTC directly,
  reprograms the PIT, hooks IRQ 0 and IRQ 1, and phase-locks its frame timer to
  the CRT by polling port 3DAh. A shim would have to emulate all of that
  anyway, and would still need a CPU for the 16-bit code.

So: emulate the machine. The job is bounded — one program, one graphics mode
family, one timer, one keyboard — and everything the game does is observable.

Toolchain: MSVC 14.29 from the VS2019 Build Tools, the only C compiler on the
box. Capstone under Python for static disassembly.

---

## 4. pfemu

```
cpu.c   682  386 real-mode interpreter, eager flags, 16/32-bit op/addr size,
             full 0F group, string ops with REP
vga.c   489  memory dispatch + VGA: text, planar write modes 0-3, latches,
             chain-4, unchained mode X, split screen, scan doubling, DAC
dev.c   425  8259 PIC pair, 8253 PIT, 8042-style keyboard, port map, CRT timing
bios.c  361  INT 10h/11h/12h/13h/15h/16h/1Ah, IRQ0/IRQ1, IVT set-up
dos.c   779  MCB chain, PSPs, file handles, FindFirst/Next, EXEC, TSR, .COM
main.c  364  Win32 window, presentation, keyboard, main loop, test harness
sound.c 364  8237 DMA, Sound Blaster DSP, waveOut sink, WAV capture
pfemu.h 105  shared declarations
             ------
             3,569 lines total
```

### 4.1 Reaching native code from the guest

BIOS and DOS handlers are native C. The guest reaches them through an
otherwise-invalid opcode. Each IVT entry points at a four-byte stub in the ROM
area:

```
F000:(0x1000 + n*4):   0F FF nn CF
```

`0F FF` is decoded by the CPU core as *"call native handler nn"*; the core
performs the IRET itself afterwards unless the handler asks it not to
(`cpu_no_iret()`, used by EXEC, by terminate, and by the blocking INT 16h).

INT 8 is the exception: it is genuine 8086 machine code assembled into the ROM
image at F000:0E00, because the game's sound driver hooks INT 8 and chains to
INT 1Ch, and a native stub cannot be chained to.

### 4.2 Time

`emu_time` advances by a fixed instruction rate (6 MIPS by default, `-ips`).
The main loop runs the guest until emulated time catches up with wall time
times `-speed`.

Two refinements were forced by the game and are described in §5:

* `emu_now()` folds the outstanding instruction count into the clock on every
  read of a *polled* register (3DAh, the PIT counters, port 61h), so that
  short pulses cannot fall between samples;
* the main loop never runs past the next timer deadline, so IRQ 0 lands on the
  instruction it is due on rather than up to a batch later.

### 4.3 CRT timing is computed, not assumed

`vga_timing()` derives the frame period from the dot clock (misc output bits
2-3, halved if SR1 bit 3), the character width (SR1 bit 0) and the CRTC
horizontal and vertical totals:

* 70.09 Hz for text and mode 13h (449 lines)
* 59.71 Hz for the 480-line mode-X screens (527 lines total)

Both numbers are consequences of the register values the game writes, not
constants in the emulator.

---

## 5. The bugs

This is the interesting part. Each of these presented as "the screen is black"
or "it hangs", and each had a specific mechanism.

### 5.1 The PIC was left masked

`pic_init` started the master's IMR at `0xFF`, so no IRQ ever reached the CPU.
A real BIOS POST leaves it at `0xB8` (IRQ 0, 1, 2 and 6 enabled).
*Symptom:* the sound-setup menu ignored the keyboard and the BIOS tick counter
stayed at 0.

### 5.2 Odd/even text addressing was off by 2×

I mapped CPU address `A` to plane offset `A & ~1`; the CRTC address counter
wants `A >> 1`.
*Symptom:* text-mode screens rendered as three overlapping copies side by side.

### 5.3 Blocking INT 16h deadlocked

The handler rewinds onto its own stub and idles until a key arrives — but
`cpu_interrupt` had cleared IF on entry, so no keyboard IRQ could ever wake it.
Fixed by restoring IF from the caller's flags image at `ss:sp+4`.

### 5.4 INT 21h AH=31h (TSR) was missing

Sound drivers got an error return and their memory was freed, which later
starved the EXEC of the intro.

### 5.5 Attribute register 0x11 vs 0x10

INT 10h AH=10h/AL=02h sets 16 palette registers **and the overscan register**,
which is AR `0x11`. I wrote the 17th byte to AR `0x10` — the attribute mode
control — clearing the 256-colour bit in the process.

### 5.6 CRTC scan doubling was ignored

The playfield is unchained **mode X, 320×240**: SR4 = 0x06 (chain-4 off),
480-line CRTC timing, and CRTC 09 = 0xC0 — whose bit 7 is *scan doubling*, so
480 scanlines display 240 rows. Rendering 480 rows showed both pages of the
double buffer stacked on top of each other.

The table-select and protection screens are a different mode again: the same
480-line double-scanned timing but **16-colour planar**, i.e. 640×240 with 2:1
pixels. Confirmed by deliberately rendering at 640×480 and seeing the screen
twice.

### 5.7 Memory fragmentation, and a deliberate deviation

`INTRO.PRG` shrinks itself to 238 KB (AH=4Ah, BX=0x3B91) and then EXECs the
`.SDR` driver. DOS gives the child the largest free block, which starts
immediately above the intro, and the driver goes resident there with AH=31h.

When the intro exits, DOS frees the intro's 238 KB but *not* the TSR. The free
store is then 238 KB below the driver and 394 KB above it — and `TABLE1.PRG`
needs **524 KB contiguous** to load (it shrinks to 330 KB only after it is
running). So the next program can never start, and every table switch would
leak another driver.

Nothing in the launcher or the intro releases it: neither contains an INT 21h
AH=49h, an AH=58h allocation-strategy change, or a call to the driver's own
uninstall entry — and the driver *has* one, at a routine that frees its
environment block and then its own PSP block.

pfemu therefore treats a resident child as owned by its parent and releases it
when the parent exits (`mcb_free_children`). This is the one deliberate
departure from strict DOS semantics, and it comes with a consequence — see
§5.10.

*(This remains the one thing I could not explain about the shipped game. The
arithmetic is not in doubt: 524 KB does not fit in 394 KB. Something about the
memory map on the machines this was tested on must have differed.)*

### 5.8 Clock resolution: the sound driver's phase-locked loop

The first hard one. After switching from a hardcoded 70 Hz retrace to computed
CRT timing, the game hung with an all-black palette.

The driver contains a calibration loop. Written out:

```
        mov dx,40h ; mov es,dx ; mov dx,es:[63h] ; add dl,6   ; dx = 3DAh
  wait: in al,dx ; test al,8 ; jnz wait          ; wait for vsync to fall
        in al,dx ; test al,8 ; jz  $-3           ; wait for vsync to rise
        mov al,30h ; out 43h,al                  ; timer 0, mode 0, one-shot
        mov bx,di  ; out 40h,bl ; out 40h,bh     ; reload = DI
        xor bx,bx  ; sti
  cnt:  mov dx,3DAh
        in al,dx ; test al,1 ; jnz $-3           ; wait for bit 0 to fall
        in al,dx ; test al,1 ; jz  $-3           ; wait for bit 0 to rise
        inc bx
        jnz cnt                                  ; ISR sets BX=FFFFh to break out
        cli
        cmp bp,[45Eh]                            ; BP = the count the ISR captured
        ja  toobig
        jb  toosmall
        loop again                               ; ten exact matches to accept
```

with a temporary INT 8 handler installed for the duration:

```
        push ax ; mov al,20h ; out 20h,al        ; EOI
        mov bp,bx                                ; capture the count
        mov bx,0FFFFh                            ; make the INC BX above wrap to 0
        pop ax ; iret
```

So: start a one-shot timer, count edges of 3DAh bit 0 until it fires, and
adjust the reload until the count equals a target exactly, ten times. It is a
phase-locked loop that ties the game's frame timer to the actual monitor.

Three separate defects fell out of it.

**(a) The clock only moved every 64 instructions.** Those bit-0 pulses are
about 6 µs long; 64 instructions at 6 MIPS is 10.7 µs. The pulses fell between
samples. Fixed with `emu_now()`, which makes every polled register read exact
to the instruction.

**(b) IRQ 0 was delivered up to 64 instructions late.** The one-shot's start
time was quantised the same way, and the interrupt was only checked at batch
boundaries, so the captured count jittered by a third of a scan line. Fixed by
computing the reload deadline with `emu_now()` and by never letting the main
loop run past the next timer deadline.

**(c) 3DAh bit 0 must pulse during vertical blanking.** This is the one I got
wrong twice, and the driver settled it.

The obvious reading of bit 0 is "the display is not active", which would make
it sit at 1 for the whole 47-line vertical blanking interval. Under that model
the edge count as a function of the timer reload has a flat step 1,718 timer
ticks wide, right where the driver has to land. The loop walks across a flat
step 20 ticks at a time, gives up, and restarts — forever. The trace shows it
exactly: the count pinned at 480 while the reload marched down from 24,940 to
20,370 over 45 emulated seconds, restarting every ~2.4 s.

Setting bit 0 for the ~3 lines of vertical *retrace* only is better but still
leaves a small flat step in the same place, and the loop still misses.

With bit 0 following **horizontal retrace alone** — pulsing once per scan line
for all 527 lines of the frame — the curve becomes a staircase of roughly one
line (38 ticks) per step. A residual flat step of about 100 ticks survives near
the target, but the loop crosses that in five rounds instead of eighty-six, and
locks in about 25 rounds altogether:

```
[pll] reload=24700 bp=02B4     [pll] reload=19950 bp=020D
[pll] reload=23450 bp=0288     [pll] reload=19930 bp=020D
[pll] reload=22530 bp=0267     [pll] reload=19921 bp=020C
   ...                         [pll] reload=19921 bp=020B cx=0009   <- match
[pll] reload=20030 bp=020D     [pll] reload=19921 bp=020B cx=0008
```

It settles on a reload of 19,921 — a 59.9 Hz game tick against a 59.71 Hz
refresh — then restores the old INT 8, switches the timer back to mode 3, and
the game proceeds.

The driver's algorithm only terminates if the count is *strictly monotone* in
the reload. That is a hardware fact deduced from software: the card this was
written for kept bit 0 pulsing through vertical blanking. The driver is using
it as a scan-line clock.

### 5.9 PIT mode 0 is a one-shot, not a rate generator

With the PLL locked, the intro still showed nothing and quit after five
seconds. An execution ring buffer (§6) showed why: the guest was executing the
BIOS data area as code, at `0000:00CD`, `0000:01CD`, …, until it happened to
run into a `CD 20` — an INT 20h — and terminated.

Walking back: the last real instruction was inside the driver's INT 8 handler,
at an `lcall [si-4]` through an event-list entry whose far pointer was
`0000:0000`.

The event list lives in the driver's data segment. Entries are 9 bytes — key,
delay, priority, far pointer — registered through INT 66h functions 0Bh and
0Ch; the ISR walks the list, schedules the next timer interval from the current
entry, and calls the *previous* entry's callback. It wraps when the current
entry's delay equals a computed sentinel. Dumping the live table showed two
valid entries and a terminator, but the walk pointer had advanced three times
without wrapping.

The cause was mine. I treated PIT channel 0 as periodic in every mode. Mode 0
is *interrupt on terminal count*: **one** interrupt per count written, after
which the output stays high until the count is loaded again. The calibration
loop uses mode 0 — so after it finished, my spurious extra interrupts arrived
in the window between the driver putting its real INT 8 handler back and
finishing the initialisation of the list that handler walks. Each spurious
interrupt advanced the walk pointer past the end of the list.

Fixing mode 0 to fire once fixed the intro.

### 5.10 A released TSR leaves its interrupt vectors dangling

The consequence of §5.7. Releasing the driver's memory left INT 8 and INT 66h
pointing into it; `TABLE1.PRG` then loaded over that memory, and the first
timer tick jumped into the middle of the table's bitmap data.

The trace is unambiguous — the last good address is the driver's INT 8 entry
`3CAB:05BF`, and the very next instruction executed is `3CAB:4B00`, which is
sprite data.

Since pfemu is the one pulling the memory out from under the driver, pfemu
does what the driver's own uninstall would have done: it snapshots the IVT when
a child is EXEC'd, and when it releases that child's block it restores any
vector that points into it.

```
[dos] releasing resident child 3C9B of 0104
[dos]   int 08 restored 3CAB05BF -> F0000E00
[dos]   int 66 restored 3CAB0040 -> F0001198
```

---

## 5.11 Reading the controls out of the binary

The scrolling in-game help is unreadable from screenshots, so the key map was
taken from the table's own INT 9 handler instead (`TABLE1.PRG`, image offset
`0x3F34`). It is a flat chain of compares:

| scan code | effect |
|-----------|--------|
| `2A` / `38` / `1D` | left flipper — set bit 1 of `[2F2A]`, then INT 66h AL=11h (the sound driver's "play sample") |
| `AA` / `B8` / `9D` | left flipper released — clear bit 1 |
| `36` | right flipper — set bit 0 |
| `B6` | right flipper released |
| `E0 50` / `E0 D0` | plunger: pulls the plunger sprite pointer `[23A9]` from `5E65` to `5E08` and back |
| `39` / `B9` | space — a separate held flag at `[2311]` |
| `57` / `58` | F11 / F12 — write `0B` / `0C` to `[3555]` |
| `E0 49` / `E0 51` | recognised and ignored |

Three keys for each flipper (shift, alt and control, left and right) is a
comfort feature, not redundancy — and the fact that the flipper branch calls
INT 66h directly from inside the keyboard interrupt is why the sound driver has
to be resident for the game to feel right.

One consequence for pfemu: the host had bound F12 to "quit", which the game
uses. Quitting moved to Scroll Lock, which nothing in the game reads.

## 5.12 Sound

`SETSOUND.EXE` runs under pfemu, so the way to find out what the game wants
from a sound card is to let it ask. Selecting **SoundBlaster** produces two
questions — base port (default 220h) and IRQ (2/3/5/7, default 7) — and writes
a 25-byte `SOUND.CFG`. No autodetection: the driver is told where the card is.
That removes the worst risk from emulating one.

With that config, `-iotrace` shows `SBLASTER.SDR` programming the machine, and
the whole protocol falls out in twenty lines:

```
out 0Ah,05      mask DMA channel 1
out 0Ch,00      clear the address flip-flop
out 0Bh,59      channel 1, memory->device, auto-init, single transfer
out 83h,08      page 08   ->  buffer at physical 08:0020
out 02h,20 / 00 offset 0020
out 03h,2F / 2A count 2A2Fh, a 10,800-byte ring
out 226h,1 / 0  DSP reset; poll 22Ah until it answers AAh
in  22Ch        wait for the write-buffer-ready bit
out 22Ch,D1     speaker on
out 22Ch,40 AD  time constant ADh -> 1000000/(256-173) = 12,048 Hz
out 22Ch,14 1F FD   8-bit DMA output
out 0Ah,01      unmask channel 1 - playback runs
```

So it is plain 8-bit auto-init DMA playback. The driver's software MOD mixer
fills one half of a 10,800-byte ring while the card plays the other. It finds
out where the card has got to by writing port 0Ch to clear the flip-flop and
reading the channel's *current* address back from port 02h — which is why the
DMA controller has to expose a genuinely advancing address, not just accept
writes.

What pfemu needed, all in `sound.c`:

* **An 8237 DMA controller.** Address and count registers with the low/high
  flip-flop, mode, single and all mask registers, master clear, and the page
  registers (83h is channel 1). A `dma_fetch()` that reads a byte from
  `page<<16 | addr`, advances, and on count underflow either reloads from the
  base registers (auto-init) or masks the channel.
* **A Sound Blaster DSP.** The reset handshake (write 1, write 0, answer `AA`),
  the command/data port with its argument state machine, commands 10h, 14h,
  1Ch, 40h, 48h, D0h/D4h, D1h/D3h, E0h, E1h, F2h, and the read-buffer-status
  port at 22Eh which doubles as the IRQ acknowledge.
* **A pacing tick.** `sb_tick()` runs from `dev_tick()`, converts elapsed
  emulated time into a number of samples due at the programmed rate, pulls that
  many bytes through the DMA channel, converts unsigned 8-bit to signed 16-bit,
  and raises the card's IRQ at each block boundary.
* **A host sink.** Win32 `waveOut` with eight small buffers; blocks are dropped
  rather than queued if the emulator ever runs ahead, so audio stays live
  instead of drifting.

One bug had to be fixed before any of this could run: on the Sound Blaster path
the intro takes an error branch that calls INT 21h AH=49h with `ES=0000`.
`(uint16_t)(0-1)*16` is `0xFFFFFFF0`, which indexed straight off the end of the
RAM array and killed the host process. Segment validation now rejects it, as
DOS would.

Measured on a 30-second real-time capture (`-wav`): 26 seconds of audio at
12,048 Hz, peaks near ±16,000 of full scale, RMS moving between 1,900 and 4,000
as the music does, and dominant partials at 44–56 Hz, 174 Hz, 247 Hz, 265 Hz,
329 Hz and 500 Hz. That is the intro `.MOD` being mixed by the game's own
driver code and played through an emulated DMA channel.

### 5.12.1 Where it stopped, and why it was not a sound bug

For most of this project the Sound Blaster path worked for the intro and the
table-select menu and then broke: picking a table brought up its own instance
of `SBLASTER.SDR`, that instance never received the module filename, and it
tried to open a file whose name it read out of its own ProTracker period table
— the failing open was literally `"X 03h ( 03h ..."`, which is `856, 808, …`
as characters. The table took an error path and exited.

The handoff is narrow and well-located, so `-intwatch 66` puts the two paths
side by side. The intro passes its `$`-terminated list
(`"Intro.Mod\0$Mod2.Mod\0$"`) to the driver through INT 66h AL=12h with `DS:DX`
pointing at its own data. The table's equivalent string,
`"TABLE1.MOD\0$table1.hi"`, lives at image offset `19B40h`, and its AL=12h call
was arriving with `DS:DX = 556F:0000` — `556F` being the *driver's* own data
segment, not the table's `1AC8`.

The table's call site says why:

```
0124:62e0  int 21h          ; AH=4Bh - EXEC its own copy of the .SDR
0124:62e2  mov al,13
0124:62e4  mov dx,3a65
0124:62e9  int 66h
0124:62eb  mov ax,0012
0124:62ee  mov dx,0         ; sets DX, and never reloads DS
0124:62f1  int 66h          ; DS:DX must be the module-name list
```

It sets `DX` and trusts `DS` to have survived the EXEC eight bytes earlier.
`dos_exec` was saving only the caller's `SS:SP`, so the parent resumed with
`DS`, `ES` and `BX`..`BP` exactly as the child had left them — and the child
had gone resident with `DS` pointing at itself. The intro never noticed,
because its own path reloads `DS` between the EXEC and its AL=12h call.

Real DOS hands the parent its register set back across EXEC. The interface
documentation does not promise it — the usual note is that EXEC destroys
everything but `CS:IP` — but programs depend on it, DOSBox models it explicitly
for that reason, and this one depends on it. `Proc` now carries `AX`..`BP`,
`DS` and `ES` across the call and `dos_terminate2` restores them, with the
`AX=0` and `CF=0` of a successful EXEC layered on top.

With that one change the AL=12h call arrives as `1AC8:0000`, the driver opens
`TABLE1.MOD`, and the music plays. All four tables load their own module.

So what had looked like an unsolved sound problem was a DOS bug that only the
sound path was strict enough to expose. Worth remembering given how much of
§5 above is hardware: the last piece of the sound work was not sound.

## 5.13 The manual-lookup protection

The intro shows *"MANUAL PROTECTION — PLEASE ENTER WORD 12 ON LINE 7 AT PAGE
23"* and will not continue without the right word. `CRACK.COM`, which shipped
on the disks, rewrites one byte of `INTRO.PRG` at file offset 239,232: the
`73` (JNC) of

```
81 FB E7 51    cmp bx, 51E7h
73 32          jnc +32h        ->  EB 32   jmp +32h
```

becomes `EB`, so the branch is always taken and any word is accepted. The
screen still appears — the patch does not remove it, it just stops caring what
you type.

pfemu originally ran `CRACK.COM` itself (which is what forced `.COM` loading)
and lived with a modified game file. It now does the same edit to the *loaded
image* at EXEC time, after checking that all six signature bytes are present,
so a different build is left alone rather than corrupted. `FANTASY/INTRO.PRG`
is byte-for-byte as it shipped; `-nopatch` disables the patch and restores the
original behaviour. Verified equivalent: patching the file with `-nopatch`, and
patching in memory with a pristine file, reach the same screen and accept the
same input.

## 5.14 The boot black screen, and shortening it

The ~5 s of black before the first intro screen (also present on period
hardware) breaks down, measured in emulated time at 1x: 0–1.6 s of real setup
(EXEC chain, `.SDR` install, LBM depack with its `PBM BODY` parser), 1.6–3.8 s
of sound-driver PLL calibration, 3.8–4.5 s of scheduler spin-up, MOD
processing and fade-in. `TIMER.BIN` was suspected and exonerated (a ~100k
instruction speed grade).

The calibration runs the shared routine twice per driver load (so every table
switch pays it again): pass 1 with target 0, returning the measurement floor
(di ~= 1, saved for the tempo ratio `[6BBE]-[6BB8]` over `[6BBC]`), a frame
measure, then pass 2 with the real line-count target, locking the PIT reload
(~19771 here, ~19921 in an earlier run — a band, not a constant). Both passes
sweep geometrically from the `mov di,1CE8` seed, ~110 rounds x ~17 ms.

`src/fantasies.c` shortens this, Fantasies sessions only, `-nopatch`-gated,
all derived from the image with strict bail-outs (anything unrecognised keeps
the slow path): the seed is preset to `0x4D3B` (pass 2 then locks in ~10
rounds), and pass 1 is NOP'd with its save cell preset to 1 — inside the
natural 0–20 dither of that result, so the tempo ratio shifts <= 0.1%, the
same as run-to-run jitter. Verified by `-pll` trace (lock, scheduler
handoff), `-mem` (`[6BB8]` reads back 1), screenshot timeline (first light
~4.5 s -> ~3.2 s) and a NOSOUND scratch-install boot; 8 of 11 drivers derive
cleanly (ADLIB/INTERNAL/THING keep the slow path). One jitter source remains:
a stale pending IRQ0 can spoil round 1 (~1/3 of runs), costing an extra ~0.3 s
detour that still converges — the controller is self-correcting either way.

### 5.14.1 Reverted: the pass-1 skip corrupted memory outside the driver

The verification above (screenshot timeline, `-mem`, a NOSOUND boot) missed
the case that mattered: it never checked a *table's* own driver load, and
never compared a sound-enabled boot's doc-check screen pixel-for-pixel
against a silent one. Both broke. With a sound driver loaded, the intro's
manual-lookup ("doc-check") screen rendered with visibly corrupted text; with
sound on, Table 4 crashed on load with corrupted graphics. `-nopatch` (which
disables this shortcut along with the other two Fantasies patches) made both
symptoms disappear, which is what pointed back here.

Root cause: `fantasies_patch_sdr()`'s pass-1 skip doesn't just NOP the call
and preset a register — it computes a *memory* address (`dsbase*16 + dcell`)
and writes a preset result there, so the driver's own calibration code reads
back the value it would have computed itself. `dsbase` comes from a `push
imm16 / pop ds` immediate baked into the `.SDR` file — traced live at
`[01E2:6BB8]` (linear `0x089D8`) for `SBLASTER.SDR`, identically whether the
intro or a table loaded it, and identically before and after checking
whether it needed relocation (it didn't: the value is a fixed constant in
the file, not a relocation-table entry, so it has nothing to do with where
*this* EXEC's driver or caller actually landed in memory). That fixed address
happens to fall inside whichever process is resident there at the time: the
intro's own memory when the intro loads the driver (corrupting a couple of
bytes near the doc-check text — the "harmless-looking" case the original
verification happened to land on), or a table's own live code/data when the
table loads its bundled copy of the same driver (an instant crash).

Disabled: `dos.c` no longer calls `fantasies_patch_sdr()` (the function stays
in the tree, unused, since the signature/offset reverse-engineering is
expensive to redo). The full two-pass calibration always runs now — back to
the ~5 s boot black screen `-nopatch` used to restore. Re-enabling the
shortcut needs the poke address checked against the actual owning process's
MCB block before writing, not just pattern-matched out of the file.

## 6. Debugging tools

Almost all of the time went into *locating* faults, not fixing them. The
emulator grew a small instrument panel, and every one of these earned its
keep:

| flag | what it does |
|------|--------------|
| `-secs N -speed X` | run headless for N wall seconds at X times real time |
| `-shot f` / `-shotevery N` | write PPM screenshots, one or a sequence |
| `-keys "t:sc:updown,…"` | drive the keyboard from a script, at emulated times |
| `-xring` | ring buffer of the last 8,192 executed addresses, printed at exit with straight-line runs collapsed |
| `-trap LO HI` | stop the moment execution enters a linear address range, then print the ring |
| `-trapexit` | stop when a child process terminates, and say who called |
| `-pll N` | trace N timer-0 reloads with the guest's DI/BP/CX and the current CRT geometry |
| `-mem LIN` | hex dump 256 bytes of guest memory at exit |
| `-intwatch NN` | log every `INT NN` with AX, BX and the real calling address |
| `-dosdbg` | log every INT 21h call with the caller's address |
| `-iotrace N` | log the first N accesses to the DMA, page, Sound Blaster and OPL ports, with the calling address |
| `-wav FILE` | capture everything sent to the host audio device as a WAV |
| `-nopatch` | leave the manual-lookup check in place |
| `-force256`, `-nodbl`, `-oldtiming` | force renderer/timing variants, to bisect display faults |

Two of these deserve comment.

**The execution ring** is the single most valuable tool in the box. When a
guest runs off into data, the final CS:IP tells you nothing — it is wherever
the garbage walked to. The ring tells you the last address that was real code.
Both §5.9 and §5.10 were solved in one run each, once it existed.

**Reproducible headless runs** matter more than a debugger here. `-speed 8
-secs 3 -keys …` reproduces a specific frame of gameplay deterministically in
a few seconds of wall time, which makes bisecting a rendering or timing change
a matter of diffing two PPMs.

The screenshots are also how every graphics conclusion in §5.6 was reached:
render it wrong on purpose, look at how it is wrong, and read the mode off the
failure.

---

## 7. Static analysis helpers

* `re/mz.py` — MZ header parser. Every "how big is this really" question went
  through it. `TABLE1.PRG`: header says 1,049 pages, last page 246 bytes,
  32 header paragraphs → 536,310 image bytes = 33,520 paragraphs = the 524 KB
  of §5.7.
* `re/d16.py` — capstone 16-bit disassembler wrapper.
* `re/scan.py` — byte scanner for INT/IN/OUT usage, for finding the hardware
  touch points in a 500 KB binary quickly.
* The reconstructed MS-DOS port source (`historicalsource/pinballfantasies`
  on GitHub: `INTRO.ASM`, `FANTASIE.ASM`, table sources) — a third-party
  reconstruction, not the original code, but invaluable as a *map*: symbol
  names and comments (`julius` loading both 16-palettes, `CHANGE16PAL` with
  "set 2 palette modes (on rasterint)", the `dumretf`/`creatretf` VBLANK /
  RASTERINT callbacks) predict exact port traffic that the emulation trace
  then confirms. Solved the §21 menu palette flash in OPTIMIZATIONS.md this
  way. Caveat: reconstructed, so trust-but-verify against live `-mem`
  dumps — same addressing trap as above applies in reverse.

One trap worth recording: the resident part of a `.SDR` is **not** at the file
offset you expect. The driver copies its resident block down over its own
initialisation code, so a runtime offset is the file image offset plus `0x72`.
Disassembling the file and then trusting those addresses against a live dump
produces nonsense. Dumping live memory (`-mem`) and disassembling *that* is the
reliable path.

---

## 8. Deviations from real hardware

Stated plainly, because they are the parts a future reader should distrust
first:

1. **Resident children are released with their parent** (§5.7), and their
   interrupt vectors are restored (§5.10). Strict DOS keeps a TSR forever.
   Without this the game cannot load a table at all.
2. **3DAh bit 0 follows horizontal retrace only** (§5.8c), so it pulses through
   vertical blanking. Deduced from the sound driver's requirements, not from a
   datasheet.
3. **Instruction timing is uniform** — every instruction costs 1/6,000,000 s.
   Real 386 instruction costs vary by an order of magnitude. The game does not
   appear to care, because everything it times, it times against the CRT or the
   PIT.
4. **No x87.** The intro executes a handful of x87 escapes; they are logged and
   skipped. Nothing visibly depends on them.
5. **The manual-lookup protection is patched out of the loaded image** (§5.13),
   rather than being answered. `-nopatch` turns that off.
6. ~~The sound-driver PLL calibration is short-circuited~~ — tried, reverted
   (§5.14.1): the shortcut wrote a preset result to a memory address computed
   from a constant in the `.SDR` file rather than from where the driver
   actually landed, corrupting the doc-check screen with sound on and
   crashing Table 4. The full two-pass calibration always runs now; the ~5 s
   boot black screen is back to its original length.

---

## 9. What works

* `SETSOUND.EXE` — driver menu renders, arrow keys and Enter work, writes
  `SOUND.CFG`.
* `CRACK.COM` — runs under pfemu (this is what forced `.COM` loading) and
  patches `INTRO.PRG` at file offset 239,232, `73` (JNC) → `EB` (JMP),
  defeating the manual-lookup protection. The original is kept as
  `INTRO.PRG.orig`.
* `PINBALL.EXE` → `Intro.prg`: the 21st Century griffin, Digital Illusions and
  Frontline Design logos with their palette fades, the scrolling title, and the
  table-select menu with all four table banners.
* `Table1.Prg` (Party Land) loads and runs, in attract mode and as a real game.
  **F1** adds a player and the view locks to the bottom of the table; the ball
  feeds into the shooter lane; **Down arrow** pulls and releases the plunger and
  the ball launches; the camera follows it up the table and the score panel
  switches from `PLAYERS 1` to `PLAYER 1 / BALL 1`. Both flippers respond.
  Keyboard behaviour was independently confirmed by the user playing the game
  in the window — everything keyboard-related works.
* The score panel is rendered through the CRTC's split-screen (line compare)
  register and comes out as the correct amber dot-matrix display.
* **The manual-lookup protection**, defeated in the loaded image rather than on
  disk (§5.13). `FANTASY/INTRO.PRG` is byte-for-byte as it shipped.
* **Music, through the whole game.** With `SOUND.CFG` set to `SBLASTER.SDR`
  (base 220h, IRQ 7) the game's own MOD mixer plays through an emulated 8237
  DMA channel and Sound Blaster DSP, out to Win32 `waveOut` at 12,048 Hz —
  the intro, the table-select menu, and in-game on all four tables, each
  loading its own `.MOD` (§5.12.1). `-wav <file>` captures it for offline
  checking; a 75-second real-time capture runs from `Intro.Mod` into
  `TABLE1.MOD` with no silent gap, the dominant partials moving from the
  intro's 55/82/110/165/221 Hz to the table's much brighter
  528/1046/1761/2092 Hz. `NOSOUND.SDR` still plays the whole game silently.
* Keyboard input reaches the game through the launcher's INT 9 hook and, in
  the tables, through their own.
* The emulator sustains well above real time — a 105-second emulated run
  completes in 35 seconds of wall time at `-speed 3`, i.e. roughly 3× real
  time with headroom to spare on one core.

## 10. What does not

* **Any sound card but the Sound Blaster.** `ADLIB.SDR` would need an OPL2
  synthesiser on ports 388h/389h; `INTERNAL.SDR` is a PC-speaker PWM driver —
  the only one that touches ports 61h and 42h — and would need the speaker
  output sampled into the audio stream; `GUS.SDR`, `PAS16.SDR`, `SM2.SDR` and
  `THING.SDR` want hardware nobody has modelled here. The OPL stub answers its
  status port and does nothing else. Sound Blaster is the path that works.
* **No automated test suite.** Verification is by screenshot, frame diff, WAV
  analysis and playing it. That catches "the picture changed", not "the right
  byte changed", and it is the clearest weakness in this work.
* **The other three tables** load, by the same mechanism as Party Land, but
  have not been played through.
* The intro's music file, `INTRO.MOD`, is 20 bytes shorter than its own header
  implies (61 patterns and 189,342 sample bytes call for 252,890; the file is
  252,870). `TABLE1.MOD` and `MOD2.MOD` are exact, and phase one verified all
  25 files against the CRCs stored in the archive — 25/25 — so this is how the
  game shipped, not an extraction error. It affects only the tail of the last
  sample of the intro music.

---

## 11. Building and running

```
cd pfemu
build.bat          ; calls vcvars64.bat, then one cl /O2 command
pfemu.exe -d ..\FANTASY
```

Keys are passed straight through: the game's INT 9 hooks see XT set-1 scan
codes taken from the Win32 `WM_KEYDOWN` lParam, which is already the right
encoding, with `E0` inserted for extended keys. **Scroll Lock** quits the
emulator — F12 belongs to the game.

In the menu, F1–F4 pick a table. In a table: **F1** adds a player, **Down
arrow** works the plunger, **Shift / Alt / Ctrl** (either side) are the
flippers, **Space** nudges.

### Integrity note

The game writes to its own data files: `INTRO.PRG` seeks near the end of
`Intro.Mod` and rewrites two bytes there, and the tables create `.hi` files.
Passed straight through, those writes change `INTRO.MOD` so that it no longer
matches the CRC recorded in `FANTASY.TWO` — which silently invalidates every
integrity check made afterwards. pfemu did exactly that for most of this
project before I noticed.

Every write now goes to a copy instead. A file opened for writing is copied
into `FANTASY/PFEMU-STATE/` on first use and the handle is opened there; a file
that already has a copy is read from the copy. The installed files are never
written, and the game's own state still persists across runs. Sol's runtime
does the same thing, and the idea is taken from it.

A fresh run of `pfx.py` verifies all 25 files against their archive CRCs, and
all 25 are now byte-identical to what is installed.
