/* input.h - Wiimote (+nunchuk) mapped to a simple analog stick + buttons */
#ifndef INPUT_H
#define INPUT_H

#include <stdbool.h>

typedef struct {
    float axis;      /* -1..1 horizontal */
    bool  shoot;     /* pressed this frame */
    bool  start;     /* pressed this frame */
    bool  pause;
    bool  back;
    bool  connected;
    int   source;    /* 0 none, 1 tilt, 2 nunchuk, 3 d-pad */
} Input;

void input_init(void);
void input_poll(Input *in);

#endif
