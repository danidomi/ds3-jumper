#include "audio.h"

#include <SDL.h>
#include <math.h>
#include <string.h>

#define RATE 44100
#define SFX_VOICES 16
#define MUSIC_VOICES 8

typedef enum { W_SQUARE, W_TRI, W_SAW, W_NOISE } Wave;

typedef struct {
    bool on;
    Wave wave;
    float f0, f1;  /* frequency slides from f0 to f1 */
    float dur, t;  /* seconds */
    float delay;   /* seconds before it starts */
    float vol, duty, phase, noise;
    Uint32 lfsr;
} Voice;

static SDL_AudioDeviceID dev;
static Voice sfx[SFX_VOICES], mus[MUSIC_VOICES];
static bool sfxOn = true, musicOn = true, musicPlaying;
static float master = 0.7f;
static long musicStep, stepSampleCount;

static float mtof(int n) { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

/* --- Original soundtrack: 4 bars of 16th notes in C major, 140 BPM --- */
#define BPM 140
static const unsigned char LEAD[64] = {
    72, 0, 76, 0, 79, 0, 76, 0, 77, 0, 76, 0, 74, 0, 72, 0,
    74, 0, 77, 0, 81, 0, 77, 0, 79, 0, 77, 0, 76, 0, 74, 0,
    72, 0, 76, 0, 79, 0, 84, 0, 83, 0, 79, 0, 76, 0, 79, 0,
    77, 0, 76, 0, 74, 0, 71, 0, 72, 0, 0, 0, 67, 0, 71, 0,
};
static const unsigned char BASS[64] = {
    48, 0, 0, 0, 55, 0, 0, 0, 48, 0, 0, 0, 55, 0, 0, 0,
    50, 0, 0, 0, 57, 0, 0, 0, 50, 0, 0, 0, 57, 0, 0, 0,
    48, 0, 0, 0, 55, 0, 0, 0, 52, 0, 0, 0, 55, 0, 0, 0,
    53, 0, 0, 0, 53, 0, 0, 0, 55, 0, 0, 0, 43, 0, 0, 0,
};

static void start(Voice *pool, int n, Wave w, float f0, float f1, float dur,
                  float vol, float duty, float delay) {
    Voice *v = &pool[0];
    for (int i = 0; i < n; i++) {           /* free voice, else the oldest */
        if (!pool[i].on) { v = &pool[i]; break; }
        if (pool[i].t / pool[i].dur > v->t / v->dur) v = &pool[i];
    }
    memset(v, 0, sizeof *v);
    v->on = true; v->wave = w; v->f0 = f0; v->f1 = f1; v->dur = dur;
    v->vol = vol; v->duty = duty; v->delay = delay; v->lfsr = 0xACE1u;
}

static float voiceSample(Voice *v) {
    const float dt = 1.0f / RATE;
    if (v->delay > 0) { v->delay -= dt; return 0; }
    float p = v->t / v->dur;
    if (p >= 1) { v->on = false; return 0; }
    float f = v->f0 + (v->f1 - v->f0) * p;
    v->phase += f * dt;
    float s;
    if (v->phase >= 1) {
        v->phase -= floorf(v->phase);
        Uint32 bit = ((v->lfsr >> 0) ^ (v->lfsr >> 2) ^ (v->lfsr >> 3) ^ (v->lfsr >> 5)) & 1;
        v->lfsr = (v->lfsr >> 1) | (bit << 15);
        v->noise = (v->lfsr & 1) ? 1.0f : -1.0f;
    }
    switch (v->wave) {
    case W_SQUARE: s = v->phase < v->duty ? 1 : -1; break;
    case W_TRI: s = 4 * fabsf(v->phase - 0.5f) - 1; break;
    case W_SAW: s = 2 * v->phase - 1; break;
    default: s = v->noise; break;
    }
    float env = (v->t < 0.004f ? v->t / 0.004f : 1) * (1 - p);
    v->t += dt;
    return s * env * v->vol;
}

static void musicTick(void) {
    int i = (int)(musicStep % 64);
    if (LEAD[i]) start(mus, MUSIC_VOICES, W_SQUARE, mtof(LEAD[i]), mtof(LEAD[i]), 0.17f, 0.10f, 0.25f, 0);
    if (BASS[i]) start(mus, MUSIC_VOICES, W_TRI, mtof(BASS[i]), mtof(BASS[i]), 0.40f, 0.26f, 0.5f, 0);
    if (i % 4 == 2) start(mus, MUSIC_VOICES, W_NOISE, 9000, 9000, 0.035f, 0.035f, 0.5f, 0);
    if (i % 8 == 0) start(mus, MUSIC_VOICES, W_TRI, 140, 50, 0.09f, 0.22f, 0.5f, 0); /* kick */
    musicStep++;
}

static void callback(void *ud, Uint8 *stream, int len) {
    (void)ud;
    Sint16 *out = (Sint16 *)stream;
    int n = len / 2;
    const long stepLen = RATE * 60 / BPM / 4;
    for (int i = 0; i < n; i++) {
        float mix = 0;
        if (musicPlaying && musicOn) {
            if (stepSampleCount-- <= 0) { stepSampleCount = stepLen; musicTick(); }
            for (int k = 0; k < MUSIC_VOICES; k++) if (mus[k].on) mix += voiceSample(&mus[k]);
        }
        if (sfxOn)
            for (int k = 0; k < SFX_VOICES; k++) if (sfx[k].on) mix += voiceSample(&sfx[k]);
        mix *= master;
        if (mix > 1) mix = 1;
        if (mix < -1) mix = -1;
        out[i] = (Sint16)(mix * 30000);
    }
}

bool audio_init(void) {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) return false;
    SDL_AudioSpec want = { 0 }, got;
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 512;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &got, 0);
    if (!dev) return false;
    SDL_PauseAudioDevice(dev, 0);
    return true;
}

void audio_quit(void) {
    if (dev) SDL_CloseAudioDevice(dev);
    dev = 0;
}

void audio_config(bool s, bool m, int volume) {
    if (!dev) return;
    SDL_LockAudioDevice(dev);
    sfxOn = s; musicOn = m;
    master = (volume < 0 ? 0 : volume > 10 ? 10 : volume) / 10.0f;
    if (!s) memset(sfx, 0, sizeof sfx);
    if (!m) memset(mus, 0, sizeof mus);
    SDL_UnlockAudioDevice(dev);
}

void audio_music(bool playing) {
    if (!dev || playing == musicPlaying) return;
    SDL_LockAudioDevice(dev);
    musicPlaying = playing;
    if (playing) { musicStep = 0; stepSampleCount = 0; }
    else memset(mus, 0, sizeof mus);
    SDL_UnlockAudioDevice(dev);
}

void audio_play(Sfx s) {
    if (!dev || !sfxOn) return;
    SDL_LockAudioDevice(dev);
#define V(w, f0, f1, d, vol, duty, delay) start(sfx, SFX_VOICES, w, f0, f1, d, vol, duty, delay)
    switch (s) {
    case SFX_JUMP: V(W_SQUARE, 280, 720, 0.14f, 0.30f, 0.5f, 0); break;
    case SFX_LAND: V(W_NOISE, 1200, 300, 0.06f, 0.10f, 0.5f, 0); break;
    case SFX_COIN:
        V(W_SQUARE, 988, 988, 0.08f, 0.26f, 0.5f, 0);
        V(W_SQUARE, 1319, 1319, 0.22f, 0.26f, 0.5f, 0.07f);
        break;
    case SFX_STOMP:
        V(W_SQUARE, 620, 140, 0.14f, 0.22f, 0.5f, 0);
        V(W_NOISE, 3000, 600, 0.08f, 0.14f, 0.5f, 0);
        break;
    case SFX_HURT:
        V(W_SAW, 420, 70, 0.35f, 0.25f, 0.5f, 0);
        V(W_NOISE, 4000, 300, 0.25f, 0.18f, 0.5f, 0);
        break;
    case SFX_FALL: V(W_TRI, 900, 90, 0.7f, 0.35f, 0.5f, 0); break;
    case SFX_GAMEOVER: {
        const int notes[] = { 67, 63, 60, 55 };
        for (int i = 0; i < 4; i++)
            V(W_SQUARE, mtof(notes[i]), mtof(notes[i]), i == 3 ? 0.6f : 0.2f, 0.18f, 0.5f, i * 0.2f);
        break;
    }
    case SFX_START: {
        const int notes[] = { 60, 64, 67, 72 };
        for (int i = 0; i < 4; i++)
            V(W_SQUARE, mtof(notes[i]), mtof(notes[i]), 0.12f, 0.16f, 0.25f, i * 0.07f);
        break;
    }
    case SFX_MENU_MOVE: V(W_SQUARE, 880, 880, 0.04f, 0.10f, 0.5f, 0); break;
    case SFX_MENU_OK:
        V(W_SQUARE, 660, 660, 0.05f, 0.14f, 0.5f, 0);
        V(W_SQUARE, 990, 990, 0.09f, 0.14f, 0.5f, 0.05f);
        break;
    case SFX_MENU_BACK: V(W_SQUARE, 660, 400, 0.09f, 0.13f, 0.5f, 0); break;
    case SFX_TOGGLE: V(W_SQUARE, 1250, 1250, 0.035f, 0.10f, 0.25f, 0); break;
    }
#undef V
    SDL_UnlockAudioDevice(dev);
}
