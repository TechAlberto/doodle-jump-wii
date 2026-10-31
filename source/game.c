/* game.c - a from-scratch Doodle Jump style platformer.
 *
 * Everything here is hand drawn with the software renderer in gfx.c: no
 * extracted art, sprites or samples from the Android package.  The mechanics
 * (auto bouncing, one way scrolling, crumbling platforms, monsters, spring /
 * propeller / jetpack pickups and nose shots) are the familiar ones.
 */
#include "game.h"
#include "gfx.h"
#include "sfx.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#ifdef TELEMETRY
#include <ogc/lwp_watchdog.h>
#endif

#define SCR_W 640.0f
#define SCR_H 480.0f

#define PLAYER_HW 16.0f
#define PLAYER_HH 20.0f

#define GRAVITY     2450.0f
#define JUMP_VY      960.0f
#define SPRING_VY   1700.0f
#define MAX_FALL     1500.0f
#define MOVE_ACCEL   3400.0f
#define MOVE_MAX      440.0f
#define MOVE_DRAG    2200.0f

#define CAM_ANCHOR   280.0f   /* keep the doodler this far above the bottom */
#define CAM_START    (-60.0f)
#define SCORE_PER_PX (1.0f / 7.0f)

#define MAX_PLATFORMS 56
#define MAX_MONSTERS   8
#define MAX_BULLETS   10
#define MAX_PARTICLES 96

/* ------------------------------------------------------------------ types */

typedef enum { PF_NORMAL, PF_MOVING, PF_BREAK, PF_VANISH } PlatType;
typedef enum { PU_NONE, PU_SPRING, PU_PROPELLER, PU_JETPACK } PowerType;

typedef struct {
    bool active;
    float x, y;            /* y = top surface, world space (y grows up) */
    float w;
    PlatType type;
    PowerType power;
    float dir, speed;
    float spring_t;
    u32 seed;
} Platform;

typedef struct {
    bool active;
    float x, y;
    float base_x, range;
    float vx;
    float phase;
    u32 seed;
} Monster;

typedef struct {
    bool active;
    float x, y, vy;
} Bullet;

typedef struct {
    bool active;
    float x, y, vx, vy;
    float life, max_life;
    float size;
    float grav;
    GfxColor col;
} Particle;

typedef struct {
    float x, y, vx, vy;
    float max_height;
    float power_t;
    PowerType power;
    float shoot_cd;
    int facing;
} Player;

enum { ST_TITLE, ST_PLAY, ST_PAUSE, ST_DEAD };

/* ---------------------------------------------------------------- globals */

static Platform platforms[MAX_PLATFORMS];
static Monster  monsters[MAX_MONSTERS];
static Bullet   bullets[MAX_BULLETS];
static Particle particles[MAX_PARTICLES];
static Player   player;

static int   state;
static float cam_y;
static float next_platform_y;
static float last_platform_x;   /* keeps successive platforms within jump range */
static float last_solid_x;      /* last non-crumbling platform, for skip-safe jumps */
static int   last_platform_type;
static int   score, best;
static bool  new_best;
static float dead_timer;
static float time_now;
static u32   game_seed;
#ifdef AUTOPLAY
static float auto_timer;
static float auto_want;   /* steering target, -1 when no platform matched */
static float auto_ty;     /* target surface height */
static int   auto_nup;    /* surfaces above the feet this bounce */
static int   auto_nok;    /* ... of those, reachable horizontally */
static float auto_apex;   /* feet height at the top of this bounce */
static float auto_topx;   /* highest platform above the feet */
static float auto_topy;
#endif

/* Per-stage draw timings for the telemetry build (see tools/decode_telemetry.py). */
#define PROF_SLOTS 6
#ifdef TELEMETRY
static float prof_us[PROF_SLOTS];
static u32   prof_t0;
static inline void prof_begin(void) { prof_t0 = gettick(); }
static inline void prof_end(int i)
{
    prof_us[i] = (float)ticks_to_microsecs(gettick() - prof_t0);
    prof_t0 = gettick();
}
#else
#define prof_begin()   ((void)0)
#define prof_end(i)    ((void)0)
#endif

/* ------------------------------------------------------------ tiny helpers */

#define GC(r, g, b)    ((GfxColor){(u8)(r), (u8)(g), (u8)(b), 255})
#define GCA(r, g, b, a) ((GfxColor){(u8)(r), (u8)(g), (u8)(b), (u8)(a)})

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static u32 hash32(u32 x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Stable -1..1 jitter so a platform keeps the same wobble every frame. */
static float jitter(u32 seed, int i)
{
    u32 h = hash32(seed + 0x9e3779b9u * (u32)(i + 1));
    return ((float)(h >> 8) / 16777216.0f) * 2.0f - 1.0f;
}

static u32 rng_next(void)
{
    game_seed = game_seed * 1664525u + 1013904223u;
    return game_seed;
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(rng_next() >> 8) / 16777216.0f);
}

static inline float scr_y(float wy)
{
    return SCR_H - (wy - cam_y);
}

/* ------------------------------------------------------------- particles */

static void spawn_particle(float x, float y, float vx, float vy, float life,
                           float size, float grav, GfxColor col)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (p->active) continue;
        p->active = true;
        p->x = x; p->y = y;
        p->vx = vx; p->vy = vy;
        p->life = p->max_life = life;
        p->size = size;
        p->grav = grav;
        p->col = col;
        return;
    }
}

static void burst(float x, float y, int n, GfxColor col, float speed)
{
    for (int i = 0; i < n; i++) {
        float a = frand(0.0f, 6.2831853f);
        float s = frand(speed * 0.35f, speed);
        spawn_particle(x, y, cosf(a) * s, sinf(a) * s,
                       frand(0.35f, 0.8f), frand(2.0f, 5.0f), 1500.0f, col);
    }
}

static void dust(float x, float y)
{
    for (int i = 0; i < 3; i++)
        spawn_particle(x + frand(-22.0f, 22.0f), y,
                       frand(-35.0f, 35.0f), frand(20.0f, 90.0f),
                       frand(0.25f, 0.45f), frand(2.0f, 4.0f), 300.0f,
                       GCA(205, 200, 185, 220));
}

static void update_particles(float dt)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        Particle *p = &particles[i];
        if (!p->active) continue;
        p->x += p->vx * dt;
        p->y += p->vy * dt;
        p->vy -= p->grav * dt;
        p->life -= dt;
        if (p->life <= 0.0f) p->active = false;
    }
}

static void draw_particles(void)
{
    for (int i = 0; i < MAX_PARTICLES; i++) {
        const Particle *p = &particles[i];
        if (!p->active) continue;
        float sy = scr_y(p->y);
        if (sy < -20.0f || sy > SCR_H + 20.0f) continue;
        float k = clampf(p->life / p->max_life, 0.0f, 1.0f);
        GfxColor c = p->col;
        c.a = (u8)((float)c.a * k);
        gfx_circle(p->x, sy, p->size * (0.55f + 0.45f * k), c);
    }
}

/* --------------------------------------------------------------- doodler */

static void draw_doodler(float cx, float cy, float vx, float vy,
                         PowerType power, float power_t, float t)
{
    GfxColor body  = GC(154, 214, 92);
    GfxColor body2 = GC(190, 235, 130);
    GfxColor line  = GC(58, 112, 42);
    GfxColor white = GC(252, 252, 248);
    GfxColor ink   = GC(38, 40, 40);

    float face = clampf(vx / 260.0f, -1.0f, 1.0f);
    float stretch = clampf(1.0f + vy * 0.00006f, 0.88f, 1.14f);
    float rx = 18.0f / stretch;
    float ry = 20.5f * stretch;

    /* Jetpack tank and exhaust sit behind the body. */
    if (power == PU_JETPACK) {
        gfx_round_rect(cx - 11.0f, cy - 8.0f, 22.0f, 30.0f, 7.0f, GC(126, 136, 150));
        gfx_round_rect_outline(cx - 11.0f, cy - 8.0f, 22.0f, 30.0f, 7.0f, 2.2f, GC(70, 78, 90));
        gfx_fill_rect(cx - 11.0f, cy + 8.0f, 22.0f, 5.0f, GC(240, 160, 60));
        float flick = 5.0f + 3.0f * sinf(t * 38.0f);
        float f1[6] = { cx - 7.0f, cy + 24.0f, cx + 7.0f, cy + 24.0f, cx, cy + 24.0f + flick + 8.0f };
        float f2[6] = { cx - 3.5f, cy + 24.0f, cx + 3.5f, cy + 24.0f, cx, cy + 24.0f + flick };
        gfx_polygon(f1, 3, GC(248, 150, 46));
        gfx_polygon(f2, 3, GC(255, 226, 110));
    }

    /* Feet and legs first so the body overlaps them. */
    gfx_ellipse(cx - 9.0f, cy + ry + 1.0f, 7.5f, 3.8f, line);
    gfx_ellipse(cx + 9.0f, cy + ry + 1.0f, 7.5f, 3.8f, line);
    gfx_line(cx - 7.0f, cy + ry - 3.0f, cx - 8.0f, cy + ry + 1.0f, 3.0f, line);
    gfx_line(cx + 7.0f, cy + ry - 3.0f, cx + 8.0f, cy + ry + 1.0f, 3.0f, line);

    /* Body */
    gfx_ellipse(cx, cy, rx, ry, body);
    gfx_ellipse_outline(cx, cy, rx, ry, 2.6f, line);

    /* Snout */
    float nose_x = cx + face * 8.0f;
    gfx_ellipse(nose_x, cy + 2.0f, 7.5f, 6.5f, body2);

    /* Eyes: white circles with pupils that follow the lean, plus a blink. */
    bool blink = fmodf(t + 0.4f, 3.9f) > 3.76f;
    float eye_x = cx + face * 3.0f;
    if (blink) {
        gfx_line(eye_x - 12.0f, cy - 6.0f, eye_x - 2.0f, cy - 6.0f, 2.2f, ink);
        gfx_line(eye_x + 2.0f, cy - 6.0f, eye_x + 12.0f, cy - 6.0f, 2.2f, ink);
    } else {
        gfx_circle(eye_x - 7.0f, cy - 6.0f, 5.8f, white);
        gfx_circle(eye_x + 7.0f, cy - 6.0f, 5.8f, white);
        gfx_circle(eye_x - 7.0f + face * 1.6f, cy - 5.4f, 2.8f, ink);
        gfx_circle(eye_x + 7.0f + face * 1.6f, cy - 5.4f, 2.8f, ink);
    }

    /* A little smile under the snout. */
    gfx_line(cx - 5.0f + face * 2.0f, cy + 10.0f, cx, cy + 12.0f, 2.0f, ink);
    gfx_line(cx, cy + 12.0f, cx + 6.0f + face * 2.0f, cy + 9.5f, 2.0f, ink);

    /* Arms */
    gfx_line(cx - rx + 1.0f, cy + 1.0f, cx - rx - 7.0f, cy - 3.0f, 3.0f, line);
    gfx_line(cx + rx - 1.0f, cy + 1.0f, cx + rx + 7.0f, cy - 3.0f, 3.0f, line);
    gfx_circle(cx - rx - 8.0f, cy - 4.0f, 3.2f, line);
    gfx_circle(cx + rx + 8.0f, cy - 4.0f, 3.2f, line);

    /* Propeller beanie */
    if (power == PU_PROPELLER && power_t > 0.0f) {
        gfx_ellipse(cx, cy - ry + 5.0f, 12.5f, 6.5f, GC(228, 84, 72));
        gfx_ellipse_outline(cx, cy - ry + 5.0f, 12.5f, 6.5f, 2.2f, GC(142, 40, 34));
        gfx_line(cx - 12.0f, cy - ry + 6.0f, cx + 12.0f, cy - ry + 6.0f, 2.0f, GC(142, 40, 34));
        gfx_line(cx, cy - ry + 4.0f, cx, cy - ry - 8.0f, 2.4f, GC(96, 102, 112));
        float a = t * 27.0f;
        float blade = cosf(a) * 16.0f;
        gfx_line(cx - blade, cy - ry - 9.0f, cx + blade, cy - ry - 9.0f, 4.0f, GC(96, 102, 112));
        gfx_circle(cx, cy - ry - 9.0f, 3.0f, GC(240, 240, 246));
    }
}

/* --------------------------------------------------------------- platforms */

static void draw_spring(float cx, float top, float squash)
{
    float base = top + 2.0f;
    float h = 16.0f - squash * 45.0f;
    if (h < 5.0f) h = 5.0f;

    gfx_fill_rect(cx - 10.0f, base - 1.0f, 20.0f, 4.0f, GC(146, 44, 38));

    float pts[16];
    int n = 0;
    for (int i = 0; i <= 6; i++) {
        float f = (float)i / 6.0f;
        pts[n++] = cx + ((i & 1) ? 6.5f : -6.5f);
        pts[n++] = base - 3.0f - h * f;
    }
    gfx_polyline(pts, 7, 2.4f, GC(112, 112, 126));

    gfx_round_rect(cx - 10.0f, base - 3.0f - h - 4.0f, 20.0f, 6.0f, 2.5f, GC(226, 76, 64));
    gfx_round_rect_outline(cx - 10.0f, base - 3.0f - h - 4.0f, 20.0f, 6.0f, 2.5f, 1.4f,
                           GC(140, 38, 32));
}

static void draw_powerup(float cx, float sy, PowerType type, u32 seed)
{
    float bob = sinf(time_now * 3.0f + (float)(seed & 255) * 0.07f) * 2.5f;
    float cy = sy - 27.0f + bob;

    if (type == PU_PROPELLER) {
        gfx_line(cx, cy + 7.0f, cx, cy + 1.0f, 2.2f, GC(96, 102, 112));
        gfx_ellipse(cx, cy + 8.0f, 11.5f, 6.0f, GC(228, 84, 72));
        gfx_ellipse_outline(cx, cy + 8.0f, 11.5f, 6.0f, 2.0f, GC(142, 40, 34));
        gfx_line(cx - 11.0f, cy + 9.0f, cx + 11.0f, cy + 9.0f, 2.0f, GC(142, 40, 34));
        float a = time_now * 22.0f + (float)(seed & 63);
        float blade = cosf(a) * 15.0f;
        gfx_line(cx - blade, cy - 1.0f, cx + blade, cy - 1.0f, 4.0f, GC(92, 98, 110));
        gfx_circle(cx, cy - 1.0f, 3.0f, GC(240, 240, 246));
    } else {
        gfx_round_rect(cx - 9.0f, cy - 4.0f, 18.0f, 24.0f, 6.0f, GC(128, 138, 152));
        gfx_round_rect_outline(cx - 9.0f, cy - 4.0f, 18.0f, 24.0f, 6.0f, 2.0f, GC(70, 78, 90));
        gfx_fill_rect(cx - 9.0f, cy + 8.0f, 18.0f, 5.0f, GC(240, 160, 60));
        gfx_line(cx - 5.0f, cy - 9.0f, cx - 5.0f, cy - 3.0f, 2.2f, GC(70, 78, 90));
        gfx_line(cx + 5.0f, cy - 9.0f, cx + 5.0f, cy - 3.0f, 2.2f, GC(70, 78, 90));
        float f = 5.0f + 2.0f * sinf(time_now * 21.0f);
        float p1[6] = { cx - 5.0f, cy + 20.0f, cx + 5.0f, cy + 20.0f, cx, cy + 20.0f + f + 6.0f };
        float p2[6] = { cx - 2.5f, cy + 20.0f, cx + 2.5f, cy + 20.0f, cx, cy + 20.0f + f };
        gfx_polygon(p1, 3, GC(248, 150, 46));
        gfx_polygon(p2, 3, GC(255, 226, 110));
    }
}

static void draw_platform_shape(float cx, float sy, float w, PlatType type,
                                PowerType power, float spring_t, u32 seed)
{
    GfxColor fill, line, light;

    switch (type) {
    case PF_MOVING:
        fill = GC(132, 200, 236); line = GC(40, 92, 132); light = GC(198, 236, 250);
        break;
    case PF_BREAK:
        fill = GC(198, 150, 96); line = GC(110, 70, 36); light = GC(232, 198, 152);
        break;
    case PF_VANISH:
        fill = GC(246, 246, 236); line = GC(150, 170, 186); light = GC(255, 255, 255);
        break;
    default:
        fill = GC(150, 214, 110); line = GC(54, 110, 44); light = GC(198, 236, 158);
        break;
    }

    float x = cx - w * 0.5f;
    const float h = 16.0f;
    const float r = 7.0f;

    gfx_round_rect(x + 2.0f, sy + 3.0f, w, h, r, GCA(120, 128, 118, 42));
    gfx_round_rect(x, sy, w, h, r, fill);
    gfx_round_rect_outline(x, sy, w, h, r, 2.6f, line);

    /* Highlight and hand-drawn hatching. */
    gfx_line(x + 10.0f, sy + 4.5f, x + w - 10.0f, sy + 4.5f, 2.2f, light);
    for (int i = 0; i < 3; i++) {
        float hx = x + w * (0.24f + 0.24f * (float)i) + jitter(seed, i) * 2.0f;
        gfx_line(hx, sy + 7.5f, hx + w * 0.10f, sy + 13.0f, 1.5f, light);
    }

    if (type == PF_BREAK) {
        float crack[10] = {
            x + w * 0.42f, sy + 1.5f,
            x + w * 0.51f, sy + 5.5f,
            x + w * 0.44f, sy + 9.0f,
            x + w * 0.55f, sy + 14.5f,
        };
        gfx_polyline(crack, 4, 1.8f, line);
    } else if (type == PF_MOVING) {
        gfx_line(x + 8.0f, sy + 8.0f, x + 13.0f, sy + 5.0f, 1.6f, line);
        gfx_line(x + 8.0f, sy + 8.0f, x + 13.0f, sy + 11.0f, 1.6f, line);
        gfx_line(x + w - 8.0f, sy + 8.0f, x + w - 13.0f, sy + 5.0f, 1.6f, line);
        gfx_line(x + w - 8.0f, sy + 8.0f, x + w - 13.0f, sy + 11.0f, 1.6f, line);
    }

    if (power == PU_SPRING) {
        draw_spring(cx, sy, spring_t);
    } else if (power == PU_PROPELLER || power == PU_JETPACK) {
        draw_powerup(cx, sy, power, seed);
    }
}

static void draw_platform(const Platform *p)
{
    float sy = scr_y(p->y);
    if (sy < -70.0f || sy > SCR_H + 40.0f) return;
    draw_platform_shape(p->x, sy, p->w, p->type, p->power, p->spring_t, p->seed);
}

/* --------------------------------------------------------------- monsters */

static void draw_monster(const Monster *m)
{
    float sx = m->x;
    float sy = scr_y(m->y);
    if (sy < -60.0f || sy > SCR_H + 60.0f) return;

    GfxColor body = GC(199, 112, 208);
    GfxColor line = GC(112, 44, 120);
    GfxColor white = GC(252, 252, 248);
    GfxColor ink = GC(40, 30, 44);

    float wob = 1.0f + 0.05f * sinf(time_now * 5.0f + m->phase);
    float rx = 21.0f * wob;
    float ry = 16.0f / wob;

    gfx_ellipse(sx + 2.0f, sy + ry + 10.0f, rx * 0.85f, 4.0f, GCA(120, 100, 120, 40));

    /* feet */
    gfx_ellipse(sx - 9.0f, sy + ry + 4.0f, 7.5f, 4.0f, line);
    gfx_ellipse(sx + 9.0f, sy + ry + 4.0f, 7.5f, 4.0f, line);

    /* little horns */
    float h1[6] = { sx - 13.0f, sy - 9.0f, sx - 5.0f, sy - 11.0f, sx - 11.0f, sy - 24.0f };
    float h2[6] = { sx + 13.0f, sy - 9.0f, sx + 5.0f, sy - 11.0f, sx + 11.0f, sy - 24.0f };
    gfx_polygon(h1, 3, line);
    gfx_polygon(h2, 3, line);

    gfx_ellipse(sx, sy, rx, ry, body);
    gfx_ellipse_outline(sx, sy, rx, ry, 2.6f, line);

    gfx_circle(sx - 7.0f, sy - 4.0f, 5.6f, white);
    gfx_circle(sx + 7.0f, sy - 4.0f, 5.6f, white);
    float px = (m->vx > 0.0f) ? 1.7f : -1.7f;
    gfx_circle(sx - 7.0f + px, sy - 4.0f, 2.6f, ink);
    gfx_circle(sx + 7.0f + px, sy - 4.0f, 2.6f, ink);

    float mouth[10] = {
        sx - 9.0f, sy + 7.0f,
        sx - 4.5f, sy + 4.0f,
        sx,        sy + 8.0f,
        sx + 4.5f, sy + 4.0f,
        sx + 9.0f, sy + 7.0f,
    };
    gfx_polyline(mouth, 5, 2.2f, line);
}

/* ----------------------------------------------------------------- bullets */

static void draw_bullets(void)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        const Bullet *b = &bullets[i];
        if (!b->active) continue;
        float sy = scr_y(b->y);
        if (sy < -30.0f || sy > SCR_H + 30.0f) continue;
        gfx_line(b->x, sy + 12.0f, b->x, sy - 3.0f, 3.4f, GCA(178, 220, 120, 170));
        gfx_ellipse(b->x, sy, 3.3f, 7.5f, GC(250, 234, 92));
        gfx_ellipse_outline(b->x, sy, 3.3f, 7.5f, 1.4f, GC(168, 148, 30));
    }
}

/* -------------------------------------------------------------- background */

static void draw_cloud(float x, float y, float s)
{
    GfxColor white = GCA(255, 255, 252, 215);
    GfxColor edge = GCA(210, 228, 240, 150);

    gfx_ellipse(x, y, 34.0f * s, 15.0f * s, white);
    gfx_ellipse(x - 22.0f * s, y + 5.0f * s, 20.0f * s, 11.0f * s, white);
    gfx_ellipse(x + 24.0f * s, y + 4.0f * s, 21.0f * s, 12.0f * s, white);
    gfx_line(x - 38.0f * s, y + 9.0f * s, x + 38.0f * s, y + 9.0f * s, 1.4f, edge);
}

static void draw_background(void)
{
    gfx_clear(GC(253, 253, 245));

    /* Faint notebook grid: vertical lines are fixed, horizontal ones drift
     * with the camera so climbing has some sense of speed. */
    for (int x = 0; x < (int)SCR_W; x += 64)
        gfx_fill_rect((float)x, 0.0f, 1.0f, SCR_H, GC(238, 244, 249));

    float off = fmodf(cam_y * 0.5f, 64.0f);
    if (off < 0.0f) off += 64.0f;
    for (float y = off - 64.0f; y < SCR_H; y += 64.0f)
        gfx_fill_rect(0.0f, y, SCR_W, 1.0f, GC(238, 244, 249));

    static const struct { float x, y, s; } clouds[] = {
        {  92.0f,  80.0f, 1.00f },
        { 402.0f, 250.0f, 1.25f },
        { 566.0f, 460.0f, 0.80f },
        { 196.0f, 640.0f, 1.10f },
        { 470.0f, 790.0f, 0.95f },
    };
    for (int i = 0; i < 5; i++) {
        float cy = fmodf(clouds[i].y + cam_y * 0.35f, 820.0f) - 40.0f;
        if (cy < -50.0f || cy > SCR_H + 50.0f) continue;
        draw_cloud(clouds[i].x, cy, clouds[i].s);
    }
}

/* ------------------------------------------------------------------- HUD */

static void text_right(const char *s, float right, float y, float size, GfxColor c)
{
    gfx_text(s, right - gfx_text_len(s, size), y, size, c);
}

static void draw_hud(void)
{
    char buf[48];

    snprintf(buf, sizeof(buf), "%d", score);
    gfx_text(buf, 20.0f, 12.0f, 38.0f, GC(46, 80, 120));
    gfx_text("SCORE", 23.0f, 56.0f, 12.0f, GC(132, 150, 168));

    int shown_best = best > score ? best : score;
    snprintf(buf, sizeof(buf), "BEST %d", shown_best);
    text_right(buf, 620.0f, 16.0f, 18.0f, GC(96, 118, 138));

#ifdef AUTOPLAY
    gfx_text("AUTO DEMO", 20.0f, 442.0f, 13.0f, GC(160, 168, 176));
#endif
}

static void draw_title(void)
{    /* Title lettering, hand drawn twice for a chunky offset shadow. */
    gfx_text_center("DOODLE", 323.0f, 39.0f, 68.0f, GC(206, 230, 182));
    gfx_text_center("DOODLE", 320.0f, 36.0f, 68.0f, GC(108, 178, 64));
    gfx_text_center("JUMP", 323.0f, 109.0f, 68.0f, GC(198, 224, 238));
    gfx_text_center("JUMP", 320.0f, 106.0f, 68.0f, GC(56, 130, 186));

    float bob = sinf(time_now * 3.0f) * 3.0f;
    draw_platform_shape(320.0f, 330.0f, 164.0f, PF_NORMAL, PU_NONE, 0.0f, 0x51ab7u);
    draw_doodler(320.0f, 304.0f + bob, 0.0f, 0.0f, PU_NONE, 0.0f, time_now);

    gfx_text_center("Wii edition", 320.0f, 362.0f, 19.0f, GC(120, 138, 150));

    if (fmodf(time_now, 1.1f) < 0.75f)
        gfx_text_center("PRESS  A  TO  START", 320.0f, 390.0f, 24.0f, GC(46, 80, 120));

    gfx_text_center("stick / tilt / d-pad  move      A or 2  shoot      + / -  pause",
                    320.0f, 424.0f, 13.5f, GC(104, 120, 134));
    gfx_text_center("original art and music  -  inspired by Doodle Jump",
                    320.0f, 448.0f, 12.0f, GC(158, 168, 176));
}

static void draw_pause_overlay(void)
{
    gfx_set_dim(105, 126, 146, 514, 334);
    gfx_round_rect(128.0f, 148.0f, 384.0f, 184.0f, 24.0f, GC(253, 253, 246));
    gfx_round_rect_outline(128.0f, 148.0f, 384.0f, 184.0f, 24.0f, 3.0f, GC(70, 96, 118));

    gfx_text_center("PAUSED", 320.0f, 170.0f, 46.0f, GC(50, 84, 122));
    gfx_text_center("+  or  A      resume", 320.0f, 238.0f, 19.0f, GC(70, 86, 100));
    gfx_text_center("B             back to title", 320.0f, 270.0f, 19.0f, GC(70, 86, 100));
}

static void draw_gameover_overlay(void)
{
    char buf[48];

    gfx_set_dim(95, 94, 102, 546, 374);
    gfx_round_rect(96.0f, 104.0f, 448.0f, 268.0f, 26.0f, GC(253, 253, 246));
    gfx_round_rect_outline(96.0f, 104.0f, 448.0f, 268.0f, 26.0f, 3.0f, GC(150, 90, 80));

    gfx_text_center("GAME OVER", 320.0f, 124.0f, 48.0f, GC(196, 74, 62));

    snprintf(buf, sizeof(buf), "SCORE   %d", score);
    gfx_text_center(buf, 320.0f, 188.0f, 28.0f, GC(52, 78, 104));
    snprintf(buf, sizeof(buf), "BEST    %d", best);
    gfx_text_center(buf, 320.0f, 222.0f, 22.0f, GC(106, 124, 140));

    if (new_best && fmodf(time_now, 0.7f) < 0.45f)
        gfx_text_center("NEW BEST!", 320.0f, 252.0f, 20.0f, GC(214, 146, 40));

    gfx_text_center("A  play again        B  title", 320.0f, 306.0f, 20.0f, GC(70, 86, 100));
}

/* ---------------------------------------------------------------- spawning */

static Platform *free_platform(void)
{
    for (int i = 0; i < MAX_PLATFORMS; i++)
        if (!platforms[i].active) return &platforms[i];
    return NULL;
}

static Monster *free_monster(void)
{
    for (int i = 0; i < MAX_MONSTERS; i++)
        if (!monsters[i].active) return &monsters[i];
    return NULL;
}

static int active_monsters(void)
{
    int n = 0;
    for (int i = 0; i < MAX_MONSTERS; i++)
        if (monsters[i].active) n++;
    return n;
}

static void spawn_platform(float y, bool base)
{
    Platform *p = free_platform();
    if (!p) return;
    memset(p, 0, sizeof(*p));
    p->active = true;
    p->y = y;
    p->seed = rng_next();
    p->type = PF_NORMAL;
    p->power = PU_NONE;

    if (base) {
        p->x = SCR_W * 0.5f;
        p->w = 168.0f;
        last_platform_x = p->x;
        last_solid_x = p->x;
        last_platform_type = PF_NORMAL;
        return;
    }

    int d = (int)(player.max_height / 1100.0f);
    if (d > 5) d = 5;

    int r = (int)(rng_next() % 100u);
    int br = 3 + d * 4;
    int va = br + 2 + d * 3;
    int mv = va + 8 + d * 3;
    if (r < br) p->type = PF_BREAK;
    else if (r < va) p->type = PF_VANISH;
    else if (r < mv) p->type = PF_MOVING;

    bool prev_fragile = (last_platform_type == PF_BREAK ||
                         last_platform_type == PF_VANISH);
    /* Two crumbling platforms in a row can both vanish, leaving the next solid
     * surface higher than a whole jump.  Force a normal platform between. */
    if (prev_fragile && (p->type == PF_BREAK || p->type == PF_VANISH))
        p->type = PF_NORMAL;

    p->w = (rng_next() % 100 < 16) ? frand(116.0f, 150.0f) : frand(86.0f, 104.0f);
    float margin = p->w * 0.5f + 12.0f;
    p->x = frand(margin, SCR_W - margin);

    /* Keep the new platform near the surface the player will actually jump
     * from.  After a fragile one that surface is the fragile platform itself,
     * and the step must also stay near the last solid, so the jump still works
     * when the fragile platform has already vanished. */
    float lim = prev_fragile ? 120.0f : 170.0f;
    float lo = last_platform_x - lim;
    float hi = last_platform_x + lim;
    if (prev_fragile) {
        if (lo < last_solid_x - 140.0f) lo = last_solid_x - 140.0f;
        if (hi > last_solid_x + 140.0f) hi = last_solid_x + 140.0f;
    }
    if (p->x < lo) p->x = lo;
    if (p->x > hi) p->x = hi;
    if (p->x < margin) p->x = margin;
    if (p->x > SCR_W - margin) p->x = SCR_W - margin;

    last_platform_x = p->x;
    if (p->type == PF_NORMAL || p->type == PF_MOVING) last_solid_x = p->x;

    if (p->type == PF_MOVING) {
        p->speed = frand(45.0f, 90.0f);
        p->dir = (rng_next() & 1u) ? 1.0f : -1.0f;
    }

    if (p->type == PF_NORMAL || p->type == PF_MOVING) {
        int roll = (int)(rng_next() % 100u);
        if (roll < 7) p->power = PU_SPRING;
        else if (roll < 11) p->power = PU_PROPELLER;
        else if (roll < 14) p->power = PU_JETPACK;
    }

    last_platform_type = p->type;

    /* Monsters start showing up once the player has some height, and never
     * camp on a platform that is about to crumble. */
    bool solid = (p->type == PF_NORMAL || p->type == PF_MOVING);
    if (solid && player.max_height > 900.0f && active_monsters() < MAX_MONSTERS) {
        int chance = 6 + d * 2;
        if ((int)(rng_next() % 100u) < chance) {
            Monster *m = free_monster();
            if (m) {
                memset(m, 0, sizeof(*m));
                m->active = true;
                m->y = p->y + 24.0f;
                m->x = p->x;
                m->base_x = p->x;
                m->range = p->w * 0.35f + 12.0f;
                m->vx = frand(34.0f, 62.0f) * ((rng_next() & 1u) ? 1.0f : -1.0f);
                m->phase = frand(0.0f, 6.28f);
                m->seed = rng_next();
            }
        }
    }
}

static void generate_world(void)
{
    while (next_platform_y < cam_y + 820.0f) {
        /* A fragile platform can disappear after it is used, so the next one
         * must sit close enough that the jump below it still works. */
        float gap = (last_platform_type == PF_BREAK ||
                     last_platform_type == PF_VANISH)
            ? frand(58.0f, 72.0f) : frand(64.0f, 88.0f);
        next_platform_y += gap;
        spawn_platform(next_platform_y, false);
    }
}

static void cleanup_world(void)
{
    for (int i = 0; i < MAX_PLATFORMS; i++) {
        Platform *p = &platforms[i];
        if (p->active && p->y < cam_y - 130.0f) p->active = false;
    }
    for (int i = 0; i < MAX_MONSTERS; i++) {
        Monster *m = &monsters[i];
        if (m->active && m->y < cam_y - 130.0f) m->active = false;
    }
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (b->active && (b->y > cam_y + SCR_H + 40.0f || b->y < cam_y - 80.0f))
            b->active = false;
    }
}

static void break_platform(Platform *p)
{
    for (int i = 0; i < 7; i++) {
        float ox = frand(-p->w * 0.45f, p->w * 0.45f);
        spawn_particle(p->x + ox, p->y + frand(-2.0f, 12.0f),
                       ox * 1.6f, frand(30.0f, 150.0f),
                       frand(0.5f, 0.9f), frand(3.0f, 6.0f), 1500.0f,
                       GC(186, 142, 92));
    }
    p->active = false;
}

static void vanish_platform(Platform *p)
{
    for (int i = 0; i < 8; i++) {
        spawn_particle(p->x + frand(-p->w * 0.4f, p->w * 0.4f), p->y + frand(0.0f, 12.0f),
                       frand(-40.0f, 40.0f), frand(30.0f, 120.0f),
                       frand(0.35f, 0.7f), frand(2.0f, 4.0f), 700.0f,
                       GCA(220, 232, 242, 230));
    }
    p->active = false;
}

/* ------------------------------------------------------------------ reset */

static void reset_world(void)
{
    memset(platforms, 0, sizeof(platforms));
    memset(monsters, 0, sizeof(monsters));
    memset(bullets, 0, sizeof(bullets));
    memset(particles, 0, sizeof(particles));

    player.x = SCR_W * 0.5f;
    player.y = PLAYER_HH;
    player.vx = 0.0f;
    player.vy = 0.0f;
    player.max_height = player.y;
    player.power_t = 0.0f;
    player.power = PU_NONE;
    player.shoot_cd = 0.0f;
    player.facing = 1;

    cam_y = CAM_START;
    next_platform_y = 66.0f;
    last_platform_x = SCR_W * 0.5f;
    last_solid_x = SCR_W * 0.5f;
    score = 0;
    new_best = false;
    dead_timer = 0.0f;

    spawn_platform(0.0f, true);
    generate_world();
}

static void start_game(void)
{
    reset_world();
    state = ST_PLAY;
#ifdef AUTOPLAY
    auto_timer = 0.0f;
#endif
}

void game_init(void)
{
    game_seed = 0xC0FFEEu;
    state = ST_TITLE;
    best = 0;
    time_now = 0.0f;
    cam_y = CAM_START;
    reset_world();
    state = ST_TITLE;
}

/* -------------------------------------------------------------- game logic */

static void land_on(Platform *p)
{
    player.y = p->y + PLAYER_HH;

    if (p->type == PF_BREAK) {
        break_platform(p);
        sfx_break();
        /* Still give a weaker bounce: dropping the player can leave the next
         * platform more than a full jump away. */
        player.vy = JUMP_VY * 0.72f;
        return;
    }

    if (p->power == PU_SPRING) {
        player.vy = SPRING_VY;
        p->spring_t = 0.22f;
        sfx_spring();
    } else {
        player.vy = JUMP_VY;
        sfx_jump();
    }

    player.power = PU_NONE;
    player.power_t = 0.0f;

    if (p->type == PF_VANISH) vanish_platform(p);
    if (p->type == PF_MOVING) player.vx += p->dir * p->speed * 0.25f;

    dust(player.x, p->y);
}

static void collect_powerup(Platform *p)
{
    float py = p->y + 26.0f;
    if (fabsf(player.x - p->x) > 30.0f || fabsf(player.y - py) > 40.0f) return;

    PowerType type = p->power;
    player.power = type;
    player.power_t = (type == PU_JETPACK) ? 1.35f : 0.85f;
    player.vy = 520.0f;
    p->power = PU_NONE;

    sfx_powerup();
    burst(player.x, player.y, 10,
          type == PU_JETPACK ? GC(250, 180, 70) : GC(240, 120, 110), 160.0f);
}

static void update_platforms(float dt)
{
    for (int i = 0; i < MAX_PLATFORMS; i++) {
        Platform *p = &platforms[i];
        if (!p->active) continue;

        if (p->spring_t > 0.0f) {
            p->spring_t -= dt;
            if (p->spring_t < 0.0f) p->spring_t = 0.0f;
        }

        if (p->type == PF_MOVING) {
            p->x += p->dir * p->speed * dt;
            float lo = p->w * 0.5f + 8.0f;
            float hi = SCR_W - p->w * 0.5f - 8.0f;
            if (p->x < lo) { p->x = lo; p->dir = 1.0f; }
            if (p->x > hi) { p->x = hi; p->dir = -1.0f; }
        }
    }
}

static void update_monsters(float dt, float prev_y)
{
    for (int i = 0; i < MAX_MONSTERS; i++) {
        Monster *m = &monsters[i];
        if (!m->active) continue;

        m->x += m->vx * dt;
        if (m->x < m->base_x - m->range) { m->x = m->base_x - m->range; m->vx = fabsf(m->vx); }
        if (m->x > m->base_x + m->range) { m->x = m->base_x + m->range; m->vx = -fabsf(m->vx); }

        if (fabsf(player.x - m->x) > 28.0f || fabsf(player.y - m->y) > 32.0f) continue;

        if (player.vy < -40.0f && prev_y > m->y + 4.0f) {
            /* Stomp: bounce off the monster and squash it. */
            m->active = false;
            player.vy = JUMP_VY * 1.06f;
            sfx_monster();
            burst(m->x, m->y, 12, GC(196, 112, 206), 190.0f);
        } else {
            state = ST_DEAD;
            dead_timer = 0.0f;
            if (score > best) { best = score; new_best = true; }
            sfx_death();
            burst(player.x, player.y, 16, GC(150, 205, 96), 210.0f);
            return;
        }
    }
}

static void update_bullets(float dt)
{
    for (int i = 0; i < MAX_BULLETS; i++) {
        Bullet *b = &bullets[i];
        if (!b->active) continue;

        b->y += b->vy * dt;

        bool hit = false;
        for (int j = 0; j < MAX_MONSTERS; j++) {
            Monster *m = &monsters[j];
            if (!m->active) continue;
            if (fabsf(b->x - m->x) < 25.0f && fabsf(b->y - m->y) < 24.0f) {
                m->active = false;
                hit = true;
                sfx_monster();
                burst(m->x, m->y, 12, GC(196, 112, 206), 190.0f);
                break;
            }
        }
        if (hit) b->active = false;
    }
}

static void update_camera(void)
{
    if (player.y - cam_y > CAM_ANCHOR)
        cam_y = player.y - CAM_ANCHOR;
    if (cam_y < CAM_START) cam_y = CAM_START;
}

static void update_play(const Input *in, float dt)
{
    update_platforms(dt);

    /* Horizontal control: nunchuk stick / tilt / d-pad all arrive as axis. */
    float axis = in->axis;
    if (axis > 0.12f || axis < -0.12f) {
        player.vx += axis * MOVE_ACCEL * dt;
        if (player.vx > MOVE_MAX) player.vx = MOVE_MAX;
        if (player.vx < -MOVE_MAX) player.vx = -MOVE_MAX;
        player.facing = axis > 0.0f ? 1 : -1;
    } else {
        float drag = MOVE_DRAG * dt;
        if (fabsf(player.vx) <= drag) player.vx = 0.0f;
        else player.vx -= (player.vx > 0.0f ? 1.0f : -1.0f) * drag;
    }

    if (player.power_t > 0.0f) {
        player.power_t -= dt;
        player.vy = (player.power == PU_JETPACK) ? 1350.0f : 950.0f;
        if (fmodf(time_now, 0.03f) < 0.018f) {
            float fx = player.x + frand(-8.0f, 8.0f);
            float fy = player.y - PLAYER_HH - 4.0f;
            GfxColor c = (player.power == PU_JETPACK) ? GC(248, 160, 60) : GC(180, 190, 205);
            spawn_particle(fx, fy, frand(-30.0f, 30.0f), -frand(60.0f, 140.0f),
                           0.35f, frand(2.0f, 4.0f), 0.0f, c);
        }
        if (player.power_t <= 0.0f) {
            player.power_t = 0.0f;
            player.power = PU_NONE;
            player.vy = 640.0f;
        }
    } else {
        player.vy -= GRAVITY * dt;
        if (player.vy < -MAX_FALL) player.vy = -MAX_FALL;
    }

    float prev_y = player.y;
    player.x += player.vx * dt;
    player.y += player.vy * dt;

    if (player.x < -24.0f) player.x = SCR_W + 24.0f;
    if (player.x > SCR_W + 24.0f) player.x = -24.0f;

    if (player.y > player.max_height) player.max_height = player.y;

    /* One way platform collision: only when falling and not under power. */
    if (player.power_t <= 0.0f && player.vy <= 0.0f) {
        float feet_prev = prev_y - PLAYER_HH;
        float feet_now = player.y - PLAYER_HH;

        for (int i = 0; i < MAX_PLATFORMS; i++) {
            Platform *p = &platforms[i];
            if (!p->active) continue;
            if (p->y > feet_prev + 4.0f) continue;
            if (p->y < feet_now - 4.0f) continue;
            if (feet_prev < p->y - 4.0f) continue;
            if (fabsf(player.x - p->x) > p->w * 0.5f + 10.0f) continue;
            land_on(p);
            break;
        }
    }

    /* Powerup pickups hover just above their platform. */
    for (int i = 0; i < MAX_PLATFORMS; i++) {
        Platform *p = &platforms[i];
        if (!p->active) continue;
        if (p->power == PU_PROPELLER || p->power == PU_JETPACK)
            collect_powerup(p);
    }

    update_monsters(dt, prev_y);
    if (state != ST_PLAY) return;

    /* Nose shots. */
    if (player.shoot_cd > 0.0f) player.shoot_cd -= dt;
    if (in->shoot && player.shoot_cd <= 0.0f && player.power_t <= 0.0f) {
        for (int i = 0; i < MAX_BULLETS; i++) {
            Bullet *b = &bullets[i];
            if (b->active) continue;
            b->active = true;
            b->x = player.x;
            b->y = player.y + 20.0f;
            b->vy = 1250.0f;
            break;
        }
        player.shoot_cd = 0.22f;
        sfx_shoot();
    }
    update_bullets(dt);
    update_particles(dt);

    update_camera();
    generate_world();
    cleanup_world();

    int s = (int)((player.max_height - PLAYER_HH) * SCORE_PER_PX);
    if (s < 0) s = 0;
    score = s;

    /* Falling off the bottom of the screen ends the run. */
    if (player.y < cam_y - 28.0f) {
        state = ST_DEAD;
        dead_timer = 0.0f;
        if (score > best) { best = score; new_best = true; }
        sfx_death();
        burst(player.x, player.y, 14, GC(150, 205, 96), 190.0f);
    }
}

static void update_dead(float dt)
{
    player.vy -= GRAVITY * 0.55f * dt;
    player.x += player.vx * dt;
    player.y += player.vy * dt;
    update_particles(dt);
}

/* --------------------------------------------------------------- autoplay */

#ifdef AUTOPLAY
/* Furthest the doodler can travel sideways in `t` seconds while accelerating
 * towards `dir` (+1/-1) from its current velocity. */
static float h_travel(float dir, float t)
{
    float v0 = player.vx * dir;
    float tc = (MOVE_MAX - v0) / MOVE_ACCEL;
    if (tc < 0.0f) tc = 0.0f;
    if (t <= tc) return v0 * t + 0.5f * MOVE_ACCEL * t * t;
    return v0 * tc + 0.5f * MOVE_ACCEL * tc * tc + MOVE_MAX * (t - tc);
}

/* Choose the platform the scripted player should land on: the highest one it
 * can still get to before the current bounce comes back down.  The returned
 * time is the vertical flight time to that surface, used to predict moving
 * platforms and to reject targets it cannot cross to horizontally. */
static bool pick_platform(Platform **out, float *out_t)
{
    Platform *best = NULL;
    float best_score = -1e9f, best_dx = 1e9f, best_t = 0.0f;

    auto_nup = 0;
    auto_nok = 0;
    auto_topx = auto_topy = -9999.0f;
    auto_apex = player.y + PLAYER_HH + (player.vy > 0.0f
        ? player.vy * player.vy / (2.0f * GRAVITY) : 0.0f);

    for (int i = 0; i < MAX_PLATFORMS; i++) {
        Platform *p = &platforms[i];
        if (!p->active) continue;
        /* Include everything the player can still land on this fall, not just
         * what is above the camera lip, or the autopilot ignores the platform
         * it is about to bounce from. */
        if (p->y < cam_y - 70.0f) continue;
        if (p->y > player.y + PLAYER_HH - 4.0f &&
            p->y <= auto_apex) {
            auto_nup++;
            if (p->y > auto_topy) { auto_topy = p->y; auto_topx = p->x; }
        }

        /* Height of the feet above this surface.  A negative value means the
         * player is already past it, which is fine while it is still falling. */
        float d = (p->y + PLAYER_HH) - player.y;
        float disc = player.vy * player.vy - 2.0f * GRAVITY * d;
        if (disc <= 0.0f) continue;          /* above this bounce's apex */
        float t = (player.vy + sqrtf(disc)) / GRAVITY;
        if (t <= 0.0f) continue;

        float px = p->x;
        if (p->type == PF_MOVING) px += p->dir * p->speed * t;
        float dx = px - player.x;

        /* How far it can drift sideways before it reaches that surface, plus
         * the landing tolerance on either side of the platform. */
        float reach = h_travel(dx >= 0.0f ? 1.0f : -1.0f, t)
                    + p->w * 0.5f + 10.0f;
        if (fabsf(dx) > reach) continue;
        auto_nok++;

        /* Prefer height, but stay off crumbling/vanishing surfaces when a
         * normal one is nearly as high. */
        float score = p->y;
        if (p->type == PF_BREAK) score -= 55.0f;
        if (p->type == PF_VANISH) score -= 25.0f;

        if (score > best_score + 0.5f ||
            (fabsf(score - best_score) <= 0.5f && fabsf(dx) < best_dx)) {
            best_score = score;
            best_dx = fabsf(dx);
            best = p;
            best_t = t;
        }
    }

    if (best) {
        *out = best;
        *out_t = best_t;
    }
    return best != NULL;
}

static void autopilot(Input *in, float dt)
{
    if (dt <= 0.0f) dt = 0.016f;

    in->shoot = false;

    if (player.power_t > 0.0f) {
        auto_want = -1.0f;
        in->axis = clampf((SCR_W * 0.5f - player.x) / 120.0f, -1.0f, 1.0f);
        return;
    }

    /* Aim at the highest surface this bounce can still reach; once falling,
     * that is naturally the highest platform below.  The old controller used
     * to drift to the middle of the screen and miss everything underneath. */
    Platform *target = NULL;
    float target_t = 0.0f;
    pick_platform(&target, &target_t);

    float want_x = SCR_W * 0.5f;
    if (target) {
        want_x = target->x;
        if (target->type == PF_MOVING)
            want_x += target->dir * target->speed * target_t;
    }

    /* Rising into a monster kills, so slip around one that sits on the path
     * (landing on it from above is a stomp, handled below). */
    if (player.vy > 0.0f) {
        for (int i = 0; i < MAX_MONSTERS; i++) {
            Monster *m = &monsters[i];
            if (!m->active) continue;
            float mdy = m->y - player.y;
            if (mdy < -10.0f || mdy > 160.0f) continue;
            if (fabsf(m->x - player.x) > 70.0f) continue;
            if (fabsf(m->x - want_x) > 90.0f) continue;
            want_x = m->x + (want_x >= m->x ? 78.0f : -78.0f);
            break;
        }
    } else {
        /* Descending: line up on a nearby monster to stomp it. */
        for (int i = 0; i < MAX_MONSTERS; i++) {
            Monster *m = &monsters[i];
            if (!m->active) continue;
            float mdy = m->y - player.y;
            if (mdy < -95.0f || mdy > 8.0f) continue;
            if (fabsf(m->x - player.x) < 75.0f) { want_x = m->x; break; }
        }
    }

    auto_want = target ? want_x : -1.0f;
    auto_ty = target ? target->y : -9999.0f;
    if (want_x < 26.0f) want_x = 26.0f;
    if (want_x > SCR_W - 26.0f) want_x = SCR_W - 26.0f;

    /* Bang-bang with stopping-distance prediction.  The game only applies
     * drag when the stick is centred, so coast as soon as drag alone would
     * stop us inside the landing window. */
    float dxw = want_x - player.x;
    float stop = player.vx * player.vx / (2.0f * MOVE_DRAG);
    if (fabsf(dxw) <= 6.0f && fabsf(player.vx) < 45.0f) {
        in->axis = 0.0f;
    } else if (dxw > 0.0f) {
        in->axis = (player.vx > 0.0f && stop > dxw - 8.0f) ? 0.0f : 1.0f;
    } else {
        in->axis = (player.vx < 0.0f && stop > -dxw - 8.0f) ? 0.0f : -1.0f;
    }

    if (player.shoot_cd <= 0.0f) {
        for (int i = 0; i < MAX_MONSTERS; i++) {
            Monster *m = &monsters[i];
            if (!m->active) continue;
            float dy = m->y - player.y;
            if (dy < 12.0f || dy > 320.0f) continue;
            float lead = m->vx * (dy / 1250.0f);
            if (fabsf(m->x + lead - player.x) < 26.0f) {
                in->shoot = true;
                break;
            }
        }
    }
}
#endif

/* ------------------------------------------------------------------ update */

void game_update(const Input *in, float dt)
{
    if (dt < 0.001f) dt = 0.001f;
    if (dt > 0.05f) dt = 0.05f;

    time_now += dt;

    Input local = *in;
    (void)local;

#ifdef AUTOPLAY
    if (state == ST_TITLE) {
        auto_timer += dt;
        if (auto_timer > 1.2f) start_game();
    } else if (state == ST_DEAD) {
        auto_timer += dt;
        if (auto_timer > 1.6f) start_game();
    }
    if (state == ST_PLAY) autopilot(&local, dt);
#endif

    switch (state) {
    case ST_TITLE:
        if (local.start) start_game();
        break;

    case ST_PLAY:
        if (local.pause) { state = ST_PAUSE; break; }
        if (local.back) { state = ST_TITLE; break; }
        update_play(&local, dt);
        break;

    case ST_PAUSE:
        if (local.pause || local.start) { state = ST_PLAY; break; }
        if (local.back) { state = ST_TITLE; break; }
        break;

    case ST_DEAD:
        dead_timer += dt;
        update_dead(dt);
        if (dead_timer > 0.55f) {
            if (local.start) start_game();
            else if (local.back) {
                state = ST_TITLE;
#ifdef AUTOPLAY
                auto_timer = 0.0f;
#endif
            }
        }
        break;
    }

#ifdef DEBUG_LOG
    {
        static int dbg = 0;
        if (++dbg >= 60) {
            dbg = 0;
            printf("dj state=%d score=%d best=%d y=%.1f vy=%.1f cam=%.1f\n",
                   state, score, best, player.y, player.vy, cam_y);
        }
    }
#endif
}

/* -------------------------------------------------------------------- draw */

void game_draw(void)
{
    gfx_set_dim(0, 0, 0, 0, 0);
    prof_begin();
    draw_background();
    prof_end(0);

    if (state == ST_TITLE) {
        draw_title();
        prof_end(1);
#ifdef TELEMETRY
        for (int i = 2; i < PROF_SLOTS; i++) prof_us[i] = 0.0f;
#endif
        return;
    }

    prof_begin();
    for (int i = 0; i < MAX_PLATFORMS; i++)
        if (platforms[i].active) draw_platform(&platforms[i]);
    prof_end(1);

    prof_begin();
    draw_particles();
    for (int i = 0; i < MAX_MONSTERS; i++)
        if (monsters[i].active) draw_monster(&monsters[i]);
    draw_bullets();
    prof_end(2);

    prof_begin();
    draw_doodler(player.x, scr_y(player.y), player.vx, player.vy,
                 player.power, player.power_t, time_now);
    prof_end(3);

    prof_begin();
    draw_hud();
    prof_end(4);

    prof_begin();
    if (state == ST_PAUSE) draw_pause_overlay();
    else if (state == ST_DEAD) draw_gameover_overlay();
    prof_end(5);
}

/* ----------------------------------------------------------------- getters */

int game_score(void) { return score; }
int game_best(void)  { return best; }

void game_debug(GameDebug *out)
{
    int np = 0, nm = 0;
    for (int i = 0; i < MAX_PLATFORMS; i++) if (platforms[i].active) np++;
    for (int i = 0; i < MAX_MONSTERS; i++) if (monsters[i].active) nm++;

    out->state = state;
    out->score = score;
    out->best = best;
    out->platforms = np;
    out->monsters = nm;
    out->y = player.y;
    out->cam = cam_y;
    out->max_height = player.max_height;
    out->vy = player.vy;
    out->x = player.x;
    out->vx = player.vx;
#ifdef AUTOPLAY
    out->want = auto_want;
    out->want_y = auto_ty;
    out->nup = auto_nup;
    out->nok = auto_nok;
    out->apex = auto_apex;
    out->topx = auto_topx;
    out->topy = auto_topy;
#else
    out->want = -1.0f;
    out->want_y = -9999.0f;
    out->nup = 0;
    out->nok = 0;
    out->apex = 0.0f;
    out->topx = out->topy = -9999.0f;
#endif
}

#ifdef TELEMETRY
/* Microseconds spent drawing each stage during the last game_draw(). */
const float *game_profile(void) { return prof_us; }
#endif
