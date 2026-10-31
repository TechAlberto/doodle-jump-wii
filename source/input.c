/* input.c - WPAD wrapper.
 *
 * Movement accepts any of the three things a Wii player is likely to reach
 * for: the nunchuk stick (best), tilting the wiimote (like the phone original
 * being tilted) and the d-pad. */
#include "input.h"

#include <wiiuse/wpad.h>
#include <math.h>

void input_init(void)
{
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
}

static float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void input_poll(Input *in)
{
    WPAD_ScanPads();

    u32 held = WPAD_ButtonsHeld(0);
    u32 down = WPAD_ButtonsDown(0);

    in->axis = 0.0f;
    in->source = 0;

    u32 type = 0;
    in->connected = (WPAD_Probe(0, &type) == WPAD_ERR_NONE);

    const WPADData *wm = WPAD_Data(0);
    if (wm && in->connected) {
        if (wm->exp.type == WPAD_EXP_NUNCHUK) {
            float sx = (float)wm->exp.nunchuk.js.pos.x -
                       (float)wm->exp.nunchuk.js.center.x;
            float mag = wm->exp.nunchuk.js.mag;
            if (mag > 0.16f) {
                float v = (mag - 0.16f) / 0.72f;
                in->axis = (sx >= 0.0f ? 1.0f : -1.0f) * clampf(v, 0.0f, 1.0f);
                in->source = 2;
            }
        } else {
            float tilt = (float)wm->accel.x - 128.0f;
            const float dead = 14.0f;
            if (fabsf(tilt) > dead) {
                float v = (fabsf(tilt) - dead) / 62.0f;
                in->axis = (tilt > 0.0f ? 1.0f : -1.0f) * clampf(v, 0.0f, 1.0f);
                in->source = 1;
            }
        }
    }

    if (held & WPAD_BUTTON_LEFT)  { in->axis = -1.0f; in->source = 3; }
    if (held & WPAD_BUTTON_RIGHT) { in->axis =  1.0f; in->source = 3; }

    in->shoot = (down & (WPAD_BUTTON_A | WPAD_BUTTON_2)) != 0;
    in->start = (down & (WPAD_BUTTON_A | WPAD_BUTTON_2 | WPAD_BUTTON_PLUS)) != 0;
    in->pause = (down & (WPAD_BUTTON_PLUS | WPAD_BUTTON_MINUS)) != 0;
    in->back  = (down & (WPAD_BUTTON_B | WPAD_BUTTON_1)) != 0;
}
