#ifndef IOQUAKE3_XBOX_NV2A_H
#define IOQUAKE3_XBOX_NV2A_H

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"

/* Handle 0 is an unresolved shader and is never drawn. */
#define XBOX_NV2A_WHITE_TEXTURE 1
#define XBOX_NV2A_MAX_TEXTURE_SIZE 1024

qboolean XboxNV2A_Init(void);
void XboxNV2A_Shutdown(qboolean destroyWindow);
void XboxNV2A_Kill(void);
void XboxNV2A_ShowDebugScreen(void);
qboolean XboxNV2A_OwnsScreen(void);
void XboxNV2A_BeginRegistration(glconfig_t *config);
void XboxNV2A_BeginFrame(stereoFrame_t stereoFrame);
void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec);
void XboxNV2A_SetColor(const float *rgba);
qhandle_t XboxNV2A_RegisterTexture(const char *name, int width, int height,
	const byte *rgba);
void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader);

#endif
