/* gfx.h - software 2D framebuffer on top of GX.
 *
 * The game draws everything into a 640x480 RGBA8 buffer with anti-aliased
 * primitives, then the buffer is swizzled into GX tile order and blitted as a
 * single textured quad.  That keeps the drawing code portable and lets the
 * "hand drawn" look use real curves instead of sprite sheets.
 */
#ifndef GFX_H_
#define GFX_H_

#include <gccore.h>
#include <stdbool.h>

#define GFX_WIDTH 640
#define GFX_HEIGHT 480

typedef struct { u8 r, g, b, a; } GfxColor;

static inline GfxColor gfx_rgba(u8 r, u8 g, u8 b, u8 a)
{
    GfxColor c; c.r = r; c.g = g; c.b = b; c.a = a; return c;
}
static inline GfxColor gfx_rgb(u8 r, u8 g, u8 b) { return gfx_rgba(r, g, b, 255); }

/* Pixel buffer, exposed so game code can do cheap full-screen effects. */
extern GfxColor gfx_fb[GFX_WIDTH * GFX_HEIGHT];

void  gfx_init(void);
void  gfx_shutdown(void);
void  gfx_clear(GfxColor c);

/* Primitive drawing, all coordinates in pixels, y grows downwards. */
void  gfx_px(int x, int y, GfxColor c);
void  gfx_fill_rect(float x, float y, float w, float h, GfxColor c);
void  gfx_line(float x0, float y0, float x1, float y1, float thickness, GfxColor c);
void  gfx_polyline(const float *pts, int count, float thickness, GfxColor c);
void  gfx_circle(float cx, float cy, float r, GfxColor c);
void  gfx_ellipse(float cx, float cy, float rx, float ry, GfxColor c);
void  gfx_ellipse_outline(float cx, float cy, float rx, float ry, float thickness, GfxColor c);
void  gfx_ring(float cx, float cy, float r, float thickness, GfxColor c);
void  gfx_polygon(const float *pts, int count, GfxColor c);
void  gfx_round_rect(float x, float y, float w, float h, float radius, GfxColor c);
void  gfx_round_rect_outline(float x, float y, float w, float h, float radius,
                             float thickness, GfxColor c);

/* Text.  size is the cap height in pixels; the handwritten atlas is scaled to
 * match.  Returns the advance width. */
float gfx_text_len(const char *s, float size);
float gfx_text(const char *s, float x, float y, float size, GfxColor c);
float gfx_text_center(const char *s, float cx, float y, float size, GfxColor c);

/* Presentation */
void  gfx_begin(void);          /* clear the software buffer to black */
void  gfx_present(void);        /* upload + draw + flip, waits for vblank */
u32   gfx_frame_count(void);

/* Darken everything outside (x0,y0)-(x1,y1) at present time.  Any nonzero
 * level enables it (the multiplier is fixed at 161/256).  Replaces a
 * full-screen translucent fill, which cost a 1.2 MB read-modify-write pass. */
void  gfx_set_dim(int level, int x0, int y0, int x1, int y1);

/* CPU microseconds spent in the last gfx_present() before it blocked on
 * vblank, i.e. the real cost of swizzling, uploading and submitting. */
u32   gfx_present_cpu_us(void);

#ifdef TELEMETRY
/* Pixel accounting for tuning builds: fills, AA rims, lines, text, polygons. */
const u32 *gfx_counts(void);
/* CPU ticks per primitive family: fill_rect, circles/ellipses, lines, text,
 * polygons. */
const u32 *gfx_ticks(void);
#endif

#endif
