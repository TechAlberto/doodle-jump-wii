/* game.h - gameplay for the Doodle Jump style platformer. */
#ifndef GAME_H
#define GAME_H

#include "input.h"

void game_init(void);
void game_update(const Input *in, float dt);
void game_draw(void);

int  game_score(void);
int  game_best(void);

/* Snapshot of internal state, used by the telemetry build to prove from a
 * screenshot that the simulation is doing the right thing. */
typedef struct {
    int   state;        /* 0 title, 1 play, 2 pause, 3 dead */
    int   score;
    int   best;
    int   platforms;
    int   monsters;
    float y;            /* doodler world y */
    float cam;          /* camera world y */
    float max_height;
    float vy;
    float x;            /* doodler world x */
    float vx;
    float want;         /* autopilot steering target, -1 = none */
    float want_y;       /* autopilot target surface, -9999 = none */
    int   nup;          /* surfaces above the feet */
    int   nok;          /* surfaces reachable horizontally */
    float apex;         /* feet height at the top of the bounce */
    float topx;         /* highest platform above the feet */
    float topy;
} GameDebug;

void game_debug(GameDebug *out);

#ifdef TELEMETRY
/* Per-stage draw cost in microseconds: bg, platforms, actors, doodler, hud,
 * overlay.  Only available in telemetry builds. */
const float *game_profile(void);
#endif

#endif
