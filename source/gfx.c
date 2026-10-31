/* gfx.c - software renderer + GX presentation. */
#include "gfx.h"
#include "font_data.h"

#include <ogc/conf.h>
#include <ogc/gu.h>
#include <ogc/lwp_watchdog.h>
#include <math.h>
#include <malloc.h>
#include <string.h>
#include <stdio.h>

GfxColor gfx_fb[GFX_WIDTH * GFX_HEIGHT];

static GXRModeObj *rmode;
static void *xfb[2];
static int xfb_index;
static float view_w;
static u32 frame_no;

/* GX receives a 4x4-tiled RGBA8 copy of the linear framebuffer. */
static u8 texbuf[GFX_WIDTH * GFX_HEIGHT * 4] ATTRIBUTE_ALIGN(32);
static GXTexObj texobj;
static Mtx44 proj_mtx;
static u32 present_us;

/* Optional full-screen darkening applied during the swizzle.  The game sets
 * this instead of blending a translucent rectangle over 307k pixels; the
 * pixels are already in registers here, so it costs a few ALU ops instead of
 * a full read-modify-write pass over the framebuffer.  Everything except the
 * given rectangle is darkened, so a dialog drawn inside it stays bright. */
static u32 dim_on;
static int dim_x0, dim_y0, dim_x1, dim_y1;

void gfx_set_dim(int level, int x0, int y0, int x1, int y1)
{
    dim_on = level > 0;
    if (!dim_on) return;
    dim_x0 = x0; dim_y0 = y0; dim_x1 = x1; dim_y1 = y1;
}

/* Multiply a whole RGBA word by 161/256 (a 95/255 darkening) using only
 * shifts and subtractions: each byte becomes a - a/4 - a/8, and because the
 * result can never go negative there are no borrows between channels. */
static inline u32 dim_word(u32 p)
{
    u32 rgb = p & 0xffffff00u;
    return (rgb - ((rgb >> 2) & 0x3f3f3f00u) - ((rgb >> 3) & 0x1f1f1f00u)) | 0xffu;
}

static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline float clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* Pixel accounting for the tuning builds: how many pixels each primitive
 * family actually touched during the last frame.  Index order is the order
 * main.c packs them into the telemetry strip. */
#ifdef TELEMETRY
u32 gfx_npix[5];
u32 gfx_ntick[5];
static u32 fam_t0;
#define NPIX(i, n)      (gfx_npix[i] += (u32)(n))
#define PROF_START()    (fam_t0 = gettick())
#define PROF_STOP(i)    (gfx_ntick[i] += gettick() - fam_t0)
#else
#define NPIX(i, n)      ((void)0)
#define PROF_START()    ((void)0)
#define PROF_STOP(i)    ((void)0)
#endif

/* ------------------------------------------------------------------ setup */

static void *alloc_xfb(GXRModeObj *rm)
{
    void *p = SYS_AllocateFramebuffer(rm);
    return MEM_K0_TO_K1(p);
}

void gfx_init(void)
{
    VIDEO_Init();
    rmode = VIDEO_GetPreferredMode(NULL);

    xfb[0] = alloc_xfb(rmode);
    xfb[1] = alloc_xfb(rmode);
    xfb_index = 0;

    VIDEO_Configure(rmode);
    VIDEO_SetNextFramebuffer(xfb[0]);
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    if (rmode->viTVMode & VI_NON_INTERLACE)
        VIDEO_WaitVSync();

    /* The game is 4:3 (like the phone game seen through a TV).  In 16:9 the
     * quad is letterboxed at native proportions rather than stretched.  The VI
     * mode already carries this, so no IOS CONF round trip is needed (that can
     * block on a fresh NAND). */
    view_w = 640.0f;
    if (rmode->viWidth > 680)
        view_w = 640.0f * 16.0f / 12.0f;   /* 853.3 */

    void *fifo = memalign(32, 256 * 1024);
    GX_Init(fifo, 256 * 1024);
    GX_SetCopyClear((GXColor){253, 253, 245, 255}, 0x00ffffff);
    GX_SetViewport(0, 0, rmode->fbWidth, rmode->efbHeight, 0, 1);
    GX_SetDispCopyYScale((f32)rmode->xfbHeight / (f32)rmode->efbHeight);
    GX_SetScissor(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopySrc(0, 0, rmode->fbWidth, rmode->efbHeight);
    GX_SetDispCopyDst(rmode->fbWidth, rmode->xfbHeight);
    GX_SetCopyFilter(rmode->aa, rmode->sample_pattern, GX_TRUE, rmode->vfilter);
    GX_SetFieldMode(rmode->field_rendering,
                    ((rmode->viHeight == 2 * rmode->xfbHeight) ? GX_ENABLE : GX_DISABLE));
    GX_SetPixelFmt(GX_PF_RGB8_Z24, GX_ZC_LINEAR);
    GX_SetDispCopyGamma(GX_GM_1_0);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetZCompLoc(GX_FALSE);
    GX_SetColorUpdate(GX_TRUE);
    GX_SetAlphaUpdate(GX_TRUE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

    /* libogc's guOrtho is (top, bottom, left, right, near, far): a y-down
     * projection so game coordinates match the framebuffer. */
    guOrtho(proj_mtx, 0, 480.0f, 0, view_w, 0, 1000.0f);

    GX_InitTexObj(&texobj, texbuf, GFX_WIDTH, GFX_HEIGHT, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjFilterMode(&texobj, GX_NEAR, GX_NEAR);

    memset(gfx_fb, 0, sizeof(gfx_fb));
    frame_no = 0;
}

void gfx_shutdown(void)
{
    GX_DrawDone();
    VIDEO_SetBlack(TRUE);
    VIDEO_Flush();
}

u32 gfx_frame_count(void) { return frame_no; }

/* ------------------------------------------------------------ framebuffer */

void gfx_clear(GfxColor c)
{
#ifdef TEST_NOCLEAR
    return;
#endif
    if (c.a == 255) {
        u32 v = ((u32)c.r << 24) | ((u32)c.g << 16) | ((u32)c.b << 8) | 255u;
        u32 *p = (u32 *)gfx_fb;
        int n = GFX_WIDTH * GFX_HEIGHT;
        while (n >= 8) { p[0] = v; p[1] = v; p[2] = v; p[3] = v;
                         p[4] = v; p[5] = v; p[6] = v; p[7] = v; p += 8; n -= 8; }
        while (n--) *p++ = v;
        return;
    }
    for (int i = 0; i < GFX_WIDTH * GFX_HEIGHT; i++) gfx_fb[i] = c;
}

/* Source-over blend in 8.8 fixed point: (dst*(256-a) + src*a) >> 8.  Both
 * products are bounded by 255*256, so the shifted sum stays inside a byte and
 * no division (or magic multiply) is needed.  The framebuffer is always
 * opaque after the first primitive touches a pixel. */
static inline u32 lerp8(u32 d, u32 s, u32 a, u32 ia)
{
    return (d * ia + s * a) >> 8;
}

static inline void blend_px(GfxColor *p, GfxColor c, u32 a)
{
    if (a == 0) return;
    if (a >= 255) { p->r = c.r; p->g = c.g; p->b = c.b; p->a = 255; return; }
    u32 ia = 256u - a;
    u32 r = lerp8(p->r, c.r, a, ia);
    u32 g = lerp8(p->g, c.g, a, ia);
    u32 b = lerp8(p->b, c.b, a, ia);
    p->r = (u8)r; p->g = (u8)g; p->b = (u8)b; p->a = 255;
}

static inline void blend_cov(int x, int y, GfxColor c, float cov)
{
    if ((unsigned)x >= GFX_WIDTH || (unsigned)y >= GFX_HEIGHT) return;
    blend_px(&gfx_fb[y * GFX_WIDTH + x], c, (u32)((float)c.a * cov + 0.5f));
}

void gfx_px(int x, int y, GfxColor c) { blend_cov(x, y, c, 1.0f); }

/* Blend one horizontal run [x0, x1) with a constant alpha, reading and
 * writing whole 32 bit pixels so the three channels share the load/store. */
static inline void span_const(int y, int x0, int x1, GfxColor c, u32 a)
{
    if (x0 < 0) x0 = 0;
    if (x1 > GFX_WIDTH) x1 = GFX_WIDTH;
    if (x0 >= x1) return;
    u32 *row = (u32 *)gfx_fb + y * GFX_WIDTH + x0;
    int n = x1 - x0;
    NPIX(0, n);

    if (a >= 255) {
        u32 v = ((u32)c.r << 24) | ((u32)c.g << 16) | ((u32)c.b << 8) | 255u;
        while (n--) *row++ = v;
        return;
    }
    if (a == 0) return;

    const u32 ia = 256u - a;
    const u32 cr = (u32)c.r * a, cg = (u32)c.g * a, cb = (u32)c.b * a;
    while (n--) {
        u32 d = *row;
        u32 r = ((d >> 24) * ia + cr) >> 8;
        u32 g = (((d >> 16) & 0xff) * ia + cg) >> 8;
        u32 b = (((d >> 8) & 0xff) * ia + cb) >> 8;
        *row++ = (r << 24) | (g << 16) | (b << 8) | 0xffu;
    }
}

void gfx_fill_rect(float x, float y, float w, float h, GfxColor c)
{
    if (w < 0) { x += w; w = -w; }
    if (h < 0) { y += h; h = -h; }
    if (c.a == 0) return;
    int x0 = clampi((int)floorf(x), 0, GFX_WIDTH);
    int x1 = clampi((int)ceilf(x + w), 0, GFX_WIDTH);
    int y0 = clampi((int)floorf(y), 0, GFX_HEIGHT);
    int y1 = clampi((int)ceilf(y + h), 0, GFX_HEIGHT);
    PROF_START();
    for (int py = y0; py < y1; py++)
        span_const(py, x0, x1, c, c.a);
    PROF_STOP(0);
}

void gfx_line(float x0, float y0, float x1, float y1, float thickness, GfxColor c)
{
    float half = thickness * 0.5f;
    if (half < 0.5f) half = 0.5f;
    if (c.a == 0) return;

    float dx = x1 - x0, dy = y1 - y0;
    float len2 = dx * dx + dy * dy;
    if (len2 < 0.0001f) {
        PROF_START();
        gfx_circle(x0, y0, half, c);
        PROF_STOP(2);
        return;
    }
    PROF_START();
    /* Axis-aligned strokes are the common case (platform edges, highlights,
     * HUD rules): a thin bar with one coverage value per row is much cheaper
     * than walking the generic distance field. */
    if (dy == 0.0f) {
        float xa = fminf(x0, x1) - half, xb = fmaxf(x0, x1) + half;
        int pa = clampi((int)floorf(xa), 0, GFX_WIDTH - 1);
        int pb = clampi((int)ceilf(xb), 0, GFX_WIDTH - 1);
        int ya = clampi((int)floorf(y0 - half - 0.5f), 0, GFX_HEIGHT - 1);
        int yb = clampi((int)ceilf(y0 + half + 0.5f), 0, GFX_HEIGHT - 1);
        for (int py = ya; py <= yb; py++) {
            float cov = half + 0.5f - fabsf((float)py + 0.5f - y0);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            span_const(py, pa, pb + 1, c, (u32)((float)c.a * cov + 0.5f));
        }
        PROF_STOP(2);
        return;
    }
    if (dx == 0.0f) {
        float ya = fminf(y0, y1) - half, yb = fmaxf(y0, y1) + half;
        int pa = clampi((int)floorf(ya), 0, GFX_HEIGHT - 1);
        int pb = clampi((int)ceilf(yb), 0, GFX_HEIGHT - 1);
        int xa = clampi((int)floorf(x0 - half - 0.5f), 0, GFX_WIDTH - 1);
        int xb = clampi((int)ceilf(x0 + half + 0.5f), 0, GFX_WIDTH - 1);
        for (int px = xa; px <= xb; px++) {
            float cov = half + 0.5f - fabsf((float)px + 0.5f - x0);
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            u32 a = (u32)((float)c.a * cov + 0.5f);
            int y0i = clampi(pa, 0, GFX_HEIGHT);
            int y1i = clampi(pb + 1, 0, GFX_HEIGHT);
            for (int py = y0i; py < y1i; py++)
                blend_px(&gfx_fb[py * GFX_WIDTH + px], c, a);
        }
        PROF_STOP(2);
        return;
    }
    float inv_len = 1.0f / sqrtf(len2);
    float reach = half + 0.5f;      /* coverage falls to zero at this distance */
    float reach2 = reach * reach;
    float span = half + 0.75f;      /* how far the stroke reaches sideways */
    u32 ca = c.a;

    /* Walk along the major axis and only touch the thin band around the
     * segment: the squared distance is checked first, so sqrt runs only on
     * the two or three pixels that actually need anti-aliasing. */
    if (fabsf(dx) >= fabsf(dy)) {
        float inv_dx = 1.0f / dx;
        int xs = clampi((int)floorf(fminf(x0, x1) - span), 0, GFX_WIDTH - 1);
        int xe = clampi((int)ceilf(fmaxf(x0, x1) + span), 0, GFX_WIDTH - 1);
        for (int px = xs; px <= xe; px++) {
            float fx = px + 0.5f;
            float t = (fx - x0) * inv_dx;
            float yc = y0 + t * dy;
            int ys = clampi((int)floorf(yc - span), 0, GFX_HEIGHT - 1);
            int ye = clampi((int)ceilf(yc + span), 0, GFX_HEIGHT - 1);
            for (int py = ys; py <= ye; py++) {
                float fy = py + 0.5f;
                float ax = fx - x0, ay = fy - y0;
                float d2;
                if (t < 0.0f) {
                    d2 = ax * ax + ay * ay;
                } else if (t > 1.0f) {
                    float bx = fx - x1, by = fy - y1;
                    d2 = bx * bx + by * by;
                } else {
                    float perp = (ax * dy - ay * dx) * inv_len;
                    d2 = perp * perp;
                }
                if (d2 >= reach2) continue;
                float cov = reach - sqrtf(d2);
                NPIX(2, 1);
                blend_px(&gfx_fb[py * GFX_WIDTH + px], c,
                         (u32)((float)ca * cov + 0.5f));
            }
        }
    } else {
        float inv_dy = 1.0f / dy;
        int ys = clampi((int)floorf(fminf(y0, y1) - span), 0, GFX_HEIGHT - 1);
        int ye = clampi((int)ceilf(fmaxf(y0, y1) + span), 0, GFX_HEIGHT - 1);
        for (int py = ys; py <= ye; py++) {
            float fy = py + 0.5f;
            float t = (fy - y0) * inv_dy;
            float xc = x0 + t * dx;
            int xs = clampi((int)floorf(xc - span), 0, GFX_WIDTH - 1);
            int xe = clampi((int)ceilf(xc + span), 0, GFX_WIDTH - 1);
            for (int px = xs; px <= xe; px++) {
                float fx = px + 0.5f;
                float ax = fx - x0, ay = fy - y0;
                float d2;
                if (t < 0.0f) {
                    d2 = ax * ax + ay * ay;
                } else if (t > 1.0f) {
                    float bx = fx - x1, by = fy - y1;
                    d2 = bx * bx + by * by;
                } else {
                    float perp = (ax * dy - ay * dx) * inv_len;
                    d2 = perp * perp;
                }
                if (d2 >= reach2) continue;
                float cov = reach - sqrtf(d2);
                NPIX(2, 1);
                blend_px(&gfx_fb[py * GFX_WIDTH + px], c,
                         (u32)((float)ca * cov + 0.5f));
            }
        }
    }
    PROF_STOP(2);
}

void gfx_polyline(const float *pts, int count, float thickness, GfxColor c)
{
    for (int i = 0; i + 1 < count; i++)
        gfx_line(pts[i * 2], pts[i * 2 + 1], pts[i * 2 + 2], pts[i * 2 + 3], thickness, c);
}

/* ------------------------------------------------------------- round shapes
 *
 * Circles and ellipses share one scanline rasteriser.  Everything is done in
 * normalised space (nx,ny): the outline becomes the unit circle and `edge`
 * (the smaller radius) converts a normalised distance error back into pixels
 * for the anti-aliasing width.  Each row fills the fully covered span with a
 * constant-alpha run and only walks sqrt() over the two rim segments, so a
 * large soft blob costs one sqrt per row instead of one per pixel.
 */

static void ellipse_fill(float cx, float cy, float rx, float ry, GfxColor c)
{
    if (!(rx > 0.0f) || !(ry > 0.0f) || c.a == 0) return;

    float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
    float edge = fminf(rx, ry);
    float band = 0.5f / edge;                 /* half the AA width */
    float in2 = (1.0f > band) ? (1.0f - band) * (1.0f - band) : 0.0f;
    float out2 = (1.0f + band) * (1.0f + band);
    float yext = ry * (1.0f + band);
    u32 ca = c.a;

    int y0 = clampi((int)floorf(cy - yext), 0, GFX_HEIGHT - 1);
    int y1 = clampi((int)ceilf(cy + yext), 0, GFX_HEIGHT - 1);
    for (int py = y0; py <= y1; py++) {
        float ny = ((float)py + 0.5f - cy) * inv_ry;
        float ny2 = ny * ny;
        if (ny2 >= out2) continue;

        int ilo = 1, ihi = 0;                 /* empty by default */
        if (ny2 < in2) {
            float hw = rx * sqrtf(in2 - ny2);
            ilo = (int)ceilf(cx - hw - 0.5f);
            ihi = (int)floorf(cx + hw - 0.5f);
            span_const(py, ilo, ihi + 1, c, ca);
        }

        float xo = rx * sqrtf(out2 - ny2);
        int xa = (int)ceilf(cx - xo - 0.5f);
        int xb = (int)floorf(cx + xo - 0.5f);
        if (xa < 0) xa = 0;
        if (xb > GFX_WIDTH - 1) xb = GFX_WIDTH - 1;
        GfxColor *row = &gfx_fb[py * GFX_WIDTH];
        for (int px = xa; px <= xb; px++) {
            if (px >= ilo && px <= ihi) continue;
            float nx = ((float)px + 0.5f - cx) * inv_rx;
            float d2 = nx * nx + ny2;
            if (d2 >= out2) continue;
            float cov = (1.0f - sqrtf(d2)) * edge + 0.5f;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            NPIX(1, 1);
            blend_px(&row[px], c, (u32)(cov * (float)ca + 0.5f));
        }
    }
}

static void ellipse_outline(float cx, float cy, float rx, float ry, float thickness,
                            GfxColor c)
{
    if (!(rx > 0.0f) || !(ry > 0.0f) || c.a == 0) return;

    float h = thickness * 0.5f;
    if (h < 0.5f) h = 0.5f;
    float inv_rx = 1.0f / rx, inv_ry = 1.0f / ry;
    float edge = fminf(rx, ry);
    float band = (h + 0.5f) / edge;
    float in2 = (1.0f > band) ? (1.0f - band) * (1.0f - band) : -1.0f;
    float out2 = (1.0f + band) * (1.0f + band);
    float yext = ry * (1.0f + band);
    u32 ca = c.a;

    int y0 = clampi((int)floorf(cy - yext), 0, GFX_HEIGHT - 1);
    int y1 = clampi((int)ceilf(cy + yext), 0, GFX_HEIGHT - 1);
    for (int py = y0; py <= y1; py++) {
        float ny = ((float)py + 0.5f - cy) * inv_ry;
        float ny2 = ny * ny;
        if (ny2 >= out2) continue;

        float xo = rx * sqrtf(out2 - ny2);
        int xa = (int)ceilf(cx - xo - 0.5f);
        int xb = (int)floorf(cx + xo - 0.5f);
        if (xa < 0) xa = 0;
        if (xb > GFX_WIDTH - 1) xb = GFX_WIDTH - 1;
        GfxColor *row = &gfx_fb[py * GFX_WIDTH];
        for (int px = xa; px <= xb; px++) {
            float nx = ((float)px + 0.5f - cx) * inv_rx;
            float d2 = nx * nx + ny2;
            if (d2 >= out2 || d2 <= in2) continue;
            float cov = h + 0.5f - fabsf(sqrtf(d2) - 1.0f) * edge;
            if (cov <= 0.0f) continue;
            NPIX(1, 1);
            blend_px(&row[px], c, (u32)(cov * (float)ca + 0.5f));
        }
    }
}

void gfx_circle(float cx, float cy, float r, GfxColor c)
{
    PROF_START();
    ellipse_fill(cx, cy, r, r, c);
    PROF_STOP(1);
}

void gfx_ring(float cx, float cy, float r, float thickness, GfxColor c)
{
    if (r <= 0.0f || c.a == 0) return;
    PROF_START();
    float h = thickness * 0.5f;
    if (h < 0.5f) h = 0.5f;
    float out_r = r + h + 0.5f, in_r = r - h - 0.5f;
    float out2 = out_r * out_r, in2 = in_r > 0.0f ? in_r * in_r : -1.0f;
    u32 ca = c.a;

    int x0 = clampi((int)floorf(cx - out_r), 0, GFX_WIDTH - 1);
    int x1 = clampi((int)ceilf(cx + out_r), 0, GFX_WIDTH - 1);
    int y0 = clampi((int)floorf(cy - out_r), 0, GFX_HEIGHT - 1);
    int y1 = clampi((int)ceilf(cy + out_r), 0, GFX_HEIGHT - 1);
    for (int py = y0; py <= y1; py++) {
        float dy = (float)py + 0.5f - cy;
        float dy2 = dy * dy;
        if (dy2 >= out2) continue;
        GfxColor *row = &gfx_fb[py * GFX_WIDTH];
        for (int px = x0; px <= x1; px++) {
            float dx = (float)px + 0.5f - cx;
            float d2 = dx * dx + dy2;
            if (d2 >= out2 || d2 <= in2) continue;
            float cov = h + 0.5f - fabsf(sqrtf(d2) - r);
            if (cov <= 0.0f) continue;
            NPIX(1, 1);
            blend_px(&row[px], c, (u32)(cov * (float)ca + 0.5f));
        }
    }
    PROF_STOP(1);
}

void gfx_ellipse(float cx, float cy, float rx, float ry, GfxColor c)
{
    PROF_START();
    ellipse_fill(cx, cy, rx, ry, c);
    PROF_STOP(1);
}

void gfx_ellipse_outline(float cx, float cy, float rx, float ry, float thickness,
                         GfxColor c)
{
    PROF_START();
    ellipse_outline(cx, cy, rx, ry, thickness, c);
    PROF_STOP(1);
}

/* Scanline polygon fill with 4 horizontal sub-samples for soft edges. */
void gfx_polygon(const float *pts, int count, GfxColor c)
{
    if (count < 3 || c.a == 0) return;
    PROF_START();
    float ymin = pts[1], ymax = pts[1];
    float xmin = pts[0], xmax = pts[0];
    for (int i = 1; i < count; i++) {
        if (pts[i * 2 + 1] < ymin) ymin = pts[i * 2 + 1];
        if (pts[i * 2 + 1] > ymax) ymax = pts[i * 2 + 1];
        if (pts[i * 2] < xmin) xmin = pts[i * 2];
        if (pts[i * 2] > xmax) xmax = pts[i * 2];
    }
    int y0 = clampi((int)floorf(ymin), 0, GFX_HEIGHT - 1);
    int y1 = clampi((int)ceilf(ymax), 0, GFX_HEIGHT - 1);
    int gx0 = clampi((int)floorf(xmin), 0, GFX_WIDTH - 1);
    int gx1 = clampi((int)ceilf(xmax), 0, GFX_WIDTH - 1);
    if (gx0 > gx1) { PROF_STOP(4); return; }

    static float cov_row[GFX_WIDTH];
    const int SUB = 4;
    const float INV_SUB = 0.25f;

    for (int py = y0; py <= y1; py++) {
        /* Only the polygon's own column span needs clearing and blending. */
        memset(cov_row + gx0, 0, sizeof(float) * (gx1 - gx0 + 1));
        int touched = 0;
        for (int s = 0; s < SUB; s++) {
            float sy = py + (s + 0.5f) * INV_SUB;
            float xs[16];
            int n = 0;
            for (int i = 0; i < count && n < 16; i++) {
                int j = (i + 1) % count;
                float ay = pts[i * 2 + 1], by = pts[j * 2 + 1];
                if ((ay <= sy && by > sy) || (by <= sy && ay > sy)) {
                    float ax = pts[i * 2], bx = pts[j * 2];
                    xs[n++] = ax + (sy - ay) * (bx - ax) / (by - ay);
                }
            }
            for (int i = 1; i < n; i++) {
                float v = xs[i];
                int j = i - 1;
                while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; }
                xs[j + 1] = v;
            }
            for (int i = 0; i + 1 < n; i += 2) {
                float xa = xs[i], xb = xs[i + 1];
                if (xb < 0 || xa > GFX_WIDTH) continue;
                int ia = clampi((int)floorf(xa), gx0, gx1);
                int ib = clampi((int)ceilf(xb), gx0, gx1);
                for (int px = ia; px <= ib; px++) {
                    float l = fmaxf(xa, (float)px);
                    float r = fminf(xb, (float)px + 1.0f);
                    if (r > l) cov_row[px] += (r - l) * INV_SUB;
                }
                touched = 1;
            }
        }
        if (!touched) continue;
        GfxColor *row = &gfx_fb[py * GFX_WIDTH];
        for (int px = gx0; px <= gx1; px++) {
            float v = cov_row[px];
            if (v > 0.001f) {
                NPIX(4, 1);
                blend_px(&row[px], c,
                         (u32)((float)c.a * (v > 1.0f ? 1.0f : v) + 0.5f));
            }
        }
    }
    PROF_STOP(4);
}

/* Filled rounded box: a single span per row, with the ends following the
 * corner arcs.  This replaces three rectangles plus four anti-aliased circles,
 * which used to dominate the per-platform cost. */
void gfx_round_rect(float x, float y, float w, float h, float r, GfxColor c)
{
    if (!(w > 0.0f) || !(h > 0.0f) || c.a == 0) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0.5f) { gfx_fill_rect(x, y, w, h, c); return; }

    int y0 = clampi((int)floorf(y), 0, GFX_HEIGHT - 1);
    int y1 = clampi((int)ceilf(y + h), 0, GFX_HEIGHT - 1);
    float cyt = y + r, cyb = y + h - r;
    float r2 = r * r;

    PROF_START();
    for (int py = y0; py <= y1; py++) {
        float fy = (float)py + 0.5f;
        float dy = (fy < cyt) ? cyt - fy : (fy > cyb ? fy - cyb : 0.0f);
        if (dy >= r) continue;
        float bulge = (dy > 0.0f) ? sqrtf(r2 - dy * dy) : r;
        float xl = x + r - bulge;
        float xr = x + w - r + bulge;
        if (xr - xl < 1.0f) {
            blend_cov((int)floorf(xl), py, c, xr - xl);
            continue;
        }
        int ia = (int)ceilf(xl);
        int ib = (int)floorf(xr);
        span_const(py, ia, ib, c, c.a);
        if (xl < (float)ia) blend_cov(ia - 1, py, c, (float)ia - xl);
        if (xr > (float)ib) blend_cov(ib, py, c, xr - (float)ib);
    }
    PROF_STOP(1);
}

/* One row band of the two corner arcs around cyc.  Coverage comes straight
 * from the radial distance, so the stroke has no visible polygon steps. */
static void corner_arcs(float xl, float xr, float cyc, float r, float half,
                        int y0, int y1, GfxColor c)
{
    float rout = r + half + 0.5f;
    float rin = r - half - 0.5f;
    if (rin < 0.0f) rin = 0.0f;
    float ro2 = rout * rout, ri2 = rin * rin;

    if (y0 < 0) y0 = 0;
    if (y1 > GFX_HEIGHT - 1) y1 = GFX_HEIGHT - 1;
    for (int py = y0; py <= y1; py++) {
        float dy = (float)py + 0.5f - cyc;
        if (dy < 0.0f) dy = -dy;
        if (dy >= rout) continue;
        float xo = sqrtf(ro2 - dy * dy);
        float xi = (dy < rin) ? sqrtf(ri2 - dy * dy) : 0.0f;
        for (int side = 0; side < 2; side++) {
            float base = side ? xr : xl;
            float lo = side ? base + xi : base - xo;
            float hi = side ? base + xo : base - xi;
            int pa = (int)floorf(lo);
            int pb = (int)ceilf(hi);
            if (pa < 0) pa = 0;
            if (pb > GFX_WIDTH - 1) pb = GFX_WIDTH - 1;
            for (int px = pa; px <= pb; px++) {
                float adx = fabsf((float)px + 0.5f - base);
                if (adx < xi || adx > xo) continue;
                float d = sqrtf(adx * adx + dy * dy);
                float cov = half + 0.5f - fabsf(d - r);
                if (cov <= 0.0f) continue;
                NPIX(2, 1);
                blend_cov(px, py, c, cov);
            }
        }
    }
}

void gfx_round_rect_outline(float x, float y, float w, float h, float r,
                            float t, GfxColor c)
{
    if (!(w > 0.0f) || !(h > 0.0f) || c.a == 0) return;
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    float half = t * 0.5f;
    if (half < 0.5f) half = 0.5f;

    gfx_line(x + r, y, x + w - r, y, t, c);
    gfx_line(x + r, y + h, x + w - r, y + h, t, c);
    gfx_line(x, y + r, x, y + h - r, t, c);
    gfx_line(x + w, y + r, x + w, y + h - r, t, c);

    PROF_START();
    corner_arcs(x + r, x + w - r, y + r, r, half,
                (int)floorf(y) - 1, (int)ceilf(y + r) + 1, c);
    corner_arcs(x + r, x + w - r, y + h - r, r, half,
                (int)floorf(y + h - r) - 1, (int)ceilf(y + h) + 1, c);
    PROF_STOP(1);
}

/* ------------------------------------------------------------------- text */

static inline float glyph_alpha(const FontGlyph *g, float sx, float sy)
{
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx > g->w - 1) sx = g->w - 1;
    if (sy > g->h - 1) sy = g->h - 1;
    int x0 = (int)sx, y0 = (int)sy;
    int x1 = x0 + 1 < g->w ? x0 + 1 : x0;
    int y1 = y0 + 1 < g->h ? y0 + 1 : y0;
    float fx = sx - x0, fy = sy - y0;
    const unsigned char *p = font_pixels + g->off;
    float a00 = p[y0 * g->w + x0], a10 = p[y0 * g->w + x1];
    float a01 = p[y1 * g->w + x0], a11 = p[y1 * g->w + x1];
    float a = (a00 * (1 - fx) + a10 * fx) * (1 - fy) +
              (a01 * (1 - fx) + a11 * fx) * fy;
    return a / 255.0f;
}

float gfx_text_len(const char *s, float size)
{
    float scale = size / (float)FONT_UNIT;
    float w = 0;
    for (const char *p = s; *p; p++) {
        int code = (unsigned char)*p;
        if (code < FONT_FIRST || code > FONT_LAST) code = '?';
        w += font_glyph[code - FONT_FIRST].adv * scale;
    }
    return w;
}

float gfx_text(const char *s, float x, float y, float size, GfxColor c)
{
    PROF_START();
    float scale = size / (float)FONT_UNIT;
    float pen = x;
    float baseline = y + FONT_ASCENT * scale;
    for (const char *p = s; *p; p++) {
        if (*p == '\n') {
            pen = x;
            baseline += FONT_LINE * scale;
            y += FONT_LINE * scale;
            continue;
        }
        int code = (unsigned char)*p;
        if (code < FONT_FIRST || code > FONT_LAST) code = '?';
        const FontGlyph *g = &font_glyph[code - FONT_FIRST];
        if (g->w > 0) {
            float dx = pen + g->ox * scale;
            float dy = baseline + g->oy * scale;
            float dw = g->w * scale, dh = g->h * scale;
            int px0 = clampi((int)floorf(dx), 0, GFX_WIDTH);
            int px1 = clampi((int)ceilf(dx + dw), 0, GFX_WIDTH);
            int py0 = clampi((int)floorf(dy), 0, GFX_HEIGHT);
            int py1 = clampi((int)ceilf(dy + dh), 0, GFX_HEIGHT);
            float inv_scale = 1.0f / scale;
            for (int py = py0; py < py1; py++) {
                float sy = ((py + 0.5f) - dy) * inv_scale - 0.5f;
                GfxColor *row = &gfx_fb[py * GFX_WIDTH];
                for (int px = px0; px < px1; px++) {
                    float sx = ((px + 0.5f) - dx) * inv_scale - 0.5f;
                    float a = glyph_alpha(g, sx, sy);
                    if (a > 0.02f) {
                        NPIX(3, 1);
                        blend_px(&row[px], c,
                                 (u32)((float)c.a * (a > 1.0f ? 1.0f : a) + 0.5f));
                    }
                }
            }
        }
        pen += g->adv * scale;
    }
    PROF_STOP(3);
    return pen - x;
}

float gfx_text_center(const char *s, float cx, float y, float size, GfxColor c)
{
    return gfx_text(s, cx - gfx_text_len(s, size) * 0.5f, y, size, c);
}

/* ----------------------------------------------------------- presentation */

static void swizzle(void)
{
    /* 4x4 blocks, each 64 bytes: 16 x (alpha,red) then 16 x (green,blue).
     * Pixels are gathered a 32 bit word at a time and two of them are packed
     * into each destination word, so a block only costs four loads and four
     * stores per row instead of eight byte loads and eight byte stores. */
    const int bw = GFX_WIDTH / 4, bh = GFX_HEIGHT / 4;
    u32 *dst = (u32 *)texbuf;
    const u32 *src = (const u32 *)gfx_fb;
    for (int by = 0; by < bh; by++) {
        for (int bx = 0; bx < bw; bx++) {
            const u32 *col = src + (by * 4) * GFX_WIDTH + bx * 4;
            for (int r = 0; r < 4; r++) {
                const u32 *s = col + r * GFX_WIDTH;
                u32 p0 = s[0], p1 = s[1], p2 = s[2], p3 = s[3];
                if (dim_on) {
                    int x = bx * 4, y = by * 4 + r;
                    if (y < dim_y0 || y >= dim_y1 || x >= dim_x1 || x + 3 < dim_x0) {
                        /* whole 4 pixel group outside the dialog */
                        p0 = dim_word(p0); p1 = dim_word(p1);
                        p2 = dim_word(p2); p3 = dim_word(p3);
                    } else if (x < dim_x0 || x + 3 >= dim_x1) {
                        /* boundary group, test each pixel */
                        if (x     < dim_x0 || x     >= dim_x1) p0 = dim_word(p0);
                        if (x + 1 < dim_x0 || x + 1 >= dim_x1) p1 = dim_word(p1);
                        if (x + 2 < dim_x0 || x + 2 >= dim_x1) p2 = dim_word(p2);
                        if (x + 3 < dim_x0 || x + 3 >= dim_x1) p3 = dim_word(p3);
                    }
                }
                u32 a0 = ((p0 & 0xffu) << 8) | (p0 >> 24);
                u32 a1 = ((p1 & 0xffu) << 8) | (p1 >> 24);
                u32 a2 = ((p2 & 0xffu) << 8) | (p2 >> 24);
                u32 a3 = ((p3 & 0xffu) << 8) | (p3 >> 24);
                u32 g0 = (p0 >> 8) & 0xffffu;
                u32 g1 = (p1 >> 8) & 0xffffu;
                u32 g2 = (p2 >> 8) & 0xffffu;
                u32 g3 = (p3 >> 8) & 0xffffu;
                dst[r * 2 + 0] = (a0 << 16) | a1;
                dst[r * 2 + 1] = (a2 << 16) | a3;
                dst[8 + r * 2 + 0] = (g0 << 16) | g1;
                dst[8 + r * 2 + 1] = (g2 << 16) | g3;
            }
            dst += 16;
        }
    }
}

void gfx_begin(void)
{
    /* nothing yet: the game clears to the paper colour itself. */
}

void gfx_present(void)
{
    u32 t_start = gettick();
    swizzle();
    /* GX reads the texture through DMA; the CPU cache must be written back or
     * the GPU samples stale bytes (a fully black screen). */
    DCStoreRange(texbuf, sizeof(texbuf));

    GX_InvalidateTexAll();
    GX_LoadTexObj(&texobj, GX_TEXMAP0);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT0, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
    GX_SetNumChans(0);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevOp(GX_TEVSTAGE0, GX_REPLACE);
    GX_SetNumTevStages(1);
    GX_SetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
    GX_SetZMode(GX_FALSE, GX_LEQUAL, GX_FALSE);
    GX_SetCullMode(GX_CULL_NONE);
    GX_SetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);

    GX_LoadProjectionMtx(proj_mtx, GX_ORTHOGRAPHIC);
    Mtx mv;
    guMtxIdentity(mv);
    GX_LoadPosMtxImm(mv, GX_PNMTX0);

    float x0 = (view_w - 640.0f) * 0.5f;
    float x1 = x0 + 640.0f;

    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    GX_Position3f32(x0, 0.0f, 0.0f);   GX_TexCoord2f32(0.0f, 0.0f);
    GX_Position3f32(x1, 0.0f, 0.0f);   GX_TexCoord2f32(1.0f, 0.0f);
    GX_Position3f32(x1, 480.0f, 0.0f); GX_TexCoord2f32(1.0f, 1.0f);
    GX_Position3f32(x0, 480.0f, 0.0f); GX_TexCoord2f32(0.0f, 1.0f);
    GX_End();
    GX_DrawDone();

    GX_CopyDisp(xfb[xfb_index], GX_TRUE);

    present_us = (u32)ticks_to_microsecs(gettick() - t_start);
    VIDEO_SetNextFramebuffer(xfb[xfb_index]);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    xfb_index ^= 1;
    frame_no++;
#ifdef TELEMETRY
    memset(gfx_npix, 0, sizeof(gfx_npix));
    memset(gfx_ntick, 0, sizeof(gfx_ntick));
#endif
}

#ifdef TELEMETRY
/* Pixels touched per primitive family since the last present. */
const u32 *gfx_counts(void) { return gfx_npix; }

/* CPU ticks spent per primitive family since the last present. */
const u32 *gfx_ticks(void) { return gfx_ntick; }
#endif

u32 gfx_present_cpu_us(void) { return present_us; }
