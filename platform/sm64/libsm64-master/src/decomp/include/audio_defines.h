#ifndef AUDIO_DEFINES_H
#define AUDIO_DEFINES_H
// libsm64: the sound words are sounds.h's (the decompilation renamed this file); these are the few older names the
// audio engine here still uses.
#include "sounds.h"
#define SOUND_STATUS_STARTING SOUND_STATUS_WAITING
#define SOUND_LO_BITFLAG_UNK1 SOUND_LOWER_BACKGROUND_MUSIC
#define SOUND_LO_BITFLAG_UNK8 SOUND_DISCRETE
#define SOUND_NO_FREQUENCY_LOSS SOUND_CONSTANT_FREQUENCY
#define SOUND_OBJ_WHOMP_LOWPRIO SOUND_ARG_LOAD(SOUND_BANK_OBJ, 0x16, 0x60, SOUND_DISCRETE)
#define SOUND_MENU_PAUSE_HIGHPRIO SOUND_ARG_LOAD(SOUND_BANK_MENU, 0x02, 0xFF, SOUND_DISCRETE)
#define NO_SOUND 0
#endif
