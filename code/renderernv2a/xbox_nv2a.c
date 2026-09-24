#include "xbox_nv2a.h"

#include "../qcommon/qcommon.h"
#include "../sys/sys_xbox.h"

#include <hal/video.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

/* q_shared.h and xgux.h define the same MIN macro. */
#undef MIN
#include "../thirdparty/xgu/xgu.h"
#include "../thirdparty/xgu/xgux.h"

#ifndef XBOX_NV2A_DIAGNOSTICS
#define XBOX_NV2A_DIAGNOSTICS 0
#endif

typedef struct {
	float position[3];
	float color[4];
	float texcoord[2];
} XboxNV2AColoredVertex;

typedef struct {
	char name[MAX_QPATH];
	void *memory;
	int width;
	int height;
	XguTexFormatColor format;
} XboxNV2AImage;

typedef struct {
	char name[MAX_QPATH];
	int flavor;
	float sort;
	int cull;
	qboolean isSky;
	int numStages;
	xboxNV2AStage_t *stages;
} XboxNV2AShader;

/* ioq3 tess: one batch of CPU vertices with one shader, drawn once per stage. */
typedef struct {
	int shader;
	int lightmap;
	qboolean is3D;
	int numVerts;
	int numIndexes;
	double shaderTime;
	const refEntity_t *entity;
	vec3_t viewOrigin;
	vec3_t ambientLight;
	vec3_t directedLight;
	vec3_t lightDir;
	float xyz[XBOX_NV2A_TESS_VERTS][3];
	float normal[XBOX_NV2A_TESS_VERTS][3];
	float st[XBOX_NV2A_TESS_VERTS][2];
	float lightSt[XBOX_NV2A_TESS_VERTS][2];
	byte color[XBOX_NV2A_TESS_VERTS][4];
	unsigned short indexes[XBOX_NV2A_TESS_INDEXES];
	float stageSt[XBOX_NV2A_TESS_VERTS][2];
	byte stageColor[XBOX_NV2A_TESS_VERTS][4];
	/* Signed distance in front of the near plane; negative vertices get clipped. */
	float nearDist[XBOX_NV2A_TESS_VERTS];
	qboolean clipNear;
} XboxNV2ATess;

/* One MD3 surface or one world/brush surface of the current scene. */
typedef struct {
	const md3Surface_t *md3;
	const xboxNV2AWorldSurface_t *world;
	int entity;
	int shader;
	int lightmap;
	float sort;
	int order;
} XboxNV2ADrawSurf;

typedef struct {
	vec3_t origin;
	float radius;
	vec3_t color;
} XboxNV2ADlight;

#define XBOX_NV2A_MAX_IMAGES 512
/* cl_cin.c MAX_VIDEO_HANDLES; each handle's scratch texture follows the pool images. */
#define XBOX_NV2A_MAX_CINEMATICS 16
#define XBOX_NV2A_MAX_SHADERS 1024
#define XBOX_NV2A_TEXTURE_POOL_BYTES (6u * 1024u * 1024u)
#define XBOX_NV2A_TEXTURE_ALIGN 128u
/* Highest physical address the xgu samples allow for GPU memory. */
#define XBOX_NV2A_MAX_RAM 0x03FFAFFF
#define XBOX_NV2A_TRIANGLE_VERTS 3
#define XBOX_NV2A_MAX_VERTS (2048u * 4u)
#define XBOX_NV2A_CLEAR_COLOR 0xff101820
#define XBOX_NV2A_ZMAX ((float)0xFFFFFF)
/* Release pbkit does not check for overflow; restart well below its 512 KiB limit. */
#define XBOX_NV2A_PUSH_LIMIT_DWORDS (96u * 1024u)
#define XBOX_NV2A_STATE_DWORDS 56u
#define XBOX_NV2A_MAX_SCENE_ENTITIES 256
#define XBOX_NV2A_MAX_SCENE_SURFACES 4096
/* ioq3 MAX_DLIGHTS and the R_SetupEntityLighting falloff constants. */
#define XBOX_NV2A_MAX_DLIGHTS 32
#define XBOX_NV2A_DLIGHT_AT_RADIUS 16.0f
#define XBOX_NV2A_DLIGHT_MINIMUM_RADIUS 16.0f
/* ioq3 r_znear default; RDF_NOWORLDMODEL scenes use a 2048 far plane. */
#define XBOX_NV2A_ZNEAR 4.0f
#define XBOX_NV2A_ZFAR 2048.0f
#define XBOX_NV2A_FUNCTABLE_SIZE 1024
#define XBOX_NV2A_FUNCTABLE_SIZE2 10
#define XBOX_NV2A_FUNCTABLE_MASK (XBOX_NV2A_FUNCTABLE_SIZE - 1)

static XboxNV2AColoredVertex xboxNV2ATriangle[XBOX_NV2A_TRIANGLE_VERTS] = {
	{{0.0f, 0.0f, 1.0f}, {1.0f, 0.08f, 0.04f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 1.0f, 0.16f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 0.24f, 1.0f, 1.0f}, {0.0f, 0.0f}},
};

/* Vertices stream into contiguous memory; the GPU reads them with DRAW_ARRAYS. */
static XboxNV2AColoredVertex *xboxNV2AVertexMemory;
static unsigned int xboxNV2AVertexUsed;
static XboxNV2ATess xboxNV2ATess;
static byte *xboxNV2ATexturePool;
static size_t xboxNV2ATexturePoolUsed;
static XboxNV2AImage xboxNV2AImages[XBOX_NV2A_MAX_IMAGES + XBOX_NV2A_MAX_CINEMATICS];
static unsigned int xboxNV2AImageCount;
static XboxNV2AShader xboxNV2AShaders[XBOX_NV2A_MAX_SHADERS];
static unsigned int xboxNV2AShaderCount;
static int xboxNV2ABoundImage;
static qboolean xboxNV2ABoundClamp;
static unsigned int xboxNV2ABoundSrcBlend;
static unsigned int xboxNV2ABoundDstBlend;
static int xboxNV2ABoundAlphaFunc;
static int xboxNV2ABoundDepthTest;
static int xboxNV2ABoundDepthWrite;
static int xboxNV2ABoundDepthEqual;
static int xboxNV2ABoundCull;
static qboolean xboxNV2ATransformIdentity;
/* w row of the active 3D transform minus znear: the near-plane distance of a vertex. */
static float xboxNV2ANearPlane[4];
static unsigned int xboxNV2APushedDwords;
static XguMatrix4x4 xboxNV2AScreenMatrix;
static int xboxNV2AWidth;
static int xboxNV2AHeight;
static qboolean xboxNV2AInitialized;
static qboolean xboxNV2AInFrame;
static qboolean xboxNV2ADebugScreen;
static float xboxNV2AColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
static float xboxNV2ASinTable[XBOX_NV2A_FUNCTABLE_SIZE];
static float xboxNV2ASquareTable[XBOX_NV2A_FUNCTABLE_SIZE];
static float xboxNV2ATriangleTable[XBOX_NV2A_FUNCTABLE_SIZE];
static float xboxNV2ASawToothTable[XBOX_NV2A_FUNCTABLE_SIZE];
static float xboxNV2AInverseSawToothTable[XBOX_NV2A_FUNCTABLE_SIZE];
static refEntity_t xboxNV2ASceneEntities[XBOX_NV2A_MAX_SCENE_ENTITIES];
static int xboxNV2ASceneEntityCount;
static int xboxNV2ASceneFirstEntity;
static XboxNV2ADlight xboxNV2ADlights[XBOX_NV2A_MAX_DLIGHTS];
static int xboxNV2ADlightCount;
static int xboxNV2ASceneFirstDlight;
static XboxNV2ADrawSurf xboxNV2ADrawSurfs[XBOX_NV2A_MAX_SCENE_SURFACES];
static int xboxNV2ADrawSurfCount;
static qboolean xboxNV2ADrawSurfOverflow;

static float XboxNV2AClamp01(float value)
{
	if (value <= 0.0f)
		return 0.0f;
	if (value >= 1.0f)
		return 1.0f;
	return value;
}

static qboolean XboxNV2AIsPowerOfTwo(int value)
{
	return value > 0 && (value & (value - 1)) == 0;
}

static unsigned int XboxNV2ALog2(int value)
{
	unsigned int shift = 0;

	while ((1 << shift) < value)
		++shift;
	return shift;
}

static unsigned int XboxNV2ASwizzledOffset(unsigned int x, unsigned int y,
	unsigned int width, unsigned int height)
{
	unsigned int offset = 0;
	unsigned int outputBit = 0;
	unsigned int bit;

	for (bit = 1; bit < width || bit < height; bit <<= 1) {
		if (bit < width)
			offset |= ((x & bit) ? 1u : 0u) << outputBit++;
		if (bit < height)
			offset |= ((y & bit) ? 1u : 0u) << outputBit++;
	}
	return offset;
}

/* DIAGNOSTIC: frame/scene trace and GPU-wait timeout for hardware tests; remove after. */
#define XBOX_NV2A_TRACE_FRAMES 3
#define XBOX_NV2A_TRACE_SCENES 16
#define XBOX_NV2A_HEARTBEAT_FRAMES 600
#define XBOX_NV2A_GPU_TIMEOUT_MS 2000
static unsigned int xboxNV2AFrameCount;
static unsigned int xboxNV2ASceneCount;
static int xboxNV2ALastSceneSignature = -1;
static const char *xboxNV2ALastShaderName = "none";

static void XboxNV2AWaitIdle(const char *where)
{
	DWORD start = GetTickCount();

	while (pb_busy()) {
		if (GetTickCount() - start > XBOX_NV2A_GPU_TIMEOUT_MS)
			Sys_Error("Xbox NV2A: GPU hang in %s, frame %u, last shader %s",
				where, xboxNV2AFrameCount, xboxNV2ALastShaderName);
	}
}

/* Callers pass an upper bound; the pushbuffer restarts once the GPU is idle. */
static void XboxNV2AReserve(unsigned int dwords)
{
	if (xboxNV2APushedDwords + dwords > XBOX_NV2A_PUSH_LIMIT_DWORDS) {
		XboxNV2AWaitIdle("reserve");
		pb_reset();
		xboxNV2APushedDwords = 0;
	}
	xboxNV2APushedDwords += dwords;
}

static void XboxNV2AInvalidateState(void)
{
	xboxNV2ABoundImage = -1;
	xboxNV2ABoundClamp = qfalse;
	xboxNV2ABoundSrcBlend = ~0u;
	xboxNV2ABoundDstBlend = ~0u;
	xboxNV2ABoundAlphaFunc = -1;
	xboxNV2ABoundDepthTest = -1;
	xboxNV2ABoundDepthWrite = -1;
	xboxNV2ABoundDepthEqual = -1;
	xboxNV2ABoundCull = -1;
}

/* ioq3 R_InitFogTable/R_Init wave tables. */
static void XboxNV2AInitTables(void)
{
	int i;

	for (i = 0; i < XBOX_NV2A_FUNCTABLE_SIZE; ++i) {
		xboxNV2ASinTable[i] = sinf(i * (2.0f * (float)M_PI / XBOX_NV2A_FUNCTABLE_SIZE));
		xboxNV2ASquareTable[i] = (i < XBOX_NV2A_FUNCTABLE_SIZE / 2) ? 1.0f : -1.0f;
		xboxNV2ASawToothTable[i] = (float)i / XBOX_NV2A_FUNCTABLE_SIZE;
		xboxNV2AInverseSawToothTable[i] = 1.0f - xboxNV2ASawToothTable[i];
		if (i < XBOX_NV2A_FUNCTABLE_SIZE / 2) {
			if (i < XBOX_NV2A_FUNCTABLE_SIZE / 4)
				xboxNV2ATriangleTable[i] = (float)i / (XBOX_NV2A_FUNCTABLE_SIZE / 4);
			else
				xboxNV2ATriangleTable[i] =
					1.0f - xboxNV2ATriangleTable[i - XBOX_NV2A_FUNCTABLE_SIZE / 4];
		} else {
			xboxNV2ATriangleTable[i] = -xboxNV2ATriangleTable[i - XBOX_NV2A_FUNCTABLE_SIZE / 2];
		}
	}
}

static const float *XboxNV2AWaveTable(int func)
{
	switch (func) {
	case XBOX_NV2A_WAVE_TRIANGLE: return xboxNV2ATriangleTable;
	case XBOX_NV2A_WAVE_SQUARE: return xboxNV2ASquareTable;
	case XBOX_NV2A_WAVE_SAWTOOTH: return xboxNV2ASawToothTable;
	case XBOX_NV2A_WAVE_INVERSE_SAWTOOTH: return xboxNV2AInverseSawToothTable;
	default: return xboxNV2ASinTable;
	}
}

/* ioq3 EvalWaveForm; noise needs ioq3's noise table, so it holds its base value. */
static float XboxNV2AEvalWave(const xboxNV2AWave_t *wave, double time)
{
	int index;

	if (wave->func == XBOX_NV2A_WAVE_NOISE)
		return wave->base;
	index = (int)((wave->phase + time * wave->frequency) * XBOX_NV2A_FUNCTABLE_SIZE);
	return wave->base + XboxNV2AWaveTable(wave->func)[index & XBOX_NV2A_FUNCTABLE_MASK] *
		wave->amplitude;
}

static XguBlendFactor XboxNV2ABlendFactor(unsigned int factor)
{
	switch (factor) {
	case GL_ZERO: return XGU_FACTOR_ZERO;
	case GL_ONE: return XGU_FACTOR_ONE;
	case GL_SRC_COLOR: return XGU_FACTOR_SRC_COLOR;
	case GL_ONE_MINUS_SRC_COLOR: return XGU_FACTOR_ONE_MINUS_SRC_COLOR;
	case GL_SRC_ALPHA: return XGU_FACTOR_SRC_ALPHA;
	case GL_ONE_MINUS_SRC_ALPHA: return XGU_FACTOR_ONE_MINUS_SRC_ALPHA;
	case GL_DST_ALPHA: return XGU_FACTOR_DST_ALPHA;
	case GL_ONE_MINUS_DST_ALPHA: return XGU_FACTOR_ONE_MINUS_DST_ALPHA;
	case GL_DST_COLOR: return XGU_FACTOR_DST_COLOR;
	case GL_ONE_MINUS_DST_COLOR: return XGU_FACTOR_ONE_MINUS_DST_COLOR;
	case GL_SRC_ALPHA_SATURATE: return XGU_FACTOR_SRC_ALPHA_SATURATE;
	default: return XGU_FACTOR_ONE;
	}
}

/* 2D stages never touch depth or culling; 3D stages test depth and write it when asked. */
static void XboxNV2AApplyStageState(const xboxNV2AStage_t *stage, int imageIndex,
	qboolean is3D, int cull)
{
	int depthWrite = is3D && stage->depthWrite;
	int depthEqual = is3D && stage->depthEqual;
	uint32_t *p;

	if (!is3D)
		cull = XBOX_NV2A_CULL_NONE;

	XboxNV2AReserve(XBOX_NV2A_STATE_DWORDS);
	p = pb_begin();
	if (imageIndex != xboxNV2ABoundImage || stage->clamp != xboxNV2ABoundClamp) {
		const XboxNV2AImage *image = &xboxNV2AImages[imageIndex];
		XguTextureAddress address = stage->clamp ? XGU_CLAMP_TO_EDGE : XGU_WRAP;

		p = xgu_set_texture_offset(p, 0,
			(void *)((uint32_t)image->memory & 0x03ffffff));
		p = xgu_set_texture_format(p, 0, 2, false, XGU_SOURCE_COLOR, 2,
			image->format, 1, XboxNV2ALog2(image->width),
			XboxNV2ALog2(image->height), 0);
		/* U, V and P must all hold a valid mode; 0 raises a GPU invalid-data error. */
		p = xgu_set_texture_address(p, 0, address, false, address, false,
			address, false, false);
		p = xgu_set_texture_control0(p, 0, true, 0, 0);
		p = xgu_set_texture_filter(p, 0, 0, XGU_TEXTURE_CONVOLUTION_QUINCUNX,
			2, 2, false, false, false, false);
		xboxNV2ABoundImage = imageIndex;
		xboxNV2ABoundClamp = stage->clamp;
	}
	if (stage->srcBlend != xboxNV2ABoundSrcBlend ||
		stage->dstBlend != xboxNV2ABoundDstBlend) {
		p = xgu_set_blend_func_sfactor(p, XboxNV2ABlendFactor(stage->srcBlend));
		p = xgu_set_blend_func_dfactor(p, XboxNV2ABlendFactor(stage->dstBlend));
		xboxNV2ABoundSrcBlend = stage->srcBlend;
		xboxNV2ABoundDstBlend = stage->dstBlend;
	}
	if (stage->alphaFunc != xboxNV2ABoundAlphaFunc) {
		p = xgu_set_alpha_test_enable(p, stage->alphaFunc != XBOX_NV2A_ALPHA_NONE);
		if (stage->alphaFunc == XBOX_NV2A_ALPHA_GT0) {
			p = xgu_set_alpha_func(p, XGU_FUNC_GREATER);
			p = xgu_set_alpha_ref(p, 0);
		} else if (stage->alphaFunc == XBOX_NV2A_ALPHA_LT128) {
			p = xgu_set_alpha_func(p, XGU_FUNC_LESS);
			p = xgu_set_alpha_ref(p, 128);
		} else if (stage->alphaFunc == XBOX_NV2A_ALPHA_GE128) {
			p = xgu_set_alpha_func(p, XGU_FUNC_GREATER_OR_EQUAL);
			p = xgu_set_alpha_ref(p, 128);
		}
		xboxNV2ABoundAlphaFunc = stage->alphaFunc;
	}
	if ((int)is3D != xboxNV2ABoundDepthTest) {
		p = xgu_set_depth_test_enable(p, is3D);
		xboxNV2ABoundDepthTest = is3D;
	}
	if (depthWrite != xboxNV2ABoundDepthWrite) {
		p = xgu_set_depth_mask(p, depthWrite);
		xboxNV2ABoundDepthWrite = depthWrite;
	}
	if (depthEqual != xboxNV2ABoundDepthEqual) {
		p = xgu_set_depth_func(p, depthEqual ? XGU_FUNC_EQUAL : XGU_FUNC_LESS_OR_EQUAL);
		xboxNV2ABoundDepthEqual = depthEqual;
	}
	/* ioq3 GL_Cull: front-sided shaders cull GL_FRONT with the default CCW front face. */
	if (cull != xboxNV2ABoundCull) {
		p = xgu_set_cull_face_enable(p, cull != XBOX_NV2A_CULL_NONE);
		if (cull != XBOX_NV2A_CULL_NONE)
			p = xgu_set_cull_face(p, cull == XBOX_NV2A_CULL_FRONT ? XGU_CULL_FRONT :
				XGU_CULL_BACK);
		xboxNV2ABoundCull = cull;
	}
	pb_end(p);
}

static void XboxNV2ASetTransform(const XguVec4 *rows, qboolean identity)
{
	uint32_t *p;

	XboxNV2AReserve(24);
	p = pb_begin();
	p = xgu_set_transform_constant_load(p, 96);
	p = xgu_set_transform_constant(p, rows, 4);
	pb_end(p);
	xboxNV2ATransformIdentity = identity;
	/* Row vectors: clip w is the dot product of (x, y, z, 1) with column 3. */
	xboxNV2ANearPlane[0] = rows[0].f[3];
	xboxNV2ANearPlane[1] = rows[1].f[3];
	xboxNV2ANearPlane[2] = rows[2].f[3];
	xboxNV2ANearPlane[3] = rows[3].f[3] - XBOX_NV2A_ZNEAR;
}

static uint16_t XboxNV2APackTexel(const byte *rgba, XguTexFormatColor format)
{
	if (format == XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED)
		return (uint16_t)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) |
			(rgba[2] >> 3));
	return (uint16_t)(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
		((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
}

static int XboxNV2AAddImage(const char *name, int width, int height,
	const byte *rgba, qboolean highColor)
{
	XboxNV2AImage *image;
	XguTexFormatColor format = highColor ? XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED :
		XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED;
	size_t textureSize = (size_t)width * (size_t)height * (highColor ? 4 : 2);
	size_t offset;
	size_t pixel;
	int x, y;

	offset = (xboxNV2ATexturePoolUsed + XBOX_NV2A_TEXTURE_ALIGN - 1) &
		~(size_t)(XBOX_NV2A_TEXTURE_ALIGN - 1);
	if (xboxNV2AImageCount >= XBOX_NV2A_MAX_IMAGES ||
		offset + textureSize > XBOX_NV2A_TEXTURE_POOL_BYTES) {
		Sys_XboxLog("Xbox NV2A: texture pool full, skipped %s\n", name);
		return 0;
	}
	for (pixel = 0; !highColor && pixel < (size_t)width * (size_t)height; ++pixel) {
		if (rgba[pixel * 4 + 3] != 255) {
			format = XGU_TEXTURE_FORMAT_A4R4G4B4_SWIZZLED;
			break;
		}
	}

	image = &xboxNV2AImages[xboxNV2AImageCount];
	image->memory = xboxNV2ATexturePool + offset;
	image->width = width;
	image->height = height;
	image->format = format;
	Q_strncpyz(image->name, name, sizeof(image->name));
	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			const byte *texel = rgba + ((size_t)y * width + x) * 4;
			unsigned int swizzled = XboxNV2ASwizzledOffset((unsigned int)x,
				(unsigned int)y, (unsigned int)width, (unsigned int)height);

			if (highColor)
				((uint32_t *)image->memory)[swizzled] = 0xff000000u |
					((uint32_t)texel[0] << 16) | ((uint32_t)texel[1] << 8) | texel[2];
			else
				((uint16_t *)image->memory)[swizzled] = XboxNV2APackTexel(texel, format);
		}
	}
	/* The GPU must not fetch a WC texture before its upload has completed. */
	__asm__ __volatile__("sfence" ::: "memory");
	xboxNV2ATexturePoolUsed = offset + textureSize;
	return (int)xboxNV2AImageCount++;
}

int XboxNV2A_FindImage(const char *name)
{
	unsigned int index;

	for (index = XBOX_NV2A_WHITE_IMAGE; index < xboxNV2AImageCount; ++index) {
		if (!Q_stricmp(xboxNV2AImages[index].name, name))
			return (int)index;
	}
	return 0;
}

int XboxNV2A_CreateImage(const char *name, int width, int height,
	const byte *rgba, qboolean highColor)
{
	int index;

	if (!name || !*name || !rgba || !xboxNV2AInitialized ||
		!XboxNV2AIsPowerOfTwo(width) || !XboxNV2AIsPowerOfTwo(height) ||
		width > XBOX_NV2A_MAX_TEXTURE_SIZE || height > XBOX_NV2A_MAX_TEXTURE_SIZE)
		return 0;
	index = XboxNV2A_FindImage(name);
	return index ? index : XboxNV2AAddImage(name, width, height, rgba, highColor);
}

/* Unresolved names are kept without stages so the loader does not retry them. */
qboolean XboxNV2A_FindShader(const char *name, int flavor, qhandle_t *handle)
{
	unsigned int index;

	for (index = XBOX_NV2A_WHITE_SHADER; index < xboxNV2AShaderCount; ++index) {
		if (xboxNV2AShaders[index].flavor == flavor &&
			!Q_stricmp(xboxNV2AShaders[index].name, name)) {
			*handle = xboxNV2AShaders[index].numStages ? (qhandle_t)index : 0;
			return qtrue;
		}
	}
	*handle = 0;
	return qfalse;
}

qhandle_t XboxNV2A_CreateShader(const char *name, int flavor,
	const xboxNV2AShaderDef_t *def)
{
	XboxNV2AShader *shader;
	int i;

	if (!xboxNV2AInitialized || xboxNV2AShaderCount >= XBOX_NV2A_MAX_SHADERS) {
		Sys_XboxLog("Xbox NV2A: shader table full, skipped %s\n", name);
		return 0;
	}
	shader = &xboxNV2AShaders[xboxNV2AShaderCount];
	memset(shader, 0, sizeof(*shader));
	Q_strncpyz(shader->name, name, sizeof(shader->name));
	shader->flavor = flavor;
	shader->sort = def->sort;
	shader->cull = def->cull;
	shader->isSky = def->isSky;
	if (def->numStages > 0)
		shader->stages = (xboxNV2AStage_t *)malloc(
			(size_t)def->numStages * sizeof(*shader->stages));
	if (shader->stages) {
		memcpy(shader->stages, def->stages,
			(size_t)def->numStages * sizeof(*shader->stages));
		shader->numStages = def->numStages;
		for (i = 0; i < shader->numStages; ++i) {
			int j;

			for (j = 0; j < shader->stages[i].numImages; ++j) {
				if (shader->stages[i].images[j] <= 0 ||
					(unsigned int)shader->stages[i].images[j] >= xboxNV2AImageCount)
					shader->stages[i].images[j] = XBOX_NV2A_WHITE_IMAGE;
			}
		}
	}
	xboxNV2AShaderCount++;
	return shader->numStages ? (qhandle_t)(xboxNV2AShaderCount - 1) : 0;
}

/* The sky box is not drawn yet, so sky surfaces are left out like unresolved ones. */
qboolean XboxNV2A_ShaderIsDrawable(qhandle_t shader)
{
	return shader > 0 && (unsigned int)shader < xboxNV2AShaderCount &&
		xboxNV2AShaders[shader].numStages && !xboxNV2AShaders[shader].isSky;
}

int XboxNV2A_ShaderCull(qhandle_t shader)
{
	if (shader <= 0 || (unsigned int)shader >= xboxNV2AShaderCount)
		return XBOX_NV2A_CULL_NONE;
	return xboxNV2AShaders[shader].cull;
}

/* Scratch textures live outside the pool; callers make sure the GPU is idle. */
static void XboxNV2AFreeCinematics(void)
{
	int i;

	for (i = 0; i < XBOX_NV2A_MAX_CINEMATICS; ++i) {
		XboxNV2AImage *image = &xboxNV2AImages[XBOX_NV2A_MAX_IMAGES + i];

		if (image->memory)
			MmFreeContiguousMemory(image->memory);
		memset(image, 0, sizeof(*image));
	}
}

static void XboxNV2AResetTextures(void)
{
	static const byte whitePixel[4] = {255, 255, 255, 255};
	xboxNV2AShaderDef_t white;
	unsigned int i;

	for (i = 0; i < xboxNV2AShaderCount; ++i)
		free(xboxNV2AShaders[i].stages);
	XboxNV2AFreeCinematics();
	memset(xboxNV2AImages, 0, sizeof(xboxNV2AImages));
	memset(xboxNV2AShaders, 0, sizeof(xboxNV2AShaders));
	Q_strncpyz(xboxNV2AImages[0].name, "*missing", sizeof(xboxNV2AImages[0].name));
	Q_strncpyz(xboxNV2AShaders[0].name, "*missing", sizeof(xboxNV2AShaders[0].name));
	xboxNV2AImageCount = XBOX_NV2A_WHITE_IMAGE;
	xboxNV2AShaderCount = XBOX_NV2A_WHITE_SHADER;
	xboxNV2ATexturePoolUsed = 0;
	XboxNV2AAddImage("*white", 1, 1, whitePixel, qfalse);

	memset(&white, 0, sizeof(white));
	white.numStages = 1;
	white.sort = XBOX_NV2A_SORT_OPAQUE;
	white.cull = XBOX_NV2A_CULL_FRONT;
	white.stages[0].images[0] = XBOX_NV2A_WHITE_IMAGE;
	white.stages[0].numImages = 1;
	white.stages[0].srcBlend = GL_SRC_ALPHA;
	white.stages[0].dstBlend = GL_ONE_MINUS_SRC_ALPHA;
	white.stages[0].clamp = qtrue;
	white.stages[0].rgbGen = XBOX_NV2A_RGBGEN_VERTEX;
	white.stages[0].alphaGen = XBOX_NV2A_ALPHAGEN_VERTEX;
	XboxNV2A_CreateShader("white", XBOX_NV2A_SHADER_2D, &white);

	XboxNV2AInvalidateState();
	xboxNV2ATess.shader = 0;
	xboxNV2ATess.lightmap = 0;
	xboxNV2ATess.numVerts = 0;
	xboxNV2ATess.numIndexes = 0;
}

/* xgux_draw_arrays splits the draw into DRAW_ARRAYS packets of 120 vertices. */
static void XboxNV2ADrawVertices(XguPrimitiveType mode, unsigned int first,
	unsigned int count)
{
	uint32_t *p;

	XboxNV2AReserve(8 + (count / 120 + 1) * 2);
	/* The vertex cache survives frames; drop lines fetched before the CPU rewrote them. */
	p = pb_begin();
	p = push_command_parameter(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
	pb_end(p);
	xgux_draw_arrays(mode, first, count);
}

/* Returns room for count vertices, rewinding the stream once the GPU is idle. */
static XboxNV2AColoredVertex *XboxNV2AStreamVertices(unsigned int count)
{
	if (xboxNV2AVertexUsed + count > XBOX_NV2A_MAX_VERTS) {
		XboxNV2AWaitIdle("vertex stream");
		xboxNV2AVertexUsed = 0;
	}
	return &xboxNV2AVertexMemory[xboxNV2AVertexUsed];
}

/* ioq3 RB_CalcDiffuseColor. */
static void XboxNV2ACalcDiffuseColor(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int i, k;

	for (i = 0; i < tess->numVerts; ++i) {
		float incoming = DotProduct(tess->normal[i], tess->lightDir);

		for (k = 0; k < 3; ++k) {
			float light = tess->ambientLight[k];

			if (incoming > 0.0f)
				light += incoming * tess->directedLight[k];
			tess->stageColor[i][k] = (byte)(light > 255.0f ? 255 : (int)light);
		}
		tess->stageColor[i][3] = 255;
	}
}

/* ioq3 ComputeColors; identityLight is 1 because the NV2A path has no overbright. */
static void XboxNV2AComputeColors(const xboxNV2AStage_t *stage)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	static const byte white[4] = {255, 255, 255, 255};
	const byte *entityColor = tess->entity ? tess->entity->shaderRGBA : white;
	int n = tess->numVerts;
	int i;

	switch (stage->rgbGen) {
	case XBOX_NV2A_RGBGEN_LIGHTING_DIFFUSE:
		XboxNV2ACalcDiffuseColor();
		break;
	case XBOX_NV2A_RGBGEN_VERTEX:
	case XBOX_NV2A_RGBGEN_EXACT_VERTEX:
		memcpy(tess->stageColor, tess->color, (size_t)n * 4);
		break;
	case XBOX_NV2A_RGBGEN_ONE_MINUS_VERTEX:
		for (i = 0; i < n; ++i) {
			tess->stageColor[i][0] = 255 - tess->color[i][0];
			tess->stageColor[i][1] = 255 - tess->color[i][1];
			tess->stageColor[i][2] = 255 - tess->color[i][2];
			tess->stageColor[i][3] = 255;
		}
		break;
	case XBOX_NV2A_RGBGEN_CONST:
		for (i = 0; i < n; ++i)
			memcpy(tess->stageColor[i], stage->constant, 4);
		break;
	case XBOX_NV2A_RGBGEN_WAVE: {
		byte value = (byte)(255 * XboxNV2AClamp01(
			XboxNV2AEvalWave(&stage->rgbWave, tess->shaderTime)));

		for (i = 0; i < n; ++i) {
			tess->stageColor[i][0] = tess->stageColor[i][1] = tess->stageColor[i][2] = value;
			tess->stageColor[i][3] = 255;
		}
		break;
	}
	case XBOX_NV2A_RGBGEN_ENTITY:
		for (i = 0; i < n; ++i)
			memcpy(tess->stageColor[i], entityColor, 4);
		break;
	case XBOX_NV2A_RGBGEN_ONE_MINUS_ENTITY:
		for (i = 0; i < n; ++i) {
			tess->stageColor[i][0] = 255 - entityColor[0];
			tess->stageColor[i][1] = 255 - entityColor[1];
			tess->stageColor[i][2] = 255 - entityColor[2];
			tess->stageColor[i][3] = 255 - entityColor[3];
		}
		break;
	default:
		memset(tess->stageColor, 0xff, (size_t)n * 4);
		break;
	}

	switch (stage->alphaGen) {
	case XBOX_NV2A_ALPHAGEN_IDENTITY:
		if (stage->rgbGen != XBOX_NV2A_RGBGEN_IDENTITY &&
			stage->rgbGen != XBOX_NV2A_RGBGEN_VERTEX) {
			for (i = 0; i < n; ++i)
				tess->stageColor[i][3] = 255;
		}
		break;
	case XBOX_NV2A_ALPHAGEN_CONST:
		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = stage->constant[3];
		break;
	case XBOX_NV2A_ALPHAGEN_WAVE: {
		byte value = (byte)(255 * XboxNV2AClamp01(
			XboxNV2AEvalWave(&stage->alphaWave, tess->shaderTime)));

		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = value;
		break;
	}
	case XBOX_NV2A_ALPHAGEN_ENTITY:
		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = entityColor[3];
		break;
	case XBOX_NV2A_ALPHAGEN_ONE_MINUS_ENTITY:
		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = 255 - entityColor[3];
		break;
	case XBOX_NV2A_ALPHAGEN_VERTEX:
		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = tess->color[i][3];
		break;
	case XBOX_NV2A_ALPHAGEN_ONE_MINUS_VERTEX:
		for (i = 0; i < n; ++i)
			tess->stageColor[i][3] = 255 - tess->color[i][3];
		break;
	}
}

static void XboxNV2ATransformTexCoords(float m00, float m01, float m10, float m11,
	float t0, float t1)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		float s = tess->stageSt[i][0];
		float t = tess->stageSt[i][1];

		tess->stageSt[i][0] = s * m00 + t * m10 + t0;
		tess->stageSt[i][1] = s * m01 + t * m11 + t1;
	}
}

static void XboxNV2AScrollTexCoords(const float *speed)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	double s = speed[0] * tess->shaderTime;
	double t = speed[1] * tess->shaderTime;
	int i;

	/* ioq3 keeps the offset in [0,1) so coordinates stay small. */
	s -= floor(s);
	t -= floor(t);
	for (i = 0; i < tess->numVerts; ++i) {
		tess->stageSt[i][0] += (float)s;
		tess->stageSt[i][1] += (float)t;
	}
}

/* ioq3 ComputeTexCoords and the RB_Calc*TexCoords helpers. */
static void XboxNV2AComputeTexCoords(const xboxNV2AStage_t *stage)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int n = tess->numVerts;
	int i, m;

	if (stage->tcGen == XBOX_NV2A_TCGEN_ENVIRONMENT) {
		for (i = 0; i < n; ++i) {
			vec3_t viewer, reflected;
			float d;

			VectorSubtract(tess->viewOrigin, tess->xyz[i], viewer);
			VectorNormalize(viewer);
			d = DotProduct(tess->normal[i], viewer);
			reflected[1] = tess->normal[i][1] * 2 * d - viewer[1];
			reflected[2] = tess->normal[i][2] * 2 * d - viewer[2];
			tess->stageSt[i][0] = 0.5f + reflected[1] * 0.5f;
			tess->stageSt[i][1] = 0.5f - reflected[2] * 0.5f;
		}
	} else if (stage->tcGen == XBOX_NV2A_TCGEN_VECTOR) {
		for (i = 0; i < n; ++i) {
			tess->stageSt[i][0] = DotProduct(tess->xyz[i], stage->tcGenVectors[0]);
			tess->stageSt[i][1] = DotProduct(tess->xyz[i], stage->tcGenVectors[1]);
		}
	} else if (stage->tcGen == XBOX_NV2A_TCGEN_LIGHTMAP) {
		memcpy(tess->stageSt, tess->lightSt, (size_t)n * sizeof(tess->lightSt[0]));
	} else {
		memcpy(tess->stageSt, tess->st, (size_t)n * sizeof(tess->st[0]));
	}

	for (m = 0; m < stage->numTexMods; ++m) {
		const xboxNV2ATexMod_t *mod = &stage->texMods[m];

		switch (mod->type) {
		case XBOX_NV2A_TCMOD_TURB: {
			double now = mod->wave.phase + tess->shaderTime * mod->wave.frequency;

			for (i = 0; i < n; ++i) {
				int s = (int)(((tess->xyz[i][0] + tess->xyz[i][2]) * (1.0 / 128 * 0.125) +
					now) * XBOX_NV2A_FUNCTABLE_SIZE);
				int t = (int)((tess->xyz[i][1] * (1.0 / 128 * 0.125) + now) *
					XBOX_NV2A_FUNCTABLE_SIZE);

				tess->stageSt[i][0] += xboxNV2ASinTable[s & XBOX_NV2A_FUNCTABLE_MASK] *
					mod->wave.amplitude;
				tess->stageSt[i][1] += xboxNV2ASinTable[t & XBOX_NV2A_FUNCTABLE_MASK] *
					mod->wave.amplitude;
			}
			break;
		}
		case XBOX_NV2A_TCMOD_ENTITY_TRANSLATE:
			if (tess->entity)
				XboxNV2AScrollTexCoords(tess->entity->shaderTexCoord);
			break;
		case XBOX_NV2A_TCMOD_SCROLL:
			XboxNV2AScrollTexCoords(mod->scroll);
			break;
		case XBOX_NV2A_TCMOD_SCALE:
			for (i = 0; i < n; ++i) {
				tess->stageSt[i][0] *= mod->scale[0];
				tess->stageSt[i][1] *= mod->scale[1];
			}
			break;
		case XBOX_NV2A_TCMOD_STRETCH: {
			float wave = XboxNV2AEvalWave(&mod->wave, tess->shaderTime);
			float p = wave != 0.0f ? 1.0f / wave : 1.0f;

			XboxNV2ATransformTexCoords(p, 0.0f, 0.0f, p, 0.5f - 0.5f * p, 0.5f - 0.5f * p);
			break;
		}
		case XBOX_NV2A_TCMOD_TRANSFORM:
			XboxNV2ATransformTexCoords(mod->matrix[0][0], mod->matrix[0][1],
				mod->matrix[1][0], mod->matrix[1][1], mod->translate[0], mod->translate[1]);
			break;
		case XBOX_NV2A_TCMOD_ROTATE: {
			int index = (int)(-mod->rotateSpeed * tess->shaderTime *
				(XBOX_NV2A_FUNCTABLE_SIZE / 360.0f));
			float sinValue = xboxNV2ASinTable[index & XBOX_NV2A_FUNCTABLE_MASK];
			float cosValue = xboxNV2ASinTable[(index + XBOX_NV2A_FUNCTABLE_SIZE / 4) &
				XBOX_NV2A_FUNCTABLE_MASK];

			XboxNV2ATransformTexCoords(cosValue, sinValue, -sinValue, cosValue,
				0.5f - 0.5f * cosValue + 0.5f * sinValue,
				0.5f - 0.5f * sinValue - 0.5f * cosValue);
			break;
		}
		}
	}
}

/* ioq3 RB_StageIteratorGeneric image selection, including animMap and $lightmap. */
static int XboxNV2AStageImage(const xboxNV2AStage_t *stage, double time)
{
	int index;

	if (stage->isLightmap)
		return xboxNV2ATess.lightmap > 0 ? xboxNV2ATess.lightmap : XBOX_NV2A_WHITE_IMAGE;
	if (stage->numImages <= 1)
		return stage->images[0];
	index = (int)(time * stage->animFrequency * XBOX_NV2A_FUNCTABLE_SIZE) >>
		XBOX_NV2A_FUNCTABLE_SIZE2;
	if (index < 0)
		index = 0;
	return stage->images[index % stage->numImages];
}

static void XboxNV2AWriteVertex(XboxNV2AColoredVertex *out, int v)
{
	const XboxNV2ATess *tess = &xboxNV2ATess;

	out->position[0] = tess->xyz[v][0];
	out->position[1] = tess->xyz[v][1];
	out->position[2] = tess->xyz[v][2];
	out->color[0] = tess->stageColor[v][0] * (1.0f / 255.0f);
	out->color[1] = tess->stageColor[v][1] * (1.0f / 255.0f);
	out->color[2] = tess->stageColor[v][2] * (1.0f / 255.0f);
	out->color[3] = tess->stageColor[v][3] * (1.0f / 255.0f);
	out->texcoord[0] = tess->stageSt[v][0];
	out->texcoord[1] = tess->stageSt[v][1];
}

/* The point where edge a-b crosses the near plane, with every attribute interpolated. */
static void XboxNV2AWriteNearVertex(XboxNV2AColoredVertex *out, int a, int b)
{
	const XboxNV2ATess *tess = &xboxNV2ATess;
	float t = tess->nearDist[a] / (tess->nearDist[a] - tess->nearDist[b]);
	int k;

	for (k = 0; k < 3; ++k)
		out->position[k] = tess->xyz[a][k] + t * (tess->xyz[b][k] - tess->xyz[a][k]);
	for (k = 0; k < 4; ++k)
		out->color[k] = (tess->stageColor[a][k] + t * ((float)tess->stageColor[b][k] -
			tess->stageColor[a][k])) * (1.0f / 255.0f);
	for (k = 0; k < 2; ++k)
		out->texcoord[k] = tess->stageSt[a][k] + t * (tess->stageSt[b][k] - tess->stageSt[a][k]);
}

/* Sutherland-Hodgman against the near plane; the kept polygon is fanned into 1 or 2 triangles. */
static unsigned int XboxNV2AClipTriangle(const unsigned short *triangle,
	XboxNV2AColoredVertex *out)
{
	const XboxNV2ATess *tess = &xboxNV2ATess;
	XboxNV2AColoredVertex polygon[4];
	unsigned int count = 0;
	int k;

	for (k = 0; k < 3; ++k) {
		int a = triangle[k];
		int b = triangle[(k + 1) % 3];
		qboolean aInside = tess->nearDist[a] >= 0.0f;

		if (aInside)
			XboxNV2AWriteVertex(&polygon[count++], a);
		if (aInside != (tess->nearDist[b] >= 0.0f))
			XboxNV2AWriteNearVertex(&polygon[count++], a, b);
	}
	if (count < 3)
		return 0;
	out[0] = polygon[0];
	out[1] = polygon[1];
	out[2] = polygon[2];
	if (count == 3)
		return 3;
	out[3] = polygon[0];
	out[4] = polygon[2];
	out[5] = polygon[3];
	return 6;
}

/* Triangles stream in chunks; a full stream is drawn, then rewound once the GPU is idle. */
static void XboxNV2ADrawTriangles(void)
{
	const XboxNV2ATess *tess = &xboxNV2ATess;
	unsigned int first = xboxNV2AVertexUsed;
	unsigned int written = 0;
	int i;

	for (i = 0; i + 2 < tess->numIndexes; i += 3) {
		const unsigned short *triangle = &tess->indexes[i];
		XboxNV2AColoredVertex clipped[6];
		unsigned int count = 3;
		qboolean crosses = tess->clipNear && (tess->nearDist[triangle[0]] < 0.0f ||
			tess->nearDist[triangle[1]] < 0.0f || tess->nearDist[triangle[2]] < 0.0f);
		XboxNV2AColoredVertex *out;

		if (crosses) {
			count = XboxNV2AClipTriangle(triangle, clipped);
			if (!count)
				continue;
		}
		if (first + written + count > XBOX_NV2A_MAX_VERTS) {
			if (written)
				XboxNV2ADrawVertices(XGU_TRIANGLES, first, written);
			XboxNV2AWaitIdle("vertex stream");
			first = 0;
			written = 0;
		}
		out = &xboxNV2AVertexMemory[first + written];
		if (crosses) {
			memcpy(out, clipped, count * sizeof(*out));
		} else {
			XboxNV2AWriteVertex(&out[0], triangle[0]);
			XboxNV2AWriteVertex(&out[1], triangle[1]);
			XboxNV2AWriteVertex(&out[2], triangle[2]);
		}
		written += count;
	}
	if (written)
		XboxNV2ADrawVertices(XGU_TRIANGLES, first, written);
	xboxNV2AVertexUsed = first + written;
}

/* Draws one stage: 2D tess holds quads in order, 3D tess holds indexed triangles. */
static void XboxNV2ADrawStage(const xboxNV2AStage_t *stage, int cull)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	XboxNV2AColoredVertex *out;
	unsigned int count = (unsigned int)tess->numVerts;
	unsigned int i;

	XboxNV2AComputeColors(stage);
	XboxNV2AComputeTexCoords(stage);
	XboxNV2AApplyStageState(stage, XboxNV2AStageImage(stage, tess->shaderTime),
		tess->is3D, cull);
	if (tess->is3D) {
		XboxNV2ADrawTriangles();
		return;
	}
	out = XboxNV2AStreamVertices(count);
	for (i = 0; i < count; ++i)
		XboxNV2AWriteVertex(&out[i], (int)i);
	XboxNV2ADrawVertices(XGU_QUADS, xboxNV2AVertexUsed, count);
	xboxNV2AVertexUsed += count;
}

/* Returns qfalse when the whole 3D batch lies behind the near plane. */
static qboolean XboxNV2AComputeNearDistances(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int behind = 0;
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		tess->nearDist[i] = tess->xyz[i][0] * xboxNV2ANearPlane[0] +
			tess->xyz[i][1] * xboxNV2ANearPlane[1] +
			tess->xyz[i][2] * xboxNV2ANearPlane[2] + xboxNV2ANearPlane[3];
		if (tess->nearDist[i] < 0.0f)
			++behind;
	}
	tess->clipNear = behind > 0;
	return behind < tess->numVerts;
}

static void XboxNV2AEndSurface(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const XboxNV2AShader *shader = &xboxNV2AShaders[tess->shader];
	int i;

	if (tess->shader > 0 && tess->numVerts > 0 && (!tess->is3D || tess->numIndexes > 0) &&
		(!tess->is3D || XboxNV2AComputeNearDistances())) {
		xboxNV2ALastShaderName = shader->name;
		if (!tess->is3D && xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES)
			Sys_XboxLog("Xbox NV2A: frame %u 2d draw %s verts=%d stages=%d image0=%d %dx%d\n",
				xboxNV2AFrameCount, shader->name, tess->numVerts, shader->numStages,
				shader->stages[0].images[0],
				xboxNV2AImages[shader->stages[0].images[0]].width,
				xboxNV2AImages[shader->stages[0].images[0]].height);
		for (i = 0; i < shader->numStages; ++i)
			XboxNV2ADrawStage(&shader->stages[i], shader->cull);
	}
	tess->shader = 0;
	tess->lightmap = 0;
	tess->numVerts = 0;
	tess->numIndexes = 0;
}

/* Same sequence as the xgu samples' init_shader. */
static void XboxNV2AInitShader(void)
{
	static const XguTransformProgramInstruction vertexProgram[] = {
#include "xbox_nv2a_vp.inl"
	};
	uint32_t *p;
	unsigned int i;

	p = pb_begin();
	p = xgu_set_transform_program_start(p, 0);
	p = xgu_set_transform_execution_mode(p, XGU_PROGRAM, XGU_RANGE_MODE_PRIVATE);
	p = xgu_set_transform_program_cxt_write_enable(p, false);
	p = xgu_set_transform_program_load(p, 0);
	for (i = 0; i < ARRAY_LEN(vertexProgram); ++i) {
		p = push_command(p, NV097_SET_TRANSFORM_PROGRAM, 4);
		p = push_parameters(p, &vertexProgram[i].i[0], 4);
	}
	pb_end(p);

	p = pb_begin();
#include "xbox_nv2a_fp.inl"
	pb_end(p);
}

static void XboxNV2ASetFrameState(void)
{
	const XboxNV2AColoredVertex *v = xboxNV2AVertexMemory;
	uint32_t *p;
	int i;

	XboxNV2AReserve(64 + (XGU_ATTRIBUTE_COUNT + 3) * 4);
	xgux_set_clear_rect(0, 0, (unsigned int)xboxNV2AWidth,
		(unsigned int)xboxNV2AHeight);
	p = pb_begin();
	/* pbkit's target switch turns on W-buffering; use the z-buffer, as nxdk-gles11 does. */
	p = push_command_parameter(p, NV097_SET_CONTROL0,
		NV097_SET_CONTROL0_STENCIL_WRITE_ENABLE | NV097_SET_CONTROL0_TEXTUREPERSPECTIVE);
	p = xgu_set_color_clear_value(p, XBOX_NV2A_CLEAR_COLOR);
	p = xgu_set_zstencil_clear_value(p, 0xffffff00);
	p = xgu_clear_surface(p, XGU_CLEAR_Z | XGU_CLEAR_STENCIL | XGU_CLEAR_COLOR);
	/* GL default front face; the viewport flips Y like GL, so winding stays GL's. */
	p = xgu_set_front_face(p, XGU_FRONT_CCW);
	p = xgu_set_cull_face_enable(p, false);
	p = xgu_set_depth_test_enable(p, false);
	p = xgu_set_depth_mask(p, false);
	p = xgu_set_stencil_test_enable(p, false);
	p = xgu_set_alpha_test_enable(p, false);
	p = xgu_set_blend_enable(p, true);
	pb_end(p);
	XboxNV2AInvalidateState();
	XboxNV2ASetTransform(xboxNV2AScreenMatrix.col, qtrue);

	for (i = 0; i < XGU_ATTRIBUTE_COUNT; ++i)
		xgux_set_attrib_pointer((XguVertexArray)i, XGU_FLOAT, 0, 0, NULL);
	xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 3, sizeof(*v),
		&v[0].position[0]);
	xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XGU_FLOAT, 4, sizeof(*v),
		&v[0].color[0]);
	xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(*v),
		&v[0].texcoord[0]);
}

static void XboxNV2AReleaseMemory(void)
{
	XboxNV2AFreeCinematics();
	if (xboxNV2AVertexMemory) {
		MmFreeContiguousMemory(xboxNV2AVertexMemory);
		xboxNV2AVertexMemory = NULL;
	}
	if (xboxNV2ATexturePool) {
		MmFreeContiguousMemory(xboxNV2ATexturePool);
		xboxNV2ATexturePool = NULL;
	}
}

qboolean XboxNV2A_Init(void)
{
	int status;
	int i;

	if (xboxNV2AInitialized)
		return qtrue;

	/* xbox_boot.c selects the mode before Com_Init reaches this boundary. */
	status = pb_init();
	if (status) {
		Sys_XboxLog("Xbox NV2A: pb_init failed (%d)\n", status);
		return qfalse;
	}
	pb_show_front_screen();

	xboxNV2AWidth = (int)pb_back_buffer_width();
	xboxNV2AHeight = (int)pb_back_buffer_height();
	if (xboxNV2AWidth <= 0 || xboxNV2AHeight <= 0) {
		Sys_XboxLog("Xbox NV2A: invalid back buffer %dx%d\n",
			xboxNV2AWidth, xboxNV2AHeight);
		pb_show_debug_screen();
		pb_kill();
		return qfalse;
	}

	XboxNV2AInitShader();

	xboxNV2ATexturePool = MmAllocateContiguousMemoryEx(
		XBOX_NV2A_TEXTURE_POOL_BYTES, 0, XBOX_NV2A_MAX_RAM, 0,
		PAGE_WRITECOMBINE | PAGE_READWRITE);
	xboxNV2AVertexMemory = MmAllocateContiguousMemoryEx(
		XBOX_NV2A_MAX_VERTS * sizeof(XboxNV2AColoredVertex), 0,
		XBOX_NV2A_MAX_RAM, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
	if (!xboxNV2ATexturePool || !xboxNV2AVertexMemory) {
		Sys_XboxLog("Xbox NV2A: texture/vertex allocation failed\n");
		XboxNV2AReleaseMemory();
		pb_show_debug_screen();
		pb_kill();
		return qfalse;
	}
	xboxNV2ATriangle[0].position[0] = xboxNV2AWidth * 0.18f;
	xboxNV2ATriangle[0].position[1] = xboxNV2AHeight * 0.70f;
	xboxNV2ATriangle[1].position[0] = xboxNV2AWidth * 0.50f;
	xboxNV2ATriangle[1].position[1] = xboxNV2AHeight * 0.20f;
	xboxNV2ATriangle[2].position[0] = xboxNV2AWidth * 0.82f;
	xboxNV2ATriangle[2].position[1] = xboxNV2AHeight * 0.70f;
	XboxNV2AInitTables();

	/* 2D vertices are already screen pixels, which program mode outputs directly. */
	memset(&xboxNV2AScreenMatrix, 0, sizeof(xboxNV2AScreenMatrix));
	for (i = 0; i < 4; ++i)
		xboxNV2AScreenMatrix.col[i].f[i] = 1.0f;
	/* The xgu samples' z-buffer precision fixup. */
	xgux_set_depth_range(0.0f, XBOX_NV2A_ZMAX);

	xboxNV2AInitialized = qtrue;
	XboxNV2AResetTextures();
	xboxNV2ADebugScreen = qfalse;
	Sys_XboxLog("Xbox NV2A: pbkit initialized at %dx%d\n",
		xboxNV2AWidth, xboxNV2AHeight);
	return qtrue;
}

/* pbkit stays alive; the client calls this on every map load and vid_restart. */
void XboxNV2A_Shutdown(qboolean destroyWindow)
{
	/* A full ref restart can follow a game directory change, so rescan scripts. */
	if (destroyWindow)
		XboxNV2AShader_FreeScripts();
	if (!xboxNV2AInitialized)
		return;
	XboxNV2AWaitIdle("shutdown");
	Sys_XboxLog("Xbox NV2A: %u images, %u shaders, %u KiB texture pool used\n",
		xboxNV2AImageCount, xboxNV2AShaderCount,
		(unsigned int)(xboxNV2ATexturePoolUsed / 1024));
	/* The world, models and skins hold shader handles, so all tables reset together. */
	XboxNV2AWorld_Free();
	XboxNV2AResetTextures();
	XboxNV2AModel_FreeAll();
	XboxNV2ASkin_FreeAll();
	xboxNV2ASceneEntityCount = 0;
	xboxNV2ASceneFirstEntity = 0;
	xboxNV2ADlightCount = 0;
	xboxNV2ASceneFirstDlight = 0;
}

void XboxNV2A_Kill(void)
{
	DWORD start = GetTickCount();

	if (!xboxNV2AInitialized)
		return;
	/* Bounded: Sys_Error calls this, and a hung GPU must not block the exit. */
	while (pb_busy() && GetTickCount() - start < XBOX_NV2A_GPU_TIMEOUT_MS)
		;
	XboxNV2AReleaseMemory();
	pb_show_debug_screen();
	pb_kill();
	xboxNV2AInitialized = qfalse;
	xboxNV2AInFrame = qfalse;
	xboxNV2ADebugScreen = qtrue;
}

void XboxNV2A_ShowDebugScreen(void)
{
	if (!xboxNV2AInitialized)
		return;
	pb_show_debug_screen();
	xboxNV2ADebugScreen = qtrue;
}

qboolean XboxNV2A_OwnsScreen(void)
{
	return xboxNV2AInitialized && !xboxNV2ADebugScreen;
}

void XboxNV2A_BeginRegistration(glconfig_t *config)
{
	if (!xboxNV2AInitialized)
		XboxNV2A_Init();

	memset(config, 0, sizeof(*config));
	config->vidWidth = xboxNV2AWidth;
	config->vidHeight = xboxNV2AHeight;
	config->windowAspect = xboxNV2AHeight > 0 ?
		(float)xboxNV2AWidth / (float)xboxNV2AHeight : 1.0f;
	config->maxTextureSize = XBOX_NV2A_MAX_TEXTURE_SIZE;
	config->numTextureUnits = 2;
	config->colorBits = 32;
	config->depthBits = 24;
	config->stencilBits = 8;
	config->driverType = GLDRV_STANDALONE;
	config->hardwareType = GLHW_GENERIC;
	config->isFullscreen = qtrue;
	Q_strncpyz(config->renderer_string, "native Xbox NV2A (pbkit)",
		sizeof(config->renderer_string));
	Q_strncpyz(config->vendor_string, "NVIDIA NV2A", sizeof(config->vendor_string));
	Q_strncpyz(config->version_string, "pbkit", sizeof(config->version_string));
}

/* Frame start follows the xgu samples: vblank, reset, target, idle, then clear. */
void XboxNV2A_BeginFrame(stereoFrame_t stereoFrame)
{
	(void)stereoFrame;
	if (!xboxNV2AInitialized)
		return;

	++xboxNV2AFrameCount;
	if (xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES ||
		xboxNV2AFrameCount % XBOX_NV2A_HEARTBEAT_FRAMES == 0)
		Sys_XboxLog("Xbox NV2A: frame %u begin\n", xboxNV2AFrameCount);
	pb_wait_for_vbl();
	pb_reset();
	pb_target_back_buffer();
	XboxNV2AWaitIdle("begin frame");
	xboxNV2APushedDwords = 0;
	xboxNV2AVertexUsed = 0;
	xboxNV2ATess.shader = 0;
	xboxNV2ATess.numVerts = 0;
	xboxNV2ATess.numIndexes = 0;
	if (XBOX_NV2A_DIAGNOSTICS) {
		pb_erase_text_screen();
		pb_printat(0, 0, "ioQuake3 Xbox NV2A %dx%d", xboxNV2AWidth, xboxNV2AHeight);
		pb_printat(1, 0, "xgu textured 2D UI path");
	}

	XboxNV2ASetFrameState();
	xboxNV2AInFrame = qtrue;
	if (XBOX_NV2A_DIAGNOSTICS) {
		XboxNV2AColoredVertex *v = XboxNV2AStreamVertices(XBOX_NV2A_TRIANGLE_VERTS);

		memcpy(v, xboxNV2ATriangle, sizeof(xboxNV2ATriangle));
		XboxNV2AApplyStageState(&xboxNV2AShaders[XBOX_NV2A_WHITE_SHADER].stages[0],
			XBOX_NV2A_WHITE_IMAGE, qfalse, XBOX_NV2A_CULL_NONE);
		XboxNV2ADrawVertices(XGU_TRIANGLES, xboxNV2AVertexUsed,
			XBOX_NV2A_TRIANGLE_VERTS);
		xboxNV2AVertexUsed += XBOX_NV2A_TRIANGLE_VERTS;
	}
}

void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec)
{
	DWORD start;

	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (!xboxNV2AInitialized || !xboxNV2AInFrame)
		return;

	XboxNV2AEndSurface();
	/* ioq3 R_InitNextFrame: scene entities and dlights last one frame. */
	xboxNV2ASceneEntityCount = 0;
	xboxNV2ASceneFirstEntity = 0;
	xboxNV2ADlightCount = 0;
	xboxNV2ASceneFirstDlight = 0;
	xboxNV2AInFrame = qfalse;
	XboxNV2AWaitIdle("end frame");
	if (XBOX_NV2A_DIAGNOSTICS)
		pb_draw_text_screen();
	start = GetTickCount();
	while (pb_finished()) {
		if (GetTickCount() - start > XBOX_NV2A_GPU_TIMEOUT_MS)
			Sys_Error("Xbox NV2A: flip timeout, frame %u", xboxNV2AFrameCount);
	}
	if (xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES)
		Sys_XboxLog("Xbox NV2A: frame %u end\n", xboxNV2AFrameCount);
}

void XboxNV2A_SetColor(const float *rgba)
{
	int i;

	for (i = 0; i < 4; ++i)
		xboxNV2AColor[i] = rgba ? XboxNV2AClamp01(rgba[i]) : 1.0f;
}

/* ioq3 RB_StretchPic: quads batch in the tess until the shader changes. */
void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	static const float corners[4][4] = {
		{0.0f, 0.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f, 0.0f},
		{1.0f, 1.0f, 1.0f, 1.0f}, {0.0f, 1.0f, 0.0f, 1.0f}
	};
	byte color[4];
	int i;

	if (!xboxNV2AInFrame || w <= 0.0f || h <= 0.0f || shader <= 0 ||
		(unsigned int)shader >= xboxNV2AShaderCount ||
		!xboxNV2AShaders[shader].numStages)
		return;
	if (tess->shader != shader || tess->is3D ||
		tess->numVerts + 4 > XBOX_NV2A_TESS_VERTS) {
		XboxNV2AEndSurface();
		if (!xboxNV2ATransformIdentity)
			XboxNV2ASetTransform(xboxNV2AScreenMatrix.col, qtrue);
		tess->shader = shader;
		tess->is3D = qfalse;
		tess->entity = NULL;
		tess->shaderTime = Sys_Milliseconds() * 0.001;
	}
	for (i = 0; i < 4; ++i)
		color[i] = (byte)(xboxNV2AColor[i] * 255.0f);
	for (i = 0; i < 4; ++i) {
		int v = tess->numVerts + i;

		tess->xyz[v][0] = x + w * corners[i][0];
		tess->xyz[v][1] = y + h * corners[i][1];
		tess->xyz[v][2] = 1.0f;
		VectorClear(tess->normal[v]);
		tess->st[v][0] = corners[i][2] ? s2 : s1;
		tess->st[v][1] = corners[i][3] ? t2 : t1;
		memcpy(tess->color[v], color, 4);
	}
	tess->numVerts += 4;
}

/* ioq3 RE_UploadCinematic: a new size reallocates, otherwise only dirty frames are copied. */
void XboxNV2A_UploadCinematic(int cols, int rows, const byte *data, int client,
	qboolean dirty)
{
	static unsigned int swizzleX[XBOX_NV2A_MAX_TEXTURE_SIZE];
	static unsigned int swizzleY[XBOX_NV2A_MAX_TEXTURE_SIZE];
	static qboolean warned;
	XboxNV2AImage *image;
	uint32_t *texels;
	qboolean resized;
	int x, y;

	if (!xboxNV2AInitialized || !data || client < 0 || client >= XBOX_NV2A_MAX_CINEMATICS ||
		!XboxNV2AIsPowerOfTwo(cols) || !XboxNV2AIsPowerOfTwo(rows) ||
		cols > XBOX_NV2A_MAX_TEXTURE_SIZE || rows > XBOX_NV2A_MAX_TEXTURE_SIZE)
		return;
	image = &xboxNV2AImages[XBOX_NV2A_MAX_IMAGES + client];
	resized = !image->memory || image->width != cols || image->height != rows;
	if (!resized && !dirty)
		return;
	/* An earlier draw in this frame may still sample the old texels. */
	XboxNV2AWaitIdle("cinematic upload");
	if (resized) {
		if (image->memory)
			MmFreeContiguousMemory(image->memory);
		memset(image, 0, sizeof(*image));
		image->memory = MmAllocateContiguousMemoryEx((size_t)cols * (size_t)rows * 4, 0,
			XBOX_NV2A_MAX_RAM, 0, PAGE_WRITECOMBINE | PAGE_READWRITE);
		if (!image->memory) {
			if (!warned)
				Sys_XboxLog("Xbox NV2A: no memory for a %dx%d cinematic\n", cols, rows);
			warned = qtrue;
			return;
		}
		image->width = cols;
		image->height = rows;
		image->format = XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED;
		Com_sprintf(image->name, sizeof(image->name), "*cinematic%d", client);
	}
	/* x and y bits never share a swizzled bit, so one table per axis is enough. */
	for (x = 0; x < cols; ++x)
		swizzleX[x] = XboxNV2ASwizzledOffset((unsigned int)x, 0, (unsigned int)cols,
			(unsigned int)rows);
	for (y = 0; y < rows; ++y)
		swizzleY[y] = XboxNV2ASwizzledOffset(0, (unsigned int)y, (unsigned int)cols,
			(unsigned int)rows);
	texels = (uint32_t *)image->memory;
	for (y = 0; y < rows; ++y) {
		const byte *rgba = data + (size_t)y * (size_t)cols * 4;

		for (x = 0; x < cols; ++x, rgba += 4)
			texels[swizzleY[y] | swizzleX[x]] = 0xff000000u | ((uint32_t)rgba[0] << 16) |
				((uint32_t)rgba[1] << 8) | rgba[2];
	}
	__asm__ __volatile__("sfence" ::: "memory");
	/* The NV2A texture cache has no documented flush, so rebind as for a new texture. */
	xboxNV2ABoundImage = -1;
}

/* ioq3 RE_StretchRaw: an opaque 2D quad with half-texel insets, in submission order. */
void XboxNV2A_DrawStretchRaw(int x, int y, int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty)
{
	static const float corners[4][2] = {
		{0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 1.0f}
	};
	const XboxNV2AImage *image;
	xboxNV2AStage_t stage;
	XboxNV2AColoredVertex *out;
	float s1, t1, s2, t2;
	int i, k;

	if (!xboxNV2AInFrame || w <= 0 || h <= 0 || client < 0 ||
		client >= XBOX_NV2A_MAX_CINEMATICS)
		return;
	XboxNV2AEndSurface();
	XboxNV2A_UploadCinematic(cols, rows, data, client, dirty);
	image = &xboxNV2AImages[XBOX_NV2A_MAX_IMAGES + client];
	if (!image->memory || image->width != cols || image->height != rows)
		return;
	if (!xboxNV2ATransformIdentity)
		XboxNV2ASetTransform(xboxNV2AScreenMatrix.col, qtrue);

	memset(&stage, 0, sizeof(stage));
	stage.clamp = qtrue;
	stage.srcBlend = GL_ONE;
	stage.dstBlend = GL_ZERO;
	XboxNV2AApplyStageState(&stage, XBOX_NV2A_MAX_IMAGES + client, qfalse,
		XBOX_NV2A_CULL_NONE);

	s1 = 0.5f / cols;
	t1 = 0.5f / rows;
	s2 = (cols - 0.5f) / cols;
	t2 = (rows - 0.5f) / rows;
	out = XboxNV2AStreamVertices(4);
	for (i = 0; i < 4; ++i) {
		out[i].position[0] = x + w * corners[i][0];
		out[i].position[1] = y + h * corners[i][1];
		out[i].position[2] = 1.0f;
		for (k = 0; k < 4; ++k)
			out[i].color[k] = 1.0f;
		out[i].texcoord[0] = corners[i][0] ? s2 : s1;
		out[i].texcoord[1] = corners[i][1] ? t2 : t1;
	}
	XboxNV2ADrawVertices(XGU_QUADS, xboxNV2AVertexUsed, 4);
	xboxNV2AVertexUsed += 4;
}


void XboxNV2A_ClearScene(void)
{
	xboxNV2ASceneFirstEntity = xboxNV2ASceneEntityCount;
	xboxNV2ASceneFirstDlight = xboxNV2ADlightCount;
}

void XboxNV2A_AddRefEntity(const refEntity_t *entity)
{
	if (!entity || xboxNV2ASceneEntityCount >= XBOX_NV2A_MAX_SCENE_ENTITIES)
		return;
	xboxNV2ASceneEntities[xboxNV2ASceneEntityCount++] = *entity;
}

/* ioq3 RE_AddDynamicLightToScene; dlights only light entities, not world surfaces. */
void XboxNV2A_AddLight(const vec3_t origin, float intensity, float r, float g,
	float b)
{
	XboxNV2ADlight *light;

	if (intensity <= 0.0f || xboxNV2ADlightCount >= XBOX_NV2A_MAX_DLIGHTS)
		return;
	light = &xboxNV2ADlights[xboxNV2ADlightCount++];
	VectorCopy(origin, light->origin);
	light->radius = intensity;
	VectorSet(light->color, r, g, b);
}

static void XboxNV2AAddDrawSurf(const md3Surface_t *md3,
	const xboxNV2AWorldSurface_t *world, int entity, int shader, int lightmap)
{
	XboxNV2ADrawSurf *drawSurf;

	if (xboxNV2ADrawSurfCount >= XBOX_NV2A_MAX_SCENE_SURFACES) {
		if (!xboxNV2ADrawSurfOverflow)
			Sys_XboxLog("Xbox NV2A: scene surface limit %d reached\n",
				XBOX_NV2A_MAX_SCENE_SURFACES);
		xboxNV2ADrawSurfOverflow = qtrue;
		return;
	}
	drawSurf = &xboxNV2ADrawSurfs[xboxNV2ADrawSurfCount];
	drawSurf->md3 = md3;
	drawSurf->world = world;
	drawSurf->entity = entity;
	drawSurf->shader = shader;
	drawSurf->lightmap = lightmap;
	drawSurf->sort = xboxNV2AShaders[shader].sort;
	drawSurf->order = xboxNV2ADrawSurfCount++;
}

void XboxNV2A_AddWorldSurface(const xboxNV2AWorldSurface_t *surface, int entity)
{
	XboxNV2AAddDrawSurf(NULL, surface, entity, surface->shader, surface->lightmap);
}

/* ioq3 R_AddMD3Surfaces shader choice: customShader, then customSkin, then the MD3 list. */
static int XboxNV2AMD3SurfaceShader(const refEntity_t *entity,
	const md3Surface_t *surface)
{
	const md3Shader_t *shaders = (const md3Shader_t *)((const byte *)surface +
		surface->ofsShaders);

	if (entity->customShader)
		return entity->customShader;
	if (entity->customSkin > 0)
		return XboxNV2ASkin_Shader(entity->customSkin, surface->name);
	if (surface->numShaders <= 0)
		return 0;
	return shaders[entity->skinNum >= 0 ? entity->skinNum % surface->numShaders : 0].shaderIndex;
}

static void XboxNV2AAddEntitySurfaces(void)
{
	int e, s;

	for (e = xboxNV2ASceneFirstEntity; e < xboxNV2ASceneEntityCount; ++e) {
		refEntity_t *entity = &xboxNV2ASceneEntities[e];
		const md3Header_t *md3;
		const md3Surface_t *surface;
		int brush;

		if (entity->reType != RT_MODEL)
			continue;
		brush = XboxNV2AModel_BrushIndex(entity->hModel);
		if (brush >= 0) {
			XboxNV2AWorld_AddBrushModel(brush, e);
			continue;
		}
		/* ioq3 R_AddMD3Surfaces: the player's own body only shows through portals. */
		md3 = XboxNV2AModel_Get(entity->hModel);
		if (!md3 || (entity->renderfx & RF_THIRD_PERSON))
			continue;
		if (entity->frame < 0 || entity->frame >= md3->numFrames ||
			entity->oldframe < 0 || entity->oldframe >= md3->numFrames) {
			entity->frame = 0;
			entity->oldframe = 0;
		}
		surface = (const md3Surface_t *)((const byte *)md3 + md3->ofsSurfaces);
		for (s = 0; s < md3->numSurfaces; ++s) {
			int shader = XboxNV2AMD3SurfaceShader(entity, surface);

			if (XboxNV2A_ShaderIsDrawable(shader))
				XboxNV2AAddDrawSurf(surface, NULL, e, shader, 0);
			surface = (const md3Surface_t *)((const byte *)surface + surface->ofsEnd);
		}
	}
}

/* ioq3 sort key order: shader sort, shader, entity, then submission order. */
static int XboxNV2ACompareDrawSurfs(const void *a, const void *b)
{
	const XboxNV2ADrawSurf *x = (const XboxNV2ADrawSurf *)a;
	const XboxNV2ADrawSurf *y = (const XboxNV2ADrawSurf *)b;

	if (x->sort != y->sort)
		return x->sort < y->sort ? -1 : 1;
	if (x->shader != y->shader)
		return x->shader - y->shader;
	if (x->entity != y->entity)
		return x->entity - y->entity;
	if (x->lightmap != y->lightmap)
		return x->lightmap - y->lightmap;
	return x->order - y->order;
}

/* Row-vector matrices, so an ioq3 column-major GL array is used as-is. */
static void XboxNV2AMatrixMultiply(const float *a, const float *b, float *out)
{
	int i, j;

	for (i = 0; i < 4; ++i) {
		for (j = 0; j < 4; ++j) {
			out[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] +
				a[i * 4 + 2] * b[2 * 4 + j] + a[i * 4 + 3] * b[3 * 4 + j];
		}
	}
}

/* ioq3 R_RotateForViewer * R_SetupProjection, then the xgu samples' viewport matrix. */
static void XboxNV2ASetupView(const refdef_t *fd, float zFar, float *worldToScreen)
{
	static const float flip[16] = {
		0, 0, -1, 0,
		-1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 0, 1
	};
	float viewer[16], world[16], projection[16], viewport[16], worldProjection[16];
	float xmax = XBOX_NV2A_ZNEAR * tanf(fd->fov_x * (float)M_PI / 360.0f);
	float ymax = XBOX_NV2A_ZNEAR * tanf(fd->fov_y * (float)M_PI / 360.0f);
	float depth = zFar - XBOX_NV2A_ZNEAR;
	int i;

	memset(viewer, 0, sizeof(viewer));
	for (i = 0; i < 3; ++i) {
		viewer[i * 4 + 0] = fd->viewaxis[0][i];
		viewer[i * 4 + 1] = fd->viewaxis[1][i];
		viewer[i * 4 + 2] = fd->viewaxis[2][i];
	}
	viewer[12] = -DotProduct(fd->vieworg, fd->viewaxis[0]);
	viewer[13] = -DotProduct(fd->vieworg, fd->viewaxis[1]);
	viewer[14] = -DotProduct(fd->vieworg, fd->viewaxis[2]);
	viewer[15] = 1.0f;
	XboxNV2AMatrixMultiply(viewer, flip, world);

	memset(projection, 0, sizeof(projection));
	projection[0] = XBOX_NV2A_ZNEAR / xmax;
	projection[5] = XBOX_NV2A_ZNEAR / ymax;
	projection[10] = -(zFar + XBOX_NV2A_ZNEAR) / depth;
	projection[11] = -1.0f;
	projection[14] = -2.0f * zFar * XBOX_NV2A_ZNEAR / depth;

	memset(viewport, 0, sizeof(viewport));
	viewport[0] = fd->width * 0.5f;
	viewport[5] = fd->height * -0.5f;
	viewport[10] = XBOX_NV2A_ZMAX * 0.5f;
	viewport[12] = fd->x + fd->width * 0.5f;
	viewport[13] = fd->y + fd->height * 0.5f;
	viewport[14] = XBOX_NV2A_ZMAX * 0.5f;
	viewport[15] = 1.0f;

	XboxNV2AMatrixMultiply(world, projection, worldProjection);
	XboxNV2AMatrixMultiply(worldProjection, viewport, worldToScreen);
}

/* ioq3 R_SetupEntityLighting: light grid or the fixed 150 light, +32 ambient, then dlights. */
static void XboxNV2ASetupEntityLighting(const refdef_t *fd, const refEntity_t *entity)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	vec3_t lightOrigin, direction, lightDir;
	float d;
	int i;

	VectorCopy((entity->renderfx & RF_LIGHTING_ORIGIN) ? entity->lightingOrigin :
		entity->origin, lightOrigin);
	if ((fd->rdflags & RDF_NOWORLDMODEL) || !XboxNV2AWorld_LightGrid(lightOrigin,
		tess->ambientLight, tess->directedLight, direction)) {
		VectorSet(tess->ambientLight, 150.0f, 150.0f, 150.0f);
		VectorSet(tess->directedLight, 150.0f, 150.0f, 150.0f);
		XboxNV2AWorld_SunDirection(direction);
	}
	for (i = 0; i < 3; ++i)
		tess->ambientLight[i] += 32.0f;

	d = VectorLength(tess->directedLight);
	VectorScale(direction, d, lightDir);
	for (i = xboxNV2ASceneFirstDlight; i < xboxNV2ADlightCount; ++i) {
		const XboxNV2ADlight *light = &xboxNV2ADlights[i];
		float power = XBOX_NV2A_DLIGHT_AT_RADIUS * light->radius * light->radius;
		vec3_t toLight;

		VectorSubtract(light->origin, lightOrigin, toLight);
		d = VectorNormalize(toLight);
		if (d < XBOX_NV2A_DLIGHT_MINIMUM_RADIUS)
			d = XBOX_NV2A_DLIGHT_MINIMUM_RADIUS;
		d = power / (d * d);
		VectorMA(tess->directedLight, d, light->color, tess->directedLight);
		VectorMA(lightDir, d, toLight, lightDir);
	}
	for (i = 0; i < 3; ++i) {
		if (tess->ambientLight[i] > 255.0f)
			tess->ambientLight[i] = 255.0f;
	}
	VectorNormalize(lightDir);
	for (i = 0; i < 3; ++i)
		tess->lightDir[i] = DotProduct(lightDir, entity->axis[i]);
}

/* ioq3 R_RotateForEntity; the world entity keeps the plain view transform and no light. */
static void XboxNV2ASetupEntity(const refdef_t *fd, int entityIndex,
	const float *worldToScreen)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const refEntity_t *entity;
	float model[16];
	float modelToScreen[16];
	vec3_t delta;
	float axisLength = 1.0f;
	int i;

	if (entityIndex == XBOX_NV2A_WORLD_ENTITY) {
		XboxNV2ASetTransform((const XguVec4 *)worldToScreen, qfalse);
		VectorCopy(fd->vieworg, tess->viewOrigin);
		VectorClear(tess->ambientLight);
		VectorClear(tess->directedLight);
		VectorClear(tess->lightDir);
		return;
	}
	entity = &xboxNV2ASceneEntities[entityIndex];
	memset(model, 0, sizeof(model));
	for (i = 0; i < 3; ++i) {
		model[0 * 4 + i] = entity->axis[0][i];
		model[1 * 4 + i] = entity->axis[1][i];
		model[2 * 4 + i] = entity->axis[2][i];
		model[3 * 4 + i] = entity->origin[i];
	}
	model[15] = 1.0f;
	XboxNV2AMatrixMultiply(model, worldToScreen, modelToScreen);
	XboxNV2ASetTransform((const XguVec4 *)modelToScreen, qfalse);

	if (entity->nonNormalizedAxes) {
		axisLength = VectorLength(entity->axis[0]);
		axisLength = axisLength ? 1.0f / axisLength : 0.0f;
	}
	VectorSubtract(fd->vieworg, entity->origin, delta);
	for (i = 0; i < 3; ++i)
		tess->viewOrigin[i] = DotProduct(delta, entity->axis[i]) * axisLength;
	XboxNV2ASetupEntityLighting(fd, entity);
}

/* Fills the tess with one MD3 surface in model space; the GPU applies the entity matrix. */
static void XboxNV2ATessSurface(const refEntity_t *entity, const md3Surface_t *surface)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const md3St_t *st = (const md3St_t *)((const byte *)surface + surface->ofsSt);
	const md3Triangle_t *triangles = (const md3Triangle_t *)((const byte *)surface +
		surface->ofsTriangles);
	int i;

	XboxNV2AModel_LerpSurface(surface, entity->frame, entity->oldframe,
		entity->backlerp, tess->xyz, tess->normal);
	for (i = 0; i < surface->numVerts; ++i) {
		tess->st[i][0] = st[i].st[0];
		tess->st[i][1] = st[i].st[1];
		memset(tess->color[i], 0xff, 4);
	}
	for (i = 0; i < surface->numTriangles; ++i) {
		tess->indexes[i * 3 + 0] = (unsigned short)triangles[i].indexes[0];
		tess->indexes[i * 3 + 1] = (unsigned short)triangles[i].indexes[1];
		tess->indexes[i * 3 + 2] = (unsigned short)triangles[i].indexes[2];
	}
	tess->numVerts = surface->numVerts;
	tess->numIndexes = surface->numTriangles * 3;
}

/* Appends one world or brush surface; its vertices are in the active transform's space. */
static void XboxNV2ATessWorldSurface(const xboxNV2AWorldSurface_t *surface)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int base = tess->numVerts;
	int i;

	for (i = 0; i < surface->numVerts; ++i) {
		const xboxNV2AWorldVert_t *in = &surface->verts[i];
		int v = base + i;

		VectorCopy(in->xyz, tess->xyz[v]);
		VectorCopy(in->normal, tess->normal[v]);
		tess->st[v][0] = in->st[0];
		tess->st[v][1] = in->st[1];
		tess->lightSt[v][0] = in->lightSt[0];
		tess->lightSt[v][1] = in->lightSt[1];
		memcpy(tess->color[v], in->color, 4);
	}
	for (i = 0; i < surface->numIndexes; ++i)
		tess->indexes[tess->numIndexes + i] = (unsigned short)(base + surface->indexes[i]);
	tess->numVerts += surface->numVerts;
	tess->numIndexes += surface->numIndexes;
}

/* ioq3 RB_RenderDrawSurfList: world surfaces with equal shader, entity and lightmap batch. */
static void XboxNV2ADrawSurfaces(const refdef_t *fd, const float *worldToScreen)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int currentEntity = XBOX_NV2A_WORLD_ENTITY - 1;
	int i;

	for (i = 0; i < xboxNV2ADrawSurfCount; ++i) {
		const XboxNV2ADrawSurf *drawSurf = &xboxNV2ADrawSurfs[i];
		const refEntity_t *entity = drawSurf->entity == XBOX_NV2A_WORLD_ENTITY ? NULL :
			&xboxNV2ASceneEntities[drawSurf->entity];

		if (drawSurf->entity != currentEntity) {
			XboxNV2AEndSurface();
			XboxNV2ASetupEntity(fd, drawSurf->entity, worldToScreen);
			currentEntity = drawSurf->entity;
		}
		if (drawSurf->md3 || tess->shader != drawSurf->shader ||
			tess->lightmap != drawSurf->lightmap ||
			tess->numVerts + drawSurf->world->numVerts > XBOX_NV2A_TESS_VERTS ||
			tess->numIndexes + drawSurf->world->numIndexes > XBOX_NV2A_TESS_INDEXES) {
			XboxNV2AEndSurface();
			tess->shader = drawSurf->shader;
			tess->lightmap = drawSurf->lightmap;
			tess->is3D = qtrue;
			tess->entity = entity;
			tess->shaderTime = fd->time * 0.001 - (entity ? entity->shaderTime : 0.0f);
		}
		if (drawSurf->md3) {
			XboxNV2ATessSurface(entity, drawSurf->md3);
			XboxNV2AEndSurface();
		} else {
			XboxNV2ATessWorldSurface(drawSurf->world);
		}
	}
	XboxNV2AEndSurface();
}

void XboxNV2A_RenderScene(const refdef_t *fd)
{
	float worldToScreen[16];
	float zFar = XBOX_NV2A_ZFAR;
	int worldSurfaces;
	int signature;
	int x, y, w, h;
	uint32_t *p;

	if (!xboxNV2AInFrame || !fd)
		return;
	XboxNV2AEndSurface();
	xboxNV2ADrawSurfCount = 0;
	if (!(fd->rdflags & RDF_NOWORLDMODEL) && XboxNV2AWorld_Loaded())
		zFar = XboxNV2AWorld_AddSurfaces(fd);
	worldSurfaces = xboxNV2ADrawSurfCount;
	XboxNV2AAddEntitySurfaces();
	/* One line per new kind of scene: menu model, lit player model, world view. */
	signature = (worldSurfaces > 0) | ((xboxNV2ADlightCount - xboxNV2ASceneFirstDlight) << 1);
	if (xboxNV2ASceneCount < XBOX_NV2A_TRACE_SCENES && xboxNV2ADrawSurfCount > 0 &&
		signature != xboxNV2ALastSceneSignature) {
		xboxNV2ASceneCount++;
		xboxNV2ALastSceneSignature = signature;
		Sys_XboxLog("Xbox NV2A: frame %u scene %dx%d world=%d entities=%d dlights=%d "
			"surfaces=%d zfar=%d\n", xboxNV2AFrameCount, fd->width, fd->height,
			worldSurfaces, xboxNV2ASceneEntityCount - xboxNV2ASceneFirstEntity,
			xboxNV2ADlightCount - xboxNV2ASceneFirstDlight, xboxNV2ADrawSurfCount, (int)zFar);
	}
	if (xboxNV2ADrawSurfCount > 0) {
		qsort(xboxNV2ADrawSurfs, (size_t)xboxNV2ADrawSurfCount, sizeof(xboxNV2ADrawSurfs[0]),
			XboxNV2ACompareDrawSurfs);
		XboxNV2ASetupView(fd, zFar, worldToScreen);

		/* ioq3 RB_BeginDrawingView clears depth inside the view before drawing it. */
		x = fd->x < 0 ? 0 : fd->x;
		y = fd->y < 0 ? 0 : fd->y;
		w = (fd->x + fd->width > xboxNV2AWidth ? xboxNV2AWidth : fd->x + fd->width) - x;
		h = (fd->y + fd->height > xboxNV2AHeight ? xboxNV2AHeight : fd->y + fd->height) - y;
		if (w > 0 && h > 0) {
			XboxNV2AReserve(16);
			xgux_set_clear_rect((unsigned int)x, (unsigned int)y, (unsigned int)w,
				(unsigned int)h);
			p = pb_begin();
			p = xgu_set_zstencil_clear_value(p, 0xffffff00);
			p = xgu_clear_surface(p, XGU_CLEAR_Z | XGU_CLEAR_STENCIL);
			pb_end(p);
		}
		XboxNV2ADrawSurfaces(fd, worldToScreen);
	}
	xboxNV2ASceneFirstEntity = xboxNV2ASceneEntityCount;
	xboxNV2ASceneFirstDlight = xboxNV2ADlightCount;
}
