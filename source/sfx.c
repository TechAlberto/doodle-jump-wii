/* sfx.c - all audio in this port is synthesised at boot: no sample files, so
 * the port ships nothing but code.  Everything is mono 16-bit PCM at 32 kHz;
 * PPC is big-endian, which is the format ASND wants (VOICE_MONO_16BIT). */
#include "sfx.h"

#include <asndlib.h>
#include <gccore.h>
#include <malloc.h>
#include <math.h>
#include <string.h>

#define SR 32000
#define MUSIC_VOICE 0
#define SFX_VOICES 8

/* ------------------------------------------------------------- oscillators */

static inline float sq(float phase) { return (phase - floorf(phase)) < 0.5f ? 1.0f : -1.0f; }
static inline float tri(float phase)
{
    float p = phase - floorf(phase);
    return p < 0.5f ? (4.0f * p - 1.0f) : (3.0f - 4.0f * p);
}
static inline float saw(float phase)
{
    float p = phase - floorf(phase);
    return 2.0f * p - 1.0f;
}

static u32 noise_state = 0x1234567u;
static inline float noise(void)
{
    noise_state ^= noise_state << 13;
    noise_state ^= noise_state >> 17;
    noise_state ^= noise_state << 5;
    return (float)(s32)noise_state / 2147483648.0f;
}

/* ---------------------------------------------------------------- effects */

typedef struct {
    s16 *data;
    int frames;
} Clip;

enum { S_JUMP, S_SPRING, S_SHOOT, S_MONSTER, S_BREAK, S_POWER, S_DEATH, S_COUNT };

static Clip clips[S_COUNT];
static s16 *music;
static int music_frames;
static bool music_enabled = true;
static int next_voice = 1;

static s16 *clip_alloc(int frames)
{
    s16 *p = memalign(32, frames * sizeof(s16) + 32);
    if (p) memset(p, 0, frames * sizeof(s16));
    return p;
}

/* Simple attack/decay envelope over the whole clip. */
static void apply_env(s16 *buf, int n, float attack, float decay_pow)
{
    int a = (int)(attack * SR);
    for (int i = 0; i < n; i++) {
        float t = (float)i / (float)n;
        float env = 1.0f - powf(t, decay_pow);
        if (i < a) env *= (float)i / (float)a;
        buf[i] = (s16)(buf[i] * env);
    }
}

static void gen_jump(Clip *c)
{
    c->frames = SR * 9 / 100;
    c->data = clip_alloc(c->frames);
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / SR;
        float f = 480.0f + 900.0f * t;           /* rising blip */
        ph += f / SR;
        float v = sq(ph) * 0.35f + tri(ph * 2) * 0.12f;
        c->data[i] = (s16)(v * 24000);
    }
    apply_env(c->data, c->frames, 0.005f, 1.6f);
}

static void gen_spring(Clip *c)
{
    c->frames = SR * 38 / 100;
    c->data = clip_alloc(c->frames);
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / SR;
        /* boing: pitch sweep up with a wobble that fades */
        float f = 170.0f * powf(6.0f, t / 0.38f) * (1.0f + 0.10f * sinf(t * 70.0f));
        ph += f / SR;
        float v = sinf(ph * 6.2831853f) * 0.8f + tri(ph) * 0.2f;
        c->data[i] = (s16)(v * 30000);
    }
    apply_env(c->data, c->frames, 0.004f, 1.3f);
}

static void gen_shoot(Clip *c)
{
    c->frames = SR * 8 / 100;
    c->data = clip_alloc(c->frames);
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / SR;
        float f = 1500.0f * powf(0.18f, t / 0.08f);
        ph += f / SR;
        float v = sq(ph) * 0.35f + noise() * 0.18f;
        c->data[i] = (s16)(v * 26000);
    }
    apply_env(c->data, c->frames, 0.002f, 2.0f);
}

static void gen_monster(Clip *c)
{
    c->frames = SR * 24 / 100;
    c->data = clip_alloc(c->frames);
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / SR;
        float f = 620.0f * powf(0.22f, t / 0.24f);
        ph += f / SR;
        float v = sq(ph) * 0.30f + noise() * 0.30f * (1.0f - t / 0.24f);
        c->data[i] = (s16)(v * 28000);
    }
    apply_env(c->data, c->frames, 0.002f, 1.8f);
}

static void gen_break(Clip *c)
{
    c->frames = SR * 16 / 100;
    c->data = clip_alloc(c->frames);
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / (float)c->frames;
        float v = noise() * (1.0f - t) * 0.55f;
        c->data[i] = (s16)(v * 26000);
    }
    apply_env(c->data, c->frames, 0.001f, 2.2f);
}

static void gen_powerup(Clip *c)
{
    c->frames = SR * 42 / 100;
    c->data = clip_alloc(c->frames);
    /* C5 E5 G5 C6 */
    static const float notes[4] = {523.25f, 659.25f, 783.99f, 1046.50f};
    int step = c->frames / 4;
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        int s = i / step;
        if (s > 3) s = 3;
        ph += notes[s] / SR;
        float v = sq(ph) * 0.30f + tri(ph * 2.0f) * 0.14f;
        c->data[i] = (s16)(v * 26000);
    }
    apply_env(c->data, c->frames, 0.004f, 1.1f);
}

static void gen_death(Clip *c)
{
    c->frames = SR * 70 / 100;
    c->data = clip_alloc(c->frames);
    float ph = 0;
    for (int i = 0; i < c->frames; i++) {
        float t = (float)i / SR;
        float f = 420.0f * powf(0.10f, t / 0.70f);
        ph += f / SR;
        float v = saw(ph) * 0.28f + sq(ph * 0.5f) * 0.18f;
        c->data[i] = (s16)(v * 26000);
    }
    apply_env(c->data, c->frames, 0.004f, 1.0f);
}

/* ------------------------------------------------------------------ music */

/* An original 8-bar bouncy loop: square lead, triangle bass, noise hats. */
#define BPM 138.0f
#define STEP (60.0f / BPM / 2.0f)          /* eighth note seconds */

static const signed char lead_notes[] = {
    /* A minor pentatonic, 8 bars of 8 steps; -1 = rest.  MIDI note numbers. */
    69, -1, 72, 74, 76, -1, 74, 72,
    69, -1, 67, -1, 69, 72, -1, 69,
    64, -1, 67, 69, 72, -1, 69, 67,
    64, -1, 62, -1, 64, 67, -1, 64,
    69, -1, 72, 74, 76, 79, 76, 74,
    72, -1, 69, -1, 72, 69, -1, 67,
    64, 67, 69, 72, 74, 72, 69, 67,
    64, -1, 62, -1, 64, -1, -1, -1,
};

static const signed char bass_notes[] = {
    45, 45, 45, 45, 45, 45, 45, 45,
    45, 45, 45, 45, 45, 45, 45, 45,
    41, 41, 41, 41, 41, 41, 41, 41,
    43, 43, 43, 43, 43, 43, 43, 43,
    45, 45, 45, 45, 45, 45, 45, 45,
    48, 48, 48, 48, 48, 48, 48, 48,
    41, 41, 41, 41, 43, 43, 43, 43,
    45, 45, 45, 45, 45, 45, 45, 45,
};

static float midi_hz(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static void gen_music(void)
{
    int steps = (int)(sizeof(lead_notes) / sizeof(lead_notes[0]));
    int step_frames = (int)(STEP * SR);
    music_frames = steps * step_frames;
    music = clip_alloc(music_frames);
    if (!music) return;

    float lead_ph = 0, bass_ph = 0;
    for (int s = 0; s < steps; s++) {
        int lead = lead_notes[s];
        int bass = bass_notes[s];
        float lf = lead > 0 ? midi_hz(lead) : 0.0f;
        float bf = bass > 0 ? midi_hz(bass) : 0.0f;
        bool hat = (s % 2) == 1;

        for (int i = 0; i < step_frames; i++) {
            int idx = s * step_frames + i;
            float t = (float)i / SR;
            float v = 0.0f;

            if (lf > 0 && t < STEP * 0.85f) {
                lead_ph += lf * (1.0f + 0.004f * sinf(t * 2.0f * 3.14159f * 5.0f)) / SR;
                float e = 1.0f - t / (STEP * 0.85f);
                v += sq(lead_ph) * 0.16f * e;
                v += tri(lead_ph * 2.0f) * 0.05f * e;
            }
            if (bf > 0) {
                bass_ph += bf / SR;
                float e = expf(-t * 9.0f);
                v += tri(bass_ph) * 0.22f * e;
            }
            if (hat && t < 0.03f) {
                float e = 1.0f - t / 0.03f;
                v += noise() * 0.05f * e;
            }
            float fade = 1.0f;
            /* de-click the loop seam */
            if (idx < 64) fade = idx / 64.0f;
            if (idx > music_frames - 64) fade = (music_frames - idx) / 64.0f;
            music[idx] = (s16)(v * 30000.0f * fade);
        }
    }
}

/* ------------------------------------------------------------------- API */

static void play(Clip *c, int vol)
{
    if (!c || !c->data) return;
    for (int i = 0; i < SFX_VOICES; i++) {
        int v = 1 + (next_voice + i) % SFX_VOICES;
        if (ASND_StatusVoice(v) == SND_UNUSED) {
            ASND_SetVoice(v, VOICE_MONO_16BIT, SR, 0, c->data, c->frames * 2, vol, vol, NULL);
            next_voice = (v + 1) % SFX_VOICES;
            return;
        }
    }
}

void sfx_init(void)
{
    ASND_Init();

    gen_jump(&clips[S_JUMP]);
    gen_spring(&clips[S_SPRING]);
    gen_shoot(&clips[S_SHOOT]);
    gen_monster(&clips[S_MONSTER]);
    gen_break(&clips[S_BREAK]);
    gen_powerup(&clips[S_POWER]);
    gen_death(&clips[S_DEATH]);

    gen_music();
    if (music) {
        DCStoreRange(music, music_frames * sizeof(s16));
        ASND_SetInfiniteVoice(MUSIC_VOICE, VOICE_MONO_16BIT, SR, 0,
                              music, music_frames * 2, 85, 85);
    }
    for (int i = 0; i < S_COUNT; i++)
        if (clips[i].data) DCStoreRange(clips[i].data, clips[i].frames * sizeof(s16));

    ASND_Pause(0);
}

void sfx_shutdown(void)
{
    ASND_End();
}

void sfx_jump(void)    { play(&clips[S_JUMP], 150); }
void sfx_spring(void)  { play(&clips[S_SPRING], 190); }
void sfx_shoot(void)   { play(&clips[S_SHOOT], 120); }
void sfx_monster(void) { play(&clips[S_MONSTER], 160); }
void sfx_break(void)   { play(&clips[S_BREAK], 120); }
void sfx_powerup(void) { play(&clips[S_POWER], 170); }
void sfx_death(void)   { play(&clips[S_DEATH], 190); }

void sfx_set_music(bool on)
{
    music_enabled = on;
    ASND_PauseVoice(MUSIC_VOICE, on ? 0 : 1);
}

bool sfx_music_on(void) { return music_enabled; }
