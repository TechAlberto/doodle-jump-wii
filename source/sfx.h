/* sfx.h - tiny procedural chiptune engine (ASND) */
#ifndef SFX_H
#define SFX_H

#include <stdbool.h>

void sfx_init(void);
void sfx_shutdown(void);

void sfx_jump(void);
void sfx_spring(void);
void sfx_shoot(void);
void sfx_monster(void);
void sfx_break(void);
void sfx_powerup(void);
void sfx_death(void);

void sfx_set_music(bool on);
bool sfx_music_on(void);

#endif
