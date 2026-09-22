/* Native Original Xbox AC97 PCM service. */
#include "xbox_snd.h"

#include <string.h>

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#define XBOX_SOUND_SAMPLES 2048

static xboxSoundFill_t xboxSoundFill;
static void *xboxSoundUserData;
static qboolean xboxSoundStarted;
static int16_t *xboxSoundBuffer;

static void Sys_XboxSoundCallback(void *device, void *data)
{
	(void)device;
	if (xboxSoundFill)
		xboxSoundFill(xboxSoundBuffer, XBOX_SOUND_SAMPLES / 2, xboxSoundUserData);
	else
		memset(xboxSoundBuffer, 0, sizeof(int16_t) * XBOX_SOUND_SAMPLES);
	XAudioProvideSamples((unsigned char *)xboxSoundBuffer,
		sizeof(int16_t) * XBOX_SOUND_SAMPLES, 0);
}

qboolean Sys_XboxSoundInit(xboxSoundFill_t fill, void *userData)
{
	if (xboxSoundStarted)
		return qtrue;
	xboxSoundBuffer = (int16_t *)MmAllocateContiguousMemoryEx(
		sizeof(int16_t) * XBOX_SOUND_SAMPLES, 0, 64 * 1024 * 1024, 0,
		PAGE_READWRITE);
	if (!xboxSoundBuffer)
		return qfalse;
	xboxSoundFill = fill;
	xboxSoundUserData = userData;
	XAudioInit(16, 2, Sys_XboxSoundCallback, NULL);
	XAudioProvideSamples((unsigned char *)xboxSoundBuffer,
		sizeof(int16_t) * XBOX_SOUND_SAMPLES, 0);
	XAudioPlay();
	xboxSoundStarted = qtrue;
	return qtrue;
}

void Sys_XboxSoundShutdown(void)
{
	if (!xboxSoundStarted)
		return;
	XAudioPause();
	MmFreeContiguousMemory(xboxSoundBuffer);
	xboxSoundBuffer = NULL;
	xboxSoundFill = NULL;
	xboxSoundUserData = NULL;
	xboxSoundStarted = qfalse;
}

void Sys_XboxSoundSetPaused(qboolean paused)
{
	if (!xboxSoundStarted)
		return;
	if (paused)
		XAudioPause();
	else
		XAudioPlay();
}
