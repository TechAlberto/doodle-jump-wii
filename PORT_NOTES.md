# Port notes

## Why this is a rewrite, not a port

The APK (`doodle-jump-3-11-40.apk`) is a Cocos2d-x application. Cocos2d-x is a
C++ engine with an OpenGL ES 2.0 renderer and a large runtime; there is no
usable OpenGL ES path on the Wii (GX is fixed-function-ish, no shaders) and
libogc has no port of the engine. Shipping even a stripped build would mean
replacing the renderer, the audio backend, the input layer and the file system
anyway, so this project reimplements the game as a native libogc application:

* **Gameplay** is reproduced from the observable mechanics (auto-bounce,
  horizontal wrap, platform types, powerups, monsters, height score).
* **Art** is not extracted from the APK. Every visual is a vector-style
  primitive drawn at runtime: rounded platforms with hatching, an ellipse
  doodler with legs and eyes, clouds, particles, monsters.
* **Audio** is not extracted either: `sfx.c` synthesises all effects and an
  original 8-bar chiptune loop into ASND voices at boot.

## Rendering

The whole game is drawn into a 640x480 RGBA8 buffer in main memory and then
uploaded to GX as a single texture:

1. Anti-aliased primitives write into `gfx_fb` (spans with fixed-point
   blending, analytic circles/ellipses/rounded boxes, scanline polygons and a
   bitmap-font sampler).
2. `swizzle()` converts the linear buffer into GX's 4x4-tiled `GX_TF_RGBA8`
   layout, two pixels per 32-bit store.
3. `DCStoreRange()` writes the texture back through the CPU cache (without it
   the GPU samples stale bytes and the screen is black).
4. One textured quad is drawn with an orthographic projection and the XFB is
   copied and flipped.

GX-specific traps that are easy to hit again:

* `guOrtho` takes (mtx, left, right, bottom, top, near, far) — swapping the
  pairs silently mirrors or squashes the image.
* `VIDEO_SetBlack(FALSE)` is required after configuring the VI, or the first
  frames stay black.
* The display copy needs `GX_CopyDisp` *and* `GX_DrawDone` before the flip.

### Aspect ratio

The game layout is 4:3. `rmode->viWidth > 680` (a 16:9 mode) makes the quad
640x480 inside an 853.3-wide orthographic projection, so the image is
pillarboxed rather than stretched. No IOS `CONF_GetAspectRatio` call is made:
that can block on a console with a fresh NAND.

## Performance

The software rasteriser is the frame budget. Measured in Dolphin at 640x480
(TELEMETRY build, so the ~5 ms telemetry strip is *not* in these numbers):

| Stage | PLAY | DEAD |
| --- | --- | --- |
| background (clear + grid + clouds) | 2.2 ms | 2.2 ms |
| platforms | 1.8 ms | 1.8 ms |
| doodler | 0.8 ms | — |
| HUD | 0.55 ms | 0.5 ms |
| overlay (panel + text) | — | 3.6 ms |
| present (swizzle + upload + flip) | 4.6 ms | 7.7 ms |
| **draw + present** | **~10 ms** | **~16 ms** |

Things that mattered, in order:

* **Transparent spans are ~15x more expensive than opaque ones.** The span
  blender reads, blends and writes every pixel; the opaque path is a plain
  32-bit store loop. Keep big fills opaque.
* **One span per row instead of one shape per corner.** The first version drew
  a rounded box as three rectangles plus four circles, and outlined it with
  four lines plus 24 short trig-generated arc segments. Replacing that with an
  analytic per-row rounded box and two analytic arc bands took the platform
  cost from 7 ms to 1.8 ms.
* **Axis-aligned lines are bars.** Platform edges, highlights and hatching are
  horizontal or vertical; a per-row coverage value avoids the generic
  distance-field loop (3.4 ms -> 0.6 ms for all lines in a frame).
* **Dim in the swizzle, not in the framebuffer.** The pause/game-over dim used
  to be a 307k-pixel translucent fill (12.8 ms). It is now applied while the
  swizzle already has each pixel in registers, multiply-free
  (`a - a/4 - a/8` per channel), and skipped for the dialog rectangle so the
  panel stays bright (12.8 ms -> 3 ms).
* **Fewer touched pixels beats fewer instructions.** Per-pixel cost in Dolphin
  is high (tens of cycles even for a simple blend), so clipping primitives to
  their real bounds is the biggest lever.

The 1.2 MB clear at the start of each frame costs ~1.5 ms. A GX paper-colour
quad plus an alpha-only clear was considered but rejected: the anti-aliased
primitives blend against the destination colour, so a transparent destination
would leave dark fringes.

## Gameplay and level generation

The world is generated upward as the player climbs. Two invariants keep every
run solvable even though break/vanish platforms are destroyed when used:

* **At most one fragile platform between solid ones.** A roll that would place
  two break/vanish platforms in a row is turned into a normal platform.
  Otherwise both could vanish and leave the next solid surface more than a full
  jump (188 px) above the last one — a permanent soft-lock. This actually
  happened at score ~1012 before the rule was added.
* **Horizontal steps stay jumpable across a skipped platform.** Each platform
  is placed within 170 px of the previous one; after a fragile platform the
  next solid is also held within 140 px of the last solid and 120 px of the
  fragile platform, so the jump still works when the fragile one is gone.

Break platforms give a reduced bounce (0.72x jump velocity, ~97 px) instead of
dropping the player, which avoids a second class of double-gap locks.

The AUTOPLAY controller aims at the highest surface it can reach before the
current bounce returns, projects moving platforms to the landing time, dodges
monsters while rising, stomps them while falling, and shoots with velocity
lead. It scans down to the bottom of the visible play area rather than only
above the camera lip, so it never loses track of the platform it is about to
land on.

## Verification

Images cannot be eyeballed reliably on a TV-scaled emulator window, so the
build can encode its state as a strip of black/white cells (`TELEMETRY=1`).
`main.c` packs 38 32-bit channels (game state, score, position, timings per
stage, pixels per primitive family, CPU ticks per primitive family, plus
autopilot diagnostics) and `tools/decode_telemetry.py` reads them back out of a
screenshot. Together with `measure_png.py`/`ink_png.py`/`zoom_png.py` this
makes rendering changes measurable rather than a matter of opinion.

## Known limitations

* The high score lives in RAM; it resets when the console is powered off.
  (Writing to NAND needs `NAND`/`ISFS` setup and a directory layout that is
  not worth the failure modes for one integer.)
* No motion-plus or pointer steering; nunchuk stick, tilt and d-pad only.
* The title/pause/game-over fonts are one bitmap atlas generated from a
  handwriting-style system font; there is no localisation.
* Sound is a fixed synthesiser, not a music engine with track data.
* `source/gfx.c.bak` is the pre-optimisation rasteriser, kept as a reference
  for the original (slower) rounded-box and line code.
