# Upscaling notes

Parked ideas, nothing implemented. The question: can Pinball Fantasies be made
to look reasonable on a modern display?

## The hard limit

There is no higher-resolution version of the game hiding anywhere. The
playfield is a pre-drawn bitmap that the 16-bit game code paints into 64 KB of
VGA planes; `src/vga.c` (`vga_render()`, the 256-colour scanout path) reads
those planes back out as indexed pixels - 320x240 in Normal mode (other modes
in the table below). No geometry, no vector art, no
source assets at a larger size. "Render the game at 4K" is off the table in the
way it would work for a 3D game or a re-implementation.

Every option below is *scaling a small framebuffer image better*, not
producing more picture. (320x240 is the common case, but not the only one -
see the source-modes table below.)

One thing worth keeping in mind: the original CRT raster was 640x480 - Mode X
320x240 with CRTC scan doubling ([Emulator](EMULATOR.md)). So 2x nearest is
already the geometrically pixel-exact reproduction of what a 1993 monitor
showed. It is not the *visually* exact one: a real CRT also added beam
profile, phosphor structure, gamma and analog blur, which is exactly what the
table's ordered dithering was authored against. Anything past 2x nearest is
interpretation, not recovery.

## Source modes (the input is not always 320x240)

`vga_render()` in `src/vga.c` emits several sizes into `fb[1024*768]`
(`src/main.c`), and `plat_present()` scales whatever it gets:

| Source | Size out of `vga_render()` | Notes |
|---|---|---|
| Table, Normal | 320x240 | Mode X 256-colour, scan-doubled to a 480-line raster |
| Table, High (F5 / launcher) | 360x350 | 256-colour, ~71 Hz; needs 12 MIPS (README, EMULATOR.md) |
| Menu / table-select | 640x480 | 16-colour planar, `h_log * dup` with `dup = 2` when doubled |
| Text / setup screens | up to 640x400-ish (80x25 etc.) | host-rendered 8x16/8x8 font path |
| Mode 13h boot (`TIMER.BIN`) | 320x200 | `plat_present()` applies a 1.2x horizontal correction to reach 4:3 |
| Score panel | split-screen via line-compare inside the same frame | not a separate size; the DMD region rides in the main image |

Consequences for everything below:

- "Letterboxes to 4:3" is shorthand. `plat_present()` letterboxes to the
  *source* aspect (`w/h`, with the 1.2 correction only for 320x200), which is
  4:3 for the table and menu but ~1.03 for hi-res. Integer factors therefore
  differ per mode (see below).
- Any GPU backend needs a variable-size dynamic texture (1024x768 max covers
  every clamped output), not a fixed 320x240 one.
- The canonical image for screenshots, `-shotevery` and replay comparison is
  whatever `vga_render()` produced that frame at its native size - not
  "the 320x240 image".

## What the present path does today

`plat_present()` in `src/main.c` letterboxes to the source aspect (4:3 for the
table/menu - see above), then calls `SetStretchBltMode(COLORONCOLOR)` +
`StretchDIBits`. COLORONCOLOR is nearest-neighbour - it drops or duplicates
whole rows and columns.

At the default 960x600 window that lands on **2.5x in both axes**: `ar` is
1.333, the window ratio is 1.6, so the picture is drawn at 800x600 from a
320x240 source. Half the source columns get 2 destination pixels and half get
3, and the same vertically. Static screens hide it; a scrolling playfield makes
the uneven columns crawl. That artifact belongs to the scaler, not to the game.

The menu (640x480 planar, doubled from 640x240) lands on 1.25x - same problem,
finer. Hi-res 360x350 in the same window lands on ~1.71x (617x600 box);
text/mode-13h frames have their own factors. The crawl is worst on the
320x240 tables because the factor is largest there, not because the other
modes are exempt.

There is already a stubbed `if(integer_scale){ }` in `plat_present()`.
The flag behind it is `static int integer_scale = 0` with no CLI or launcher
wiring, so it is currently dead code - the shape is right, the switch is
missing.

## The options, cheapest first

| Option | What it does | Verdict |
|---|---|---|
| Integer scaling | Snap the destination rect down to the largest whole multiple that fits, letterbox the rest | Do this one. Uniform pixels, scroll stops crawling, costs nothing |
| Pixel-art upscalers | xBRZ, HQ4x, ScaleFX, Omniscale - edge-directed interpolation | Expect it to look bad here; test on a screenshot before writing code |
| CRT shaders | Scanlines, aperture mask, mild curvature | The most honest "modern screen" answer, and needs the pixels to draw a mask |
| ML upscaling | ESRGAN and relatives | Practically a non-starter |

**Integer scaling.** 320x240 at 8x is 2560x1920, which fits inside 4K; 1440p
takes 5x. Claims nothing about fidelity and stays entirely in the render path.

Per-mode ceilings for reference (largest whole multiple fitting a 4:3 box):

| Display (4:3 box inside it) | 320x240 | 360x350 | 640x480 |
|---|---|---|---|
| 1080p (1440x1080) | 4x (1280x960) | 3x (1080x1050) | 2x (1280x960) |
| 1440p (1920x1440) | 5x (1600x1200) | 4x (1440x1400) | 3x (1920x1440) |
| 4K (2880x2160) | 8x (2560x1920) | 6x (2160x2100) | 4x (2560x1920) |

At the default 960x600 window the table's integer fit is 2x (640x480,
centred) - a smaller picture than today's 800x600 fill, which is why this
wants to be opt-in (flag + launcher checkbox), not silently imposed.

**Pixel-art upscalers.** These round off stairsteps, but the Pinball Fantasies
playfield is heavily dithered 256-colour art and these filters read dithering
as edges and smear it into blobs. They are tuned for flat-shaded SNES/Genesis
sprite work. The score display and small text would probably survive; the table
art probably would not. Cheap to check on a screenshot first.

**CRT shaders.** The game was authored for a CRT at 480 lines and its dithering
was designed to blend on one. Looks good at high resolution specifically
because there are finally enough pixels to draw a mask with.

**ML upscaling.** Not realtime at any sane cost, and there are no static assets
to preprocess - every frame is composited by the game at runtime.

## What it would cost structurally

GDI caps us at integer scaling *plus blurry fractional*. `StretchBlt` offers
exactly one filter above nearest (`HALFTONE`, a box filter - blurry, no
better than bilinear) and no shader hook. Integer scaling itself needs no
backend: compute the integer rect and keep calling `StretchDIBits`. CPU-side
pixel-art filters (HQx, xBRZ) could also stay on the GDI path - they run on
the `fb` buffer before present at the cost of host milliseconds per frame.
What genuinely needs a GPU backend (D3D11 or OpenGL) is anything shader-side:
bilinear/bicubic fractional fill, ScaleFX-style filters, and CRT masks. That
backend is a variable-size dynamic texture (1024x768 max, ~300 KB/frame at
320x240 = ~18 MB/s at 60 Hz - trivial) plus a textured quad and a fragment
shader.

The architecture is already in the right shape for it.
`plat_present(const uint32_t *pix, int w, int h)` is a single function with a
clean signature, and the overlays are drawn in *window* pixels after the
stretch (`osd_draw_screen`, `rec_draw_dc`), so the OSD and REC badges stay
crisp at any scale. The emulator core would not be touched at all.

The real integration risk is not the shader, it is `present_phaselock`: the
present is timed against the emulated raster phase to avoid tearing. A
swapchain with its own vsync would fight that, and a poorly chosen present mode
adds a frame of latency to a game where flipper timing is the whole point.

Mitigation, if a GPU backend ever lands: keep the *decision* where it is
(the emu-phase gate in `main.c`, which picks the quiet span after vblank and
rate-limits to 240 Hz minimum spacing) and make the GPU submit fast and
unbuffered - immediate/no-vsync present (`DXGI_PRESENT_ALLOW_TEARING` style,
no render-ahead queue), never block the emu loop on vsync. The main loop
already measures present cost (`t_pres_w`); watch it and the `-balldbg` gap
numbers before/after. Note the status quo is not zero-latency either: GDI
`BitBlt` through the DWM compositor already buffers a frame on Win10/11, so
an immediate GPU present may match or beat it. Keep `-nophaselock` working
regardless - it is the bisect tool. Do not scale inside `vga_render()`: that
function's output is the canonical image, and touching it pollutes
snapshots, replay comparison and captures.

## Fidelity

Anything past 2x integer is cosmetic smoothing in the render path only. None of
it touches the emulated framebuffer, so screenshots (which read the framebuffer
before badges *and* the OSD notice - see the captures-first ordering in
`main.c`) and the replay comparison path keep seeing the unmodified native
image at whatever size `vga_render()` produced that frame. That invariant is
load-bearing for validation: no scaler, shader or backend may feed pixels
back into `fb`, snapshots or the replay hash.

## Review verdict and suggested order

What holds: the hard limit is real, the 2.5x-crawl diagnosis is correct
(verified against the `dw = dh*ar` path: 800x600 from 320x240 at 960x600),
the GDI assessment is right, and the phaselock-vs-vsync risk is the sharpest
observation in the file.

What needed correcting: the input is not always 320x240 (hi-res 360x350,
640x480 menu, text, 320x200 with aspect correction - table added above); the
"letterbox to 4:3" shorthand and the fixed-320x240 texture description were
oversimplified accordingly; `HALFTONE` deserves its blurry-fractional
honourable mention; and the dead `integer_scale` flag has no switch wired to
it yet.

Suggested order of work, cheapest first:

1. **Integer scaling on the GDI path** (half a day): in `plat_present()`,
   after the aspect box is computed, take
   `scale = min(dw/w, dh/h)` floored, clamp to >= 1 (if the window is smaller
   than 1x, fall back to the current fill rather than clipping), centre the
   resulting `w*scale x h*scale` rect, keep `StretchDIBits`/`COLORONCOLOR`.
   Wire a `-integerscale` flag plus launcher checkbox; leave the default off
   (or default on only when the fit is >= 2x) since the picture gets smaller.
2. **Offline pixel-art test before any code**: `F11`/`-shotevery` frames are
   pixel-clean PNG/PPM at native size - run xBRZ/HQ4x on a table screenshot
   (dithered art, ball, DMD text) and a menu screenshot with an existing tool
   and look at 4x. Expect the dithering verdict above to survive; if it does,
   skip CPU filters entirely.
3. **GPU backend only if 1 stops being enough**: D3D11 FL 9_1 (or GL 3.3),
   dynamic RGBA texture, point sampler (free integer/fractional-nearest) +
   bilinear sampler (fill without crawl, at blur cost), then an optional CRT
   pass. Shader notes: draw scanlines against the 480-line raster (each
   240-row becomes 2 scanlines - the doubling bit is the tell), aperture-mask
   needs >= 1080p to resolve triads, and palette comes from 6-bit DAC values
   expanded as `(v<<2)|(v>>4)` in `build_pal()`, so handle gamma in-shader.
   Present immediate, keep the phaselock gate untouched.
4. **Explicitly not**: ML upscaling (no static assets, runtime-composited
   frames, realtime cost, temporal instability), scaling inside `vga_render()`,
   vsync-locked swapchain.
