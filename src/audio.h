/* Tiny synthesizer: retro sound effects + a looping chiptune, no assets. */
#ifndef DS3_JUMPER_AUDIO_H
#define DS3_JUMPER_AUDIO_H

#include <stdbool.h>

typedef enum {
    SFX_JUMP,
    SFX_LAND,
    SFX_COIN,
    SFX_STOMP,
    SFX_HURT,
    SFX_FALL,
    SFX_GAMEOVER,
    SFX_START,
    SFX_MENU_MOVE,
    SFX_MENU_OK,
    SFX_MENU_BACK,
    SFX_TOGGLE,
} Sfx;

bool audio_init(void);          /* false if no audio device (game still runs) */
void audio_quit(void);
void audio_play(Sfx s);
void audio_music(bool playing); /* start/stop the soundtrack */
void audio_config(bool sfxOn, bool musicOn, int volume /* 0..10 */);

#endif
