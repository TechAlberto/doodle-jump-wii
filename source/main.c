/* main.c - boot, input/render loop.
 *
 * The renderer and the game both use a fixed 640x480 layout; gfx_present()
 * waits for vblank so the loop is paced by the display.  dt is still measured
 * from the hardware timer so 50 Hz PAL modes (and Dolphin's frame limiter)
 * keep the physics at the right speed.
 */
#include <gccore.h>
#include <ogc/lwp_watchdog.h>
#include <stdio.h>

#include "gfx.h"
#include "input.h"
#include "sfx.h"
#include "game.h"
#include "dbg.h"

#ifdef CALIBRATE
/* Overlay used to calibrate the emulator's video path: exact edges, 100 px
 * ruler ticks, a 100x100 square and a 50 px circle.  With a pixel-exact
 * capture the square must stay square and the edge lines must be visible. */
static void draw_calibration(void)
{
    GfxColor red   = gfx_rgb(255, 0, 0);
    GfxColor green = gfx_rgb(0, 200, 0);
    GfxColor blue  = gfx_rgb(0, 0, 255);
    GfxColor black = gfx_rgb(0, 0, 0);

    gfx_fill_rect(0, 0, 640, 2, red);              /* top edge */
    gfx_fill_rect(0, 478, 640, 2, blue);           /* bottom edge */
    gfx_fill_rect(0, 0, 2, 480, green);            /* left edge */
    gfx_fill_rect(638, 0, 2, 480, red);            /* right edge */

    for (int y = 0; y < 480; y += 100) {           /* horizontal ruler */
        gfx_fill_rect(0, (float)y, 24, 2, black);
        char n[8];
        snprintf(n, sizeof(n), "%d", y);
        gfx_text(n, 28.0f, (float)y - 6.0f, 14.0f, black);
    }
    for (int x = 0; x < 640; x += 100) {           /* vertical ruler */
        gfx_fill_rect((float)x, 0, 2, 24, black);
    }

    gfx_fill_rect(200, 150, 100, 100, black);      /* 100x100 square */
    gfx_circle(450, 200, 50, black);               /* r=50 circle */
    gfx_fill_rect(300, 340, 200, 20, black);       /* 200x20 bar */
}
#endif

int main(void)
{
    gfx_init();
    input_init();
    sfx_init();
    game_init();

    Input in;
    u32 last = gettick();

    for (;;) {
        input_poll(&in);

        u32 now = gettick();
        float dt = (float)ticks_to_microsecs(now - last) / 1000000.0f;
        last = now;
        if (dt < 0.001f) dt = 0.001f;
        if (dt > 0.05f) dt = 0.05f;

        u32 t0 = gettick();
        game_update(&in, dt);
        u32 t1 = gettick();
        game_draw();
#ifdef CALIBRATE
        draw_calibration();
#endif
        u32 t2 = gettick();
        float update_ms = (float)ticks_to_microsecs(t1 - t0) / 1000.0f;
        float draw_ms = (float)ticks_to_microsecs(t2 - t1) / 1000.0f;

#ifdef TELEMETRY
        {
            GameDebug d;
            u32 vals[DBG_ROWS];
            game_debug(&d);
            vals[0] = (u32)d.state;
            vals[1] = (u32)d.score;
            vals[2] = (u32)d.best;
            vals[3] = (u32)(int)d.y + 10000u;
            vals[4] = (u32)(int)d.cam + 10000u;
            vals[5] = (u32)(int)d.max_height;
            vals[6] = (u32)(int)d.vy + 4000u;
            vals[7] = (u32)d.platforms;
            vals[8] = (u32)d.monsters;
            vals[9] = gfx_frame_count();
            vals[10] = (u32)ticks_to_microsecs(t1 - t0);
            vals[11] = (u32)ticks_to_microsecs(t2 - t1);
            vals[12] = gfx_present_cpu_us();
            {
                const float *p = game_profile();
                for (int i = 0; i < 6; i++) vals[13 + i] = (u32)p[i];
            }
            {
                const u32 *n = gfx_counts();
                for (int i = 0; i < 5; i++) vals[19 + i] = n[i];
            }
            {
                const u32 *t = gfx_ticks();
                for (int i = 0; i < 5; i++)
                    vals[24 + i] = (u32)ticks_to_microsecs(t[i]);
            }
            vals[29] = (u32)(int)(d.x + 10000.0f);
            vals[30] = (u32)(int)(d.vx + 4000.0f);
            vals[31] = (u32)(int)(d.want + 10000.0f);
            vals[32] = (u32)(int)(d.want_y + 10000.0f);
            vals[33] = (u32)d.nup;
            vals[34] = (u32)d.nok;
            vals[35] = (u32)(int)(d.apex + 10000.0f);
            vals[36] = (u32)(int)(d.topx + 10000.0f);
            vals[37] = (u32)(int)(d.topy + 10000.0f);
            dbg_draw_values(vals, DBG_ROWS);
        }
#endif

#ifdef PERF_OVERLAY
        {
            static float shown_ms = 0.0f;
            char buf[48];
            snprintf(buf, sizeof(buf), "%5.2f ms cpu   %d fps", shown_ms,
                     (int)(1.0f / dt + 0.5f));
            gfx_text(buf, 280.0f, 462.0f, 12.0f,
                     (GfxColor){ 120, 132, 144, 255 });
            shown_ms = update_ms + draw_ms;
        }
#else
        (void)update_ms;
        (void)draw_ms;
#endif

        gfx_present();
    }

    return 0;
}
