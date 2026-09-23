/* Native Original Xbox AC97 PCM service. */
#include "xbox_snd.h"

#include <string.h>

#include <hal/audio.h>
#include <xboxkrnl/xboxkrnl.h>

#define XBOX_SOUND_SAMPLES 2048
#define XBOX_SOUND_BUFFERS 3

static xboxSoundFill_t xboxSoundFill;
static void *xboxSoundUserData;
static qboolean xboxSoundStarted;
static int16_t *xboxSoundMemory;
static int xboxSoundNext;

static void Sys_XboxSoundQueue(void)
{
	int16_t *buffer = xboxSoundMemory + xboxSoundNext * XBOX_SOUND_SAMPLES;

	if (xboxSoundFill)
		xboxSoundFill(buffer, XBOX_SOUND_SAMPLES / 2, xboxSoundUserData);
	else
		memset(buffer, 0, sizeof(int16_t) * XBOX_SOUND_SAMPLES);
	XAudioProvideSamples((unsigned char *)buffer,
		sizeof(int16_t) * XBOX_SOUND_SAMPLES, 0);
	xboxSoundNext = (xboxSoundNext + 1) % XBOX_SOUND_BUFFERS;
}

/* Runs as a DPC once per consumed buffer; buffers complete in queue order. */
static void Sys_XboxSoundCallback(void *device, void *data)
{
	(void)device;
	(void)data;
	Sys_XboxSoundQueue();
}

qboolean Sys_XboxSoundInit(xboxSoundFill_t fill, void *userData)
{
	int i;

	if (xboxSoundStarted)
		return qtrue;
	xboxSoundMemory = (int16_t *)MmAllocateContiguousMemoryEx(
		sizeof(int16_t) * XBOX_SOUND_SAMPLES * XBOX_SOUND_BUFFERS, 0,
		64 * 1024 * 1024, 0, PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!xboxSoundMemory)
		return qfalse;
	memset(xboxSoundMemory, 0,
		sizeof(int16_t) * XBOX_SOUND_SAMPLES * XBOX_SOUND_BUFFERS);
	xboxSoundUserData = userData;
	xboxSoundNext = 0;
	XAudioInit(16, 2, Sys_XboxSoundCallback, NULL);
	for (i = 0; i < XBOX_SOUND_BUFFERS; ++i)
		Sys_XboxSoundQueue();
	xboxSoundFill = fill;
	XAudioPlay();
	xboxSoundStarted = qtrue;
	return qtrue;
}

void Sys_XboxSoundShutdown(void)
{
	if (!xboxSoundStarted)
		return;
	XAudioPause();
	MmFreeContiguousMemory(xboxSoundMemory);
	xboxSoundMemory = NULL;
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
