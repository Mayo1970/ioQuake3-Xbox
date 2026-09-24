#ifndef IOQUAKE3_XBOX_NV2A_H
#define IOQUAKE3_XBOX_NV2A_H

#include "../qcommon/q_shared.h"
#include "../qcommon/qfiles.h"
#include "../renderercommon/tr_public.h"

/* Index 0 of both tables is "unresolved" and is never drawn. */
#define XBOX_NV2A_WHITE_IMAGE 1
#define XBOX_NV2A_WHITE_SHADER 1
#define XBOX_NV2A_MAX_TEXTURE_SIZE 1024
/* ioq3 r_picmip default: mipmapped images drop one size level. */
#define XBOX_NV2A_PICMIP 1
#define XBOX_NV2A_MAX_STAGES 8
#define XBOX_NV2A_MAX_TEXMODS 4
#define XBOX_NV2A_MAX_ANIM_IMAGES 8
/* Same per-surface limits as ioq3's tess (SHADER_MAX_VERTEXES/INDEXES). */
#define XBOX_NV2A_TESS_VERTS 1000
#define XBOX_NV2A_TESS_INDEXES (6 * XBOX_NV2A_TESS_VERTS)
/* ioq3 SKY_SUBDIVISIONS; the cloud grid needs 5 * 81 tess vertices at most. */
#define XBOX_NV2A_SKY_SUBDIVISIONS 8
#define XBOX_NV2A_SKY_SIDES 6

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
#define XBOX_NV2A_SORT_FOG 7.0f
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
	XBOX_NV2A_ALPHAGEN_ONE_MINUS_ENTITY,
	XBOX_NV2A_ALPHAGEN_PORTAL
} xboxNV2AAlphaGen_t;

/* ioq3 acff_t: how a blended stage fades its colours inside fog. */
typedef enum {
	XBOX_NV2A_ACFF_NONE,
	XBOX_NV2A_ACFF_MODULATE_RGB,
	XBOX_NV2A_ACFF_MODULATE_RGBA,
	XBOX_NV2A_ACFF_MODULATE_ALPHA
} xboxNV2AAdjustForFog_t;

/* ioq3 fogPass_t: opaque shaders fog with depth equal, fog volumes with depth less-equal. */
typedef enum {
	XBOX_NV2A_FOGPASS_NONE,
	XBOX_NV2A_FOGPASS_EQUAL,
	XBOX_NV2A_FOGPASS_LE
} xboxNV2AFogPass_t;

typedef enum {
	XBOX_NV2A_TCGEN_TEXTURE,
	XBOX_NV2A_TCGEN_LIGHTMAP,
	XBOX_NV2A_TCGEN_ENVIRONMENT,
	XBOX_NV2A_TCGEN_VECTOR
} xboxNV2ATcGen_t;

typedef enum {
	XBOX_NV2A_CULL_FRONT,
	XBOX_NV2A_CULL_BACK,
	XBOX_NV2A_CULL_NONE
} xboxNV2ACull_t;

/* ioq3 lightmapIndex classes: each one gets a different implicit shader. */
typedef enum {
	XBOX_NV2A_SHADER_2D,
	XBOX_NV2A_SHADER_MODEL,
	XBOX_NV2A_SHADER_LIGHTMAP,
	XBOX_NV2A_SHADER_VERTEX
} xboxNV2AShaderFlavor_t;

/* One ioq3 shader stage; blend factors hold GL values and GL_ONE/GL_ZERO is opaque. */
typedef struct {
	int images[XBOX_NV2A_MAX_ANIM_IMAGES];
	int numImages;
	float animFrequency;
	/* $lightmap: the image comes from the drawn surface, not from images[]. */
	qboolean isLightmap;
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
	int adjustColorsForFog;
} xboxNV2AStage_t;

typedef struct {
	int numStages;
	float sort;
	int cull;
	qboolean polygonOffset;
	qboolean isSky;
	/* skyParms outer box in rt, bk, lf, ft, up, dn order; ioq3 draws none if side 0 is missing. */
	int skyBox[XBOX_NV2A_SKY_SIDES];
	float cloudHeight;
	/* surfaceparm fog; fogParms colour and depthForOpaque. */
	qboolean isFog;
	vec3_t fogColor;
	float fogDepth;
	int fogPass;
	/* alphaGen portal range; ioq3 skips portal views farther than this. */
	float portalRange;
	xboxNV2AStage_t stages[XBOX_NV2A_MAX_STAGES];
} xboxNV2AShaderDef_t;

/* World vertices keep ioq3 drawVert_t data with the lighting colour already shifted. */
typedef struct {
	float xyz[3];
	float st[2];
	float lightSt[2];
	float normal[3];
	byte color[4];
} xboxNV2AWorldVert_t;

typedef enum {
	XBOX_NV2A_SURFACE_FACE,
	XBOX_NV2A_SURFACE_GRID,
	XBOX_NV2A_SURFACE_TRIANGLES
} xboxNV2AWorldSurfaceType_t;

typedef struct {
	int type;
	int shader;
	int lightmap;
	/* nomarks, noimpact, nodraw or fog: ioq3 R_BoxSurfaces_r never puts marks on it. */
	qboolean noMarks;
	int fogIndex;
	int viewCount;
	cplane_t plane;
	vec3_t bounds[2];
	int numVerts;
	int numIndexes;
	xboxNV2AWorldVert_t *verts;
	unsigned short *indexes;
} xboxNV2AWorldSurface_t;

/* ioq3 fog_t; fog 0 means none, and surface is the visible side's plane facing into the fog. */
typedef struct {
	vec3_t bounds[2];
	byte color[4];
	float tcScale;
	qboolean hasSurface;
	float surface[4];
} xboxNV2AFog_t;

/* ioq3 sky_mins/sky_maxs: the (s, t) range of each box side that visible sky covers. */
typedef struct {
	float mins[2][XBOX_NV2A_SKY_SIDES];
	float maxs[2][XBOX_NV2A_SKY_SIDES];
} xboxNV2ASkyBounds_t;

#define XBOX_NV2A_WORLD_ENTITY (-1)

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
/* highColor keeps 8 bits per channel; lightmaps use it to avoid 16-bit banding. */
int XboxNV2A_CreateImage(const char *name, int width, int height,
	const byte *rgba, qboolean highColor);
qboolean XboxNV2A_FindShader(const char *name, int flavor, qhandle_t *handle);
qhandle_t XboxNV2A_CreateShader(const char *name, int flavor,
	const xboxNV2AShaderDef_t *def);
qboolean XboxNV2A_ShaderIsDrawable(qhandle_t shader);
qboolean XboxNV2A_ShaderIsSky(qhandle_t shader);
qboolean XboxNV2A_ShaderFogParms(qhandle_t shader, vec3_t color, float *depth);
int XboxNV2A_ShaderCull(qhandle_t shader);
void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader);
void XboxNV2A_UploadCinematic(int cols, int rows, const byte *data, int client,
	qboolean dirty);
void XboxNV2A_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty);
void XboxNV2A_ClearScene(void);
void XboxNV2A_AddRefEntity(const refEntity_t *entity);
void XboxNV2A_AddLight(const vec3_t origin, float intensity, float r, float g,
	float b);
void XboxNV2A_AddWorldSurface(const xboxNV2AWorldSurface_t *surface, int entity);
void XboxNV2A_AddSkySurface(const xboxNV2AWorldSurface_t *surface);
void XboxNV2A_AddPoly(qhandle_t shader, int numVerts, const polyVert_t *verts,
	int numPolys);
void XboxNV2A_RenderScene(const refdef_t *fd);

/* Shader scripts and image files, in xbox_nv2a_shader.c. */
qhandle_t XboxNV2AShader_Register(const char *name, qboolean mipRawImage);
qhandle_t XboxNV2AShader_RegisterModel(const char *name);
qhandle_t XboxNV2AShader_RegisterWorld(const char *name, int flavor);
void XboxNV2AShader_FreeScripts(void);

/* MD3 models, brush submodels and skins, in xbox_nv2a_model.c. */
qhandle_t XboxNV2AModel_Register(const char *name);
void XboxNV2AModel_RegisterBrush(const char *name, int submodel);
void XboxNV2AModel_FreeAll(void);
const md3Header_t *XboxNV2AModel_Get(qhandle_t handle);
int XboxNV2AModel_BrushIndex(qhandle_t handle);
void XboxNV2AModel_LerpSurface(const md3Surface_t *surface, int frame,
	int oldFrame, float backlerp, float (*xyz)[3], float (*normal)[3]);
void XboxNV2AModel_Bounds(qhandle_t handle, vec3_t mins, vec3_t maxs);
int XboxNV2AModel_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame,
	int endFrame, float frac, const char *tagName);
qhandle_t XboxNV2ASkin_Register(const char *name);
int XboxNV2ASkin_Shader(qhandle_t skin, const char *surfaceName);
void XboxNV2ASkin_FreeAll(void);

/* BSP world, in xbox_nv2a_world.c. */
void XboxNV2AWorld_Load(const char *name);
void XboxNV2AWorld_Free(void);
qboolean XboxNV2AWorld_Loaded(void);
float XboxNV2AWorld_AddSurfaces(const refdef_t *fd, const vec3_t pvsOrigin);
void XboxNV2AWorld_AddBrushModel(int submodel, int entity);
void XboxNV2AWorld_SubmodelBounds(int submodel, vec3_t mins, vec3_t maxs);
void XboxNV2AWorld_SunDirection(vec3_t direction);
qboolean XboxNV2AWorld_LightGrid(const vec3_t origin, vec3_t ambient,
	vec3_t directed, vec3_t direction);
qboolean XboxNV2AWorld_GetEntityToken(char *buffer, int size);
qboolean XboxNV2AWorld_InPVS(const vec3_t p1, const vec3_t p2);
int XboxNV2AWorld_NumFogs(void);
const xboxNV2AFog_t *XboxNV2AWorld_Fog(int index);
int XboxNV2AWorld_MarkFragments(int numPoints, const vec3_t *points,
	const vec3_t projection, int maxPoints, vec3_t pointBuffer, int maxFragments,
	markFragment_t *fragmentBuffer);

/* Sky box and cloud grids, in xbox_nv2a_sky.c; positions are relative to the view origin. */
void XboxNV2ASky_Clear(xboxNV2ASkyBounds_t *bounds);
void XboxNV2ASky_AddSurface(xboxNV2ASkyBounds_t *bounds,
	const xboxNV2AWorldSurface_t *surface, const vec3_t origin);
int XboxNV2ASky_BoxSide(const xboxNV2ASkyBounds_t *bounds, int side, float boxSize,
	float (*xyz)[3], float (*st)[2], unsigned short *indexes, int *numIndexes);
int XboxNV2ASky_Clouds(const xboxNV2ASkyBounds_t *bounds, float cloudHeight,
	float boxSize, float (*xyz)[3], float (*st)[2], unsigned short *indexes,
	int *numIndexes);

#endif
