# Upscaling notes

Parked ideas, nothing implemented. The question: can Pinball Fantasies be made
to look reasonable on a modern display?

## The hard limit

There is no higher-resolution version of the game hiding anywhere. The
playfield is a pre-drawn bitmap that the 16-bit game code paints into 64 KB of
VGA planes; `src/vga.c` (the 256-colour scanout, around line 553) reads those
planes back out as 320x240 indexed pixels. No geometry, no vector art, no
source assets at a larger size. "Render the game at 4K" is off the table in the
way it would work for a 3D game or a re-implementation.

Every option below is *scaling a 320x240 image better*, not producing more
picture.

One thing worth keeping in mind: the original CRT raster was 640x480 - Mode X
320x240 with CRTC scan doubling ([Emulator](EMULATOR.md)). So 2x nearest is
already the pixel-exact reproduction of what a 1993 monitor showed. Anything
past that is interpretation, not recovery.

## What the present path does today

`plat_present()` in `src/main.c` letterboxes to 4:3, then calls
`SetStretchBltMode(COLORONCOLOR)` + `StretchDIBits`. COLORONCOLOR is
nearest-neighbour - it drops or duplicates whole rows and columns.

At the default 960x600 window that lands on **2.5x in both axes**: `ar` is
1.333, the window ratio is 1.6, so the picture is drawn at 800x600 from a
320x240 source. Half the source columns get 2 destination pixels and half get
3, and the same vertically. Static screens hide it; a scrolling playfield makes
the uneven columns crawl. That artifact belongs to the scaler, not to the game.

The menu (640x480 planar, doubled from 640x240) lands on 1.25x - same problem,
finer.

There is already a stubbed `if(integer_scale){ }` in `plat_present()`.

## The options, cheapest first

| Option | What it does | Verdict |
|---|---|---|
| Integer scaling | Snap the destination rect down to the largest whole multiple that fits, letterbox the rest | Do this one. Uniform pixels, scroll stops crawling, costs nothing |
| Pixel-art upscalers | xBRZ, HQ4x, ScaleFX, Omniscale - edge-directed interpolation | Expect it to look bad here; test on a screenshot before writing code |
| CRT shaders | Scanlines, aperture mask, mild curvature | The most honest "modern screen" answer, and needs the pixels to draw a mask |
| ML upscaling | ESRGAN and relatives | Practically a non-starter |

**Integer scaling.** 320x240 at 8x is 2560x1920, which fits inside 4K; 1440p
takes 5x. Claims nothing about fidelity and stays entirely in the render path.

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

GDI caps us at integer scaling. `StretchBlt` offers exactly one filter above
nearest (`HALFTONE`, a box filter - blurry, no better than bilinear) and no
shader hook. Everything past that needs a GPU backend: D3D11 or OpenGL, a
320x240 texture uploaded per frame, a fragment shader.

The architecture is already in the right shape for it.
`plat_present(const uint32_t *pix, int w, int h)` is a single function with a
clean signature, and the overlays are drawn in *window* pixels after the
stretch (`osd_draw_screen`, `rec_draw_dc`), so the OSD and REC badges stay
crisp at any scale. The emulator core would not be touched at all.

The real integration risk is not the shader, it is `present_phaselock`: the
present is timed against the emulated raster phase to avoid tearing. A
swapchain with its own vsync would fight that, and a poorly chosen present mode
adds a frame of latency to a game where flipper timing is the whole point.

## Fidelity

Anything past 2x integer is cosmetic smoothing in the render path only. None of
it touches the emulated framebuffer, so screenshots (which read the framebuffer
before badges) and the replay comparison path keep seeing the unmodified
320x240 image.
