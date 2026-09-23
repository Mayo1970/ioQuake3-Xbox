#ifndef IOQUAKE3_XBOX_NV2A_H
#define IOQUAKE3_XBOX_NV2A_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../renderercommon/tr_public.h"

/* Index 0 of both tables is "unresolved" and is never drawn. */
#define XBOX_NV2A_WHITE_IMAGE 1
#define XBOX_NV2A_WHITE_SHADER 1
#define XBOX_NV2A_MAX_TEXTURE_SIZE 1024
#define XBOX_NV2A_MAX_STAGES 8
#define XBOX_NV2A_MAX_TEXMODS 4
#define XBOX_NV2A_MAX_ANIM_IMAGES 8
/* Same per-surface limits as ioq3's tess (SHADER_MAX_VERTEXES/INDEXES). */
#define XBOX_NV2A_TESS_VERTS 1000
#define XBOX_NV2A_TESS_INDEXES (6 * XBOX_NV2A_TESS_VERTS)

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

/* ioq3 shaderSort_t values. */
#define XBOX_NV2A_SORT_PORTAL 1.0f
#define XBOX_NV2A_SORT_ENVIRONMENT 2.0f
#define XBOX_NV2A_SORT_OPAQUE 3.0f
#define XBOX_NV2A_SORT_DECAL 4.0f
#define XBOX_NV2A_SORT_SEE_THROUGH 5.0f
#define XBOX_NV2A_SORT_BANNER 6.0f
#define XBOX_NV2A_SORT_UNDERWATER 8.0f
#define XBOX_NV2A_SORT_BLEND0 9.0f
#define XBOX_NV2A_SORT_BLEND1 10.0f
#define XBOX_NV2A_SORT_NEAREST 16.0f

typedef enum {
	XBOX_NV2A_ALPHA_NONE,
	XBOX_NV2A_ALPHA_GT0,
	XBOX_NV2A_ALPHA_LT128,
	XBOX_NV2A_ALPHA_GE128
} xboxNV2AAlphaFunc_t;

typedef enum {
	XBOX_NV2A_WAVE_SIN,
	XBOX_NV2A_WAVE_TRIANGLE,
	XBOX_NV2A_WAVE_SQUARE,
	XBOX_NV2A_WAVE_SAWTOOTH,
	XBOX_NV2A_WAVE_INVERSE_SAWTOOTH,
	XBOX_NV2A_WAVE_NOISE
} xboxNV2AWaveFunc_t;

typedef struct {
	int func;
	float base;
	float amplitude;
	float phase;
	float frequency;
} xboxNV2AWave_t;

typedef enum {
	XBOX_NV2A_TCMOD_SCALE,
	XBOX_NV2A_TCMOD_SCROLL,
	XBOX_NV2A_TCMOD_TURB,
	XBOX_NV2A_TCMOD_ROTATE,
	XBOX_NV2A_TCMOD_STRETCH,
	XBOX_NV2A_TCMOD_TRANSFORM,
	XBOX_NV2A_TCMOD_ENTITY_TRANSLATE
} xboxNV2ATexModType_t;

typedef struct {
	int type;
	xboxNV2AWave_t wave;
	float matrix[2][2];
	float translate[2];
	float scale[2];
	float scroll[2];
	float rotateSpeed;
} xboxNV2ATexMod_t;

typedef enum {
	XBOX_NV2A_RGBGEN_IDENTITY_LIGHTING,
	XBOX_NV2A_RGBGEN_IDENTITY,
	XBOX_NV2A_RGBGEN_VERTEX,
	XBOX_NV2A_RGBGEN_EXACT_VERTEX,
	XBOX_NV2A_RGBGEN_ONE_MINUS_VERTEX,
	XBOX_NV2A_RGBGEN_WAVE,
	XBOX_NV2A_RGBGEN_CONST,
	XBOX_NV2A_RGBGEN_ENTITY,
	XBOX_NV2A_RGBGEN_ONE_MINUS_ENTITY,
	XBOX_NV2A_RGBGEN_LIGHTING_DIFFUSE
} xboxNV2ARgbGen_t;

typedef enum {
	XBOX_NV2A_ALPHAGEN_IDENTITY,
	XBOX_NV2A_ALPHAGEN_VERTEX,
	XBOX_NV2A_ALPHAGEN_ONE_MINUS_VERTEX,
	XBOX_NV2A_ALPHAGEN_WAVE,
	XBOX_NV2A_ALPHAGEN_CONST,
	XBOX_NV2A_ALPHAGEN_ENTITY,
	XBOX_NV2A_ALPHAGEN_ONE_MINUS_ENTITY
} xboxNV2AAlphaGen_t;

typedef enum {
	XBOX_NV2A_TCGEN_TEXTURE,
	XBOX_NV2A_TCGEN_ENVIRONMENT,
	XBOX_NV2A_TCGEN_VECTOR
} xboxNV2ATcGen_t;

typedef enum {
	XBOX_NV2A_CULL_FRONT,
	XBOX_NV2A_CULL_BACK,
	XBOX_NV2A_CULL_NONE
} xboxNV2ACull_t;

/* 2D pics and model surfaces get different implicit shaders, as in ioq3. */
typedef enum {
	XBOX_NV2A_SHADER_2D,
	XBOX_NV2A_SHADER_MODEL
} xboxNV2AShaderFlavor_t;

/* One ioq3 shader stage; blend factors hold GL values and GL_ONE/GL_ZERO is opaque. */
typedef struct {
	int images[XBOX_NV2A_MAX_ANIM_IMAGES];
	int numImages;
	float animFrequency;
	qboolean clamp;
	unsigned int srcBlend;
	unsigned int dstBlend;
	int alphaFunc;
	qboolean depthWrite;
	qboolean depthEqual;
	int rgbGen;
	xboxNV2AWave_t rgbWave;
	int alphaGen;
	xboxNV2AWave_t alphaWave;
	byte constant[4];
	int tcGen;
	vec3_t tcGenVectors[2];
	int numTexMods;
	xboxNV2ATexMod_t texMods[XBOX_NV2A_MAX_TEXMODS];
} xboxNV2AStage_t;

typedef struct {
	int numStages;
	float sort;
	int cull;
	xboxNV2AStage_t stages[XBOX_NV2A_MAX_STAGES];
} xboxNV2AShaderDef_t;

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
qboolean XboxNV2A_FindShader(const char *name, int flavor, qhandle_t *handle);
qhandle_t XboxNV2A_CreateShader(const char *name, int flavor,
	const xboxNV2AShaderDef_t *def);
void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader);
void XboxNV2A_ClearScene(void);
void XboxNV2A_AddRefEntity(const refEntity_t *entity);
void XboxNV2A_RenderScene(const refdef_t *fd);

/* Shader scripts and image files, in xbox_nv2a_shader.c. */
qhandle_t XboxNV2AShader_Register(const char *name, qboolean mipRawImage);
qhandle_t XboxNV2AShader_RegisterModel(const char *name);
void XboxNV2AShader_FreeScripts(void);

/* MD3 models, in xbox_nv2a_model.c. */
qhandle_t XboxNV2AModel_Register(const char *name);
void XboxNV2AModel_FreeAll(void);
const md3Header_t *XboxNV2AModel_Get(qhandle_t handle);
void XboxNV2AModel_LerpSurface(const md3Surface_t *surface, int frame,
	int oldFrame, float backlerp, float (*xyz)[3], float (*normal)[3]);
void XboxNV2AModel_Bounds(qhandle_t handle, vec3_t mins, vec3_t maxs);
int XboxNV2AModel_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame,
	int endFrame, float frac, const char *tagName);

#endif
