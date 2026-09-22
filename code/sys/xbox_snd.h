/* Native Original Xbox AC97 PCM service. */
#ifndef IOQUAKE3_XBOX_SND_H
#define IOQUAKE3_XBOX_SND_H

#include "../qcommon/q_shared.h"

typedef void (*xboxSoundFill_t)(int16_t *samples, int sampleCount, void *userData);

qboolean Sys_XboxSoundInit(xboxSoundFill_t fill, void *userData);
void Sys_XboxSoundShutdown(void);
void Sys_XboxSoundSetPaused(qboolean paused);

#endif
