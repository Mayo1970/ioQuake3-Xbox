#ifndef IOQUAKE3_XBOX_NV2A_H
#define IOQUAKE3_XBOX_NV2A_H

#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"

/* Index 0 of both tables is "unresolved" and is never drawn. */
#define XBOX_NV2A_WHITE_IMAGE 1
#define XBOX_NV2A_WHITE_SHADER 1
#define XBOX_NV2A_MAX_TEXTURE_SIZE 1024

typedef enum {
	XBOX_NV2A_ALPHA_NONE,
	XBOX_NV2A_ALPHA_GT0,
	XBOX_NV2A_ALPHA_LT128,
	XBOX_NV2A_ALPHA_GE128
} xboxNV2AAlphaFunc_t;

/* Shader scripts name blend factors with these GL values; xbox_nv2a.c maps them to xgu. */
#define GL_ZERO 0x0000
#define GL_ONE 0x0001
#define GL_SRC_COLOR 0x0300
#define GL_ONE_MINUS_SRC_COLOR 0x0301
#define GL_SRC_ALPHA 0x0302
#define GL_ONE_MINUS_SRC_ALPHA 0x0303
#define GL_DST_ALPHA 0x0304
#define GL_ONE_MINUS_DST_ALPHA 0x0305
#define GL_DST_COLOR 0x0306
#define GL_ONE_MINUS_DST_COLOR 0x0307
#define GL_SRC_ALPHA_SATURATE 0x0308

typedef struct {
	int image;
	unsigned int srcBlend;
	unsigned int dstBlend;
	int alphaFunc;
	qboolean clamp;
	qboolean vertexColor;
	qboolean vertexAlpha;
} xboxNV2AShaderState_t;

qboolean XboxNV2A_Init(void);
void XboxNV2A_Shutdown(qboolean destroyWindow);
void XboxNV2A_Kill(void);
void XboxNV2A_ShowDebugScreen(void);
qboolean XboxNV2A_OwnsScreen(void);
void XboxNV2A_BeginRegistration(glconfig_t *config);
void XboxNV2A_BeginFrame(stereoFrame_t stereoFrame);
void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec);
void XboxNV2A_SetColor(const float *rgba);
int XboxNV2A_FindImage(const char *name);
int XboxNV2A_CreateImage(const char *name, int width, int height,
	const byte *rgba);
qboolean XboxNV2A_FindShader(const char *name, qhandle_t *handle);
qhandle_t XboxNV2A_CreateShader(const char *name,
	const xboxNV2AShaderState_t *state);
void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader);

/* Shader scripts and image files, in xbox_nv2a_shader.c. */
qhandle_t XboxNV2AShader_Register(const char *name, qboolean mipRawImage);
void XboxNV2AShader_FreeScripts(void);

#endif
