/* ioquake3 base mixer -> native Xbox AC97 PCM bridge. */
#include "xbox_snd.h"

#include "../client/snd_local.h"

#include <stdlib.h>
#include <string.h>

#define XBOX_DMA_SAMPLES 32768
/* The mixer runs at half the AC97's fixed 48 kHz to save CPU; each frame plays twice. */
#define XBOX_DMA_OUTPUT_RATE 48000
#define XBOX_DMA_UPSAMPLE 2

static volatile int xboxDmaPos;
static int xboxDmaRepeat;
static qboolean xboxDmaStarted;

static void XboxSoundFill(int16_t *samples, int sampleFrames, void *userData)
{
	const int16_t *buffer = (const int16_t *)dma.buffer;
	int i;
	int source;
	(void)userData;

	if (!xboxDmaStarted || !dma.buffer)
	{
		memset(samples, 0, sampleFrames * 2 * sizeof(*samples));
		return;
	}

	source = xboxDmaPos;
	for (i = 0; i < sampleFrames; ++i)
	{
		samples[i * 2] = buffer[source];
		samples[i * 2 + 1] = buffer[source + 1];
		if (++xboxDmaRepeat < XBOX_DMA_UPSAMPLE)
			continue;
		xboxDmaRepeat = 0;
		source += 2;
		if (source >= dma.samples)
			source = 0;
	}
	xboxDmaPos = source;
}

qboolean SNDDMA_Init(void)
{
	if (xboxDmaStarted)
		return qtrue;

	dma.samplebits = 16;
	dma.isfloat = qfalse;
	dma.channels = 2;
	dma.samples = XBOX_DMA_SAMPLES;
	dma.fullsamples = dma.samples / dma.channels;
	dma.submission_chunk = 1024;
	dma.speed = XBOX_DMA_OUTPUT_RATE / XBOX_DMA_UPSAMPLE;
	dma.buffer = (byte *)calloc((size_t)dma.samples, dma.samplebits / 8);
	if (!dma.buffer)
		return qfalse;

	memset(dma.buffer, 0, (size_t)dma.samples * dma.samplebits / 8);
	xboxDmaPos = 0;
	xboxDmaRepeat = 0;
	if (!Sys_XboxSoundInit(XboxSoundFill, NULL))
	{
		free(dma.buffer);
		dma.buffer = NULL;
		return qfalse;
	}
	xboxDmaStarted = qtrue;
	return qtrue;
}

int SNDDMA_GetDMAPos(void)
{
	return xboxDmaPos;
}

void SNDDMA_Shutdown(void)
{
	if (!xboxDmaStarted)
		return;
	Sys_XboxSoundShutdown();
	free(dma.buffer);
	dma.buffer = NULL;
	xboxDmaStarted = qfalse;
}

void SNDDMA_BeginPainting(void) {}
void SNDDMA_Submit(void) {}
