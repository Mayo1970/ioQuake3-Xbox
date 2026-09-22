/* ioquake3 base mixer -> native Xbox AC97 PCM bridge. */
#include "xbox_snd.h"

#include "../client/snd_local.h"

#include <stdlib.h>
#include <string.h>

#define XBOX_DMA_SAMPLES 32768

static volatile int xboxDmaPos;
static qboolean xboxDmaStarted;

static void XboxSoundFill(int16_t *samples, int sampleFrames, void *userData)
{
	int i;
	int source;
	(void)userData;

	if (!xboxDmaStarted || !dma.buffer)
	{
		memset(samples, 0, sampleFrames * 2 * sizeof(*samples));
		return;
	}

	source = xboxDmaPos;
	for (i = 0; i < sampleFrames * 2; ++i)
	{
		samples[i] = ((int16_t *)dma.buffer)[source];
		if (++source >= dma.samples)
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
	dma.speed = 48000;
	dma.buffer = (byte *)calloc((size_t)dma.samples, dma.samplebits / 8);
	if (!dma.buffer)
		return qfalse;

	memset(dma.buffer, 0, (size_t)dma.samples * dma.samplebits / 8);
	xboxDmaPos = 0;
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
