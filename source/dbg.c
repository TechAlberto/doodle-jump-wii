/* dbg.c - binary telemetry strip, see dbg.h. */
#include "dbg.h"
#include "gfx.h"

/* Cell layout, in game pixels.  tools/decode_telemetry.py must agree. */
#define CELL_W 6
#define CELL_H 6
#define GAP_X  1
#define GAP_Y  4
#define X0     6
#define Y0     6

void dbg_draw_values(const u32 *values, int count)
{
    if (count > DBG_ROWS) count = DBG_ROWS;
    if (count <= 0) return;

    float w = 32 * (CELL_W + GAP_X) + 8;
    float h = count * (CELL_H + GAP_Y) + 4;
    gfx_fill_rect(X0 - 4.0f, Y0 - 4.0f, w, h, gfx_rgb(0, 0, 0));

    for (int r = 0; r < count; r++) {
        for (int b = 0; b < 32; b++) {
            if (!(values[r] & (1u << b))) continue;
            gfx_fill_rect(X0 + b * (CELL_W + GAP_X), Y0 + r * (CELL_H + GAP_Y),
                          CELL_W, CELL_H, gfx_rgb(255, 255, 255));
        }
    }
}
