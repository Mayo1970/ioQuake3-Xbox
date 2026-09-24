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

/* renderercommon/tr_noise.c; tr_common.h would pull SDL_opengl.h into this file. */
float R_NoiseGet4f(float x, float y, float z, double t);
void R_NoiseInit(void);

/* 28 bytes; the colour is R, G, B, A bytes, read as NV097 UB_OGL like pbgl and nxdk-gles11. */
typedef struct {
	float position[3];
	byte color[4];
	float texcoord[2];
	float texcoord1[2];
} XboxNV2AColoredVertex;

typedef struct {
	char name[MAX_QPATH];
	void *memory;
	int width;
	int height;
	XguTexFormatColor format;
	/* Mip levels stored one after another from memory; 0 counts as 1. */
	int levels;
} XboxNV2AImage;

typedef struct {
	char name[MAX_QPATH];
	int flavor;
	float sort;
	int cull;
	qboolean polygonOffset;
	qboolean isSky;
	int skyBox[XBOX_NV2A_SKY_SIDES];
	float cloudHeight;
	qboolean isFog;
	vec3_t fogColor;
	float fogDepth;
	int fogPass;
	float portalRange;
	qboolean noDlight;
	int numDeforms;
	xboxNV2ADeform_t *deforms;
	int numStages;
	xboxNV2AStage_t *stages;
	/* Stages 0 and 1 draw as one multitextured pass. */
	qboolean collapsed;
	/* One pass whose colours and texcoords are plain world vertex data. */
	qboolean fastWorld;
} XboxNV2AShader;

/* ioq3 tess: one batch of CPU vertices with one shader, drawn once per stage. */
typedef struct {
	int shader;
	int lightmap;
	qboolean is3D;
	/* The sky never writes depth; its z sits just in front of the cleared far value. */
	qboolean isSky;
	int fogIndex;
	/* dlighted batches stay apart; dlightBits ORs the bits of the surfaces in it. */
	qboolean dlighted;
	unsigned int dlightBits;
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
	/* Texture unit 1 coordinates of a collapsed second stage, when multitexture is set. */
	float stageSt1[XBOX_NV2A_TESS_VERTS][2];
	qboolean multitexture;
	byte stageColor[XBOX_NV2A_TESS_VERTS][4];
	/* Signed distances in front of the near plane and a portal view's clip plane. */
	float nearDist[XBOX_NV2A_TESS_VERTS];
	float portalDist[XBOX_NV2A_TESS_VERTS];
	qboolean clip;
	/* The triangles one dlight reaches, for its pass. */
	unsigned short dlightIndexes[XBOX_NV2A_TESS_INDEXES];
	/* Stream indexes of one draw; a clipped triangle can become three. */
	unsigned short drawIndexes[3 * XBOX_NV2A_TESS_INDEXES];
	/* fastWorld surfaces no plane cuts; they skip the arrays above and stream as they are. */
	const xboxNV2AWorldSurface_t *fast[XBOX_NV2A_TESS_VERTS];
	int numFast;
	int fastVerts;
	int fastIndexes;
} XboxNV2ATess;

/* ioq3 srfPoly_t; verts points into xboxNV2APolyVerts. */
typedef struct {
	int shader;
	int numVerts;
	const polyVert_t *verts;
} XboxNV2APoly;

/* One surface of the current scene; md3, world and poly are all NULL for a sprite, beam or rail. */
typedef struct {
	const md3Surface_t *md3;
	const xboxNV2AWorldSurface_t *world;
	const XboxNV2APoly *poly;
	int entity;
	int shader;
	int lightmap;
	int fogIndex;
	qboolean dlighted;
	float sort;
	int order;
} XboxNV2ADrawSurf;

/* One visible sky shader of the current view and the box area its surfaces cover. */
typedef struct {
	int shader;
	xboxNV2ASkyBounds_t bounds;
} XboxNV2ASky;

#define XBOX_NV2A_MAX_IMAGES 512
/* cl_cin.c MAX_VIDEO_HANDLES; each handle's scratch texture follows the pool images. */
#define XBOX_NV2A_MAX_CINEMATICS 16
#define XBOX_NV2A_MAX_SHADERS 1024
#define XBOX_NV2A_TEXTURE_POOL_BYTES (6u * 1024u * 1024u)
#define XBOX_NV2A_TEXTURE_ALIGN 128u
/* Highest physical address the xgu samples allow for GPU memory. */
#define XBOX_NV2A_MAX_RAM 0x03FFAFFF
#define XBOX_NV2A_TRIANGLE_VERTS 3
/* Holds a full tess plus 5 clipped corners for each of its 2000 triangles, so one draw always fits. */
#define XBOX_NV2A_MAX_VERTS 12288u
#define XBOX_NV2A_VERTEX_UB_OGL ((XguVertexArrayType)NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_UB_OGL)
#define XBOX_NV2A_CLEAR_COLOR 0xff101820
#define XBOX_NV2A_ZMAX ((float)0xFFFFFF)
/* Release pbkit does not check for overflow; restart well below its 512 KiB limit. */
#define XBOX_NV2A_PUSH_LIMIT_DWORDS (96u * 1024u)
#define XBOX_NV2A_STATE_DWORDS 64u
/* NV097_SET_TEXTURE_FILTER min/mag: TENT_LOD0 is GL_LINEAR, TENT_NEARESTLOD ioq3's default
   GL_LINEAR_MIPMAP_NEAREST; the LOD clamps are 4.8 fixed point. */
#define XBOX_NV2A_FILTER_LINEAR 2
#define XBOX_NV2A_FILTER_MIPMAP 4
#define XBOX_NV2A_MAX_LOD_CLAMP 4095
/* ioq3 MAX_REFENTITIES: every smoke puff, explosion and trail is an entity too. */
#define XBOX_NV2A_MAX_SCENE_ENTITIES MAX_REFENTITIES
#define XBOX_NV2A_MAX_SCENE_SURFACES 4096
/* ioq3 R_SetupEntityLighting falloff constants. */
#define XBOX_NV2A_DLIGHT_AT_RADIUS 16.0f
#define XBOX_NV2A_DLIGHT_MINIMUM_RADIUS 16.0f
/* ioq3 r_znear default; RDF_NOWORLDMODEL scenes use a 2048 far plane. */
#define XBOX_NV2A_ZNEAR 4.0f
#define XBOX_NV2A_ZFAR 2048.0f
#define XBOX_NV2A_FUNCTABLE_SIZE 1024
#define XBOX_NV2A_FUNCTABLE_SIZE2 10
#define XBOX_NV2A_FUNCTABLE_MASK (XBOX_NV2A_FUNCTABLE_SIZE - 1)
/* q3dm10 shows two sky shaders; ioq3 draws each one it sees. */
#define XBOX_NV2A_MAX_SKIES 4
/* ioq3 MAX_POLYS and MAX_POLYVERTS, the r_maxpolys and r_maxpolyverts defaults. */
#define XBOX_NV2A_MAX_POLYS 600
#define XBOX_NV2A_MAX_POLYVERTS 3000
/* ioq3 r_railWidth, r_railCoreWidth and r_railSegmentLength defaults. */
#define XBOX_NV2A_RAIL_WIDTH 16
#define XBOX_NV2A_RAIL_CORE_WIDTH 6
#define XBOX_NV2A_RAIL_SEGMENT_LENGTH 32.0f
#define XBOX_NV2A_BEAM_SEGS 6
/* ioq3 r_offsetfactor and r_offsetunits; nxdk-gles11 and pbgl pass GL's values to the NV2A as-is. */
#define XBOX_NV2A_POLYGON_OFFSET_FACTOR (-1.0f)
#define XBOX_NV2A_POLYGON_OFFSET_UNITS (-2.0f)
/* ioq3 FOG_S, FOG_T and FOG_TABLE_SIZE. */
#define XBOX_NV2A_FOG_S 256
#define XBOX_NV2A_FOG_T 32
#define XBOX_NV2A_FOG_TABLE_SIZE 256
/* ioq3 depthRange(1, 1) for the sky; 64 units stay above float rounding near 2^24. */
#define XBOX_NV2A_SKY_DEPTH (XBOX_NV2A_ZMAX - 64.0f)
/* A triangle clipped by the near and portal planes has at most 5 corners. */
#define XBOX_NV2A_CLIP_VERTS 6
/* ioq3 RF_DEPTHHACK glDepthRange(0, 0.3). */
#define XBOX_NV2A_DEPTHHACK_RANGE 0.3f
/* ioq3 DLIGHT_SIZE: the dlight falloff image is 16x16. */
#define XBOX_NV2A_DLIGHT_SIZE 16

static XboxNV2AColoredVertex xboxNV2ATriangle[XBOX_NV2A_TRIANGLE_VERTS] = {
	{{0.0f, 0.0f, 1.0f}, {255, 20, 10, 255}, {0.0f, 0.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {20, 255, 41, 255}, {0.0f, 0.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {20, 61, 255, 255}, {0.0f, 0.0f}, {0.0f, 0.0f}},
};

/* Vertices stream into contiguous memory; the GPU reads them with DRAW_ARRAYS. */
static XboxNV2AColoredVertex *xboxNV2AVertexMemory;
static unsigned int xboxNV2AVertexUsed;
static XboxNV2ATess xboxNV2ATess;
static byte *xboxNV2ATexturePool;
static size_t xboxNV2ATexturePoolUsed;
static XboxNV2AImage xboxNV2AImages[XBOX_NV2A_MAX_IMAGES + XBOX_NV2A_MAX_CINEMATICS];
static unsigned int xboxNV2AImageCount;
static unsigned int xboxNV2ADxtImageCount;
static unsigned int xboxNV2ACollapsedCount;
static XboxNV2AShader xboxNV2AShaders[XBOX_NV2A_MAX_SHADERS];
static unsigned int xboxNV2AShaderCount;
/* Unit 0 holds the stage image; unit 1 a collapsed second stage's image, or white. */
static int xboxNV2ABoundImage[2];
static qboolean xboxNV2ABoundClamp[2];
static unsigned int xboxNV2ABoundSrcBlend;
static unsigned int xboxNV2ABoundDstBlend;
static int xboxNV2ABoundAlphaFunc;
static int xboxNV2ABoundDepthTest;
static int xboxNV2ABoundDepthWrite;
static int xboxNV2ABoundDepthEqual;
static int xboxNV2ABoundCull;
static int xboxNV2ABoundPolygonOffset;
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
static xboxNV2ADlight_t xboxNV2ADlights[XBOX_NV2A_MAX_DLIGHTS];
static int xboxNV2ADlightCount;
static int xboxNV2ASceneFirstDlight;
static XboxNV2ADrawSurf xboxNV2ADrawSurfs[XBOX_NV2A_MAX_SCENE_SURFACES];
static int xboxNV2ADrawSurfCount;
static qboolean xboxNV2ADrawSurfOverflow;
static XboxNV2ASky xboxNV2ASkies[XBOX_NV2A_MAX_SKIES];
static int xboxNV2ASkyCount;
static vec3_t xboxNV2ASkyOrigin;
/* ioq3 sky_texorder: box side i shows skyBox[order[i]]. */
static const int xboxNV2ASkyTexOrder[XBOX_NV2A_SKY_SIDES] = {0, 2, 1, 3, 4, 5};
static XboxNV2APoly xboxNV2APolys[XBOX_NV2A_MAX_POLYS];
static polyVert_t xboxNV2APolyVerts[XBOX_NV2A_MAX_POLYVERTS];
static int xboxNV2APolyCount;
static int xboxNV2APolyVertCount;
static int xboxNV2ASceneFirstPoly;
static int xboxNV2AFogImage;
static float xboxNV2AFogTable[XBOX_NV2A_FOG_TABLE_SIZE];
static int xboxNV2ADlightImage;
static cvar_t *xboxNV2AFastSky;
static cvar_t *xboxNV2ADynamicLight;
static cvar_t *xboxNV2AMultitexture;
static cvar_t *xboxNV2ASwapInterval;
/* pbkit vblank counts at which the last two queued flips show; pbkit shows one per vblank. */
static DWORD xboxNV2AFlipShow[2];
static unsigned int xboxNV2AFlipsQueued;
/* The view being drawn: the scene's own, or a mirror/portal view drawn just before it. */
static qboolean xboxNV2AViewIsPortal;
static qboolean xboxNV2AViewIsMirror;
static vec3_t xboxNV2AViewOrigin;
static vec3_t xboxNV2AViewAxis[3];
static int xboxNV2AViewTime;
/* Portal clip plane in world space and in the active transform's space; n.p >= d stays. */
static float xboxNV2APortalPlane[4];
static float xboxNV2APortalPlaneLocal[4];

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
/* Bit per generated surface type already logged; bit 31 is the scene poly. */
static unsigned int xboxNV2ATracedTypes;
#define XBOX_NV2A_TRACE_POLY 31
/* Deform types use bits 8 to 13. */
#define XBOX_NV2A_TRACE_DEFORM 8
#define XBOX_NV2A_TRACE_DEPTHHACK 14
#define XBOX_NV2A_TRACE_DLIGHT 15

/* DIAGNOSTIC: sums over one heartbeat for the hardware log; times in performance counter ticks. */
typedef struct {
	unsigned int frames;
	unsigned int draws;
	unsigned int verts;
	unsigned int fastVerts;
	unsigned int indexes;
	unsigned int streamWraps;
	unsigned int pushResets;
	LONGLONG frameTicks;
	LONGLONG sceneTicks;
	LONGLONG vblankTicks;
	LONGLONG endWaitTicks;
	LONGLONG midWaitTicks;
	LONGLONG lastBegin;
	LONGLONG sceneStart;
} XboxNV2APerfCounters;
static XboxNV2APerfCounters xboxNV2APerf;

static LONGLONG XboxNV2ATicks(void)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return now.QuadPart;
}

/* Returns the ticks spent waiting. */
static LONGLONG XboxNV2AWaitIdle(const char *where)
{
	DWORD start = GetTickCount();
	LONGLONG ticks = XboxNV2ATicks();

	while (pb_busy()) {
		if (GetTickCount() - start > XBOX_NV2A_GPU_TIMEOUT_MS)
			Sys_Error("Xbox NV2A: GPU hang in %s, frame %u, last shader %s",
				where, xboxNV2AFrameCount, xboxNV2ALastShaderName);
	}
	return XboxNV2ATicks() - ticks;
}

/* DIAGNOSTIC: per-frame averages in microseconds; nxdk printf has no floats. */
static void XboxNV2ALogPerf(void)
{
	XboxNV2APerfCounters *c = &xboxNV2APerf;
	LARGE_INTEGER frequency;
	double us;
	unsigned int frames = c->frames ? c->frames : 1;

	QueryPerformanceFrequency(&frequency);
	us = 1000000.0 / ((double)frequency.QuadPart * frames);
	Sys_XboxLog("Xbox perf: %u frames, per frame us: total=%u scene=%u gpuwait=%u vblank=%u "
		"midwait=%u; per frame: draws=%u verts=%u fastverts=%u indexes=%u; wraps=%u "
		"pushresets=%u\n",
		c->frames, (unsigned int)(c->frameTicks * us), (unsigned int)(c->sceneTicks * us),
		(unsigned int)(c->endWaitTicks * us), (unsigned int)(c->vblankTicks * us),
		(unsigned int)(c->midWaitTicks * us), c->draws / frames, c->verts / frames,
		c->fastVerts / frames, c->indexes / frames, c->streamWraps, c->pushResets);
	{
		LONGLONG lastBegin = c->lastBegin;

		memset(c, 0, sizeof(*c));
		c->lastBegin = lastBegin;
	}
}

/* Callers pass an upper bound; the pushbuffer restarts once the GPU is idle. */
static void XboxNV2AReserve(unsigned int dwords)
{
	if (xboxNV2APushedDwords + dwords > XBOX_NV2A_PUSH_LIMIT_DWORDS) {
		xboxNV2APerf.midWaitTicks += XboxNV2AWaitIdle("reserve");
		xboxNV2APerf.pushResets++;
		pb_reset();
		xboxNV2APushedDwords = 0;
	}
	xboxNV2APushedDwords += dwords;
}

static void XboxNV2AInvalidateState(void)
{
	xboxNV2ABoundImage[0] = xboxNV2ABoundImage[1] = -1;
	xboxNV2ABoundClamp[0] = xboxNV2ABoundClamp[1] = qfalse;
	xboxNV2ABoundSrcBlend = ~0u;
	xboxNV2ABoundDstBlend = ~0u;
	xboxNV2ABoundAlphaFunc = -1;
	xboxNV2ABoundDepthTest = -1;
	xboxNV2ABoundDepthWrite = -1;
	xboxNV2ABoundDepthEqual = -1;
	xboxNV2ABoundCull = -1;
	xboxNV2ABoundPolygonOffset = -1;
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
	for (i = 0; i < XBOX_NV2A_FOG_TABLE_SIZE; ++i)
		xboxNV2AFogTable[i] = sqrtf((float)i / (XBOX_NV2A_FOG_TABLE_SIZE - 1));
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

static uint32_t *XboxNV2ABindTexture(uint32_t *p, unsigned int unit, int imageIndex,
	qboolean clamp)
{
	const XboxNV2AImage *image = &xboxNV2AImages[imageIndex];
	XguTextureAddress address = clamp ? XGU_CLAMP_TO_EDGE : XGU_WRAP;
	int levels = image->levels > 1 ? image->levels : 1;

	if (imageIndex == xboxNV2ABoundImage[unit] && clamp == xboxNV2ABoundClamp[unit])
		return p;
	p = xgu_set_texture_offset(p, unit, (void *)((uint32_t)image->memory & 0x03ffffff));
	p = xgu_set_texture_format(p, unit, 2, false, XGU_SOURCE_COLOR, 2, image->format,
		(uint8_t)levels, XboxNV2ALog2(image->width), XboxNV2ALog2(image->height), 0);
	/* U, V and P must all hold a valid mode; 0 raises a GPU invalid-data error. */
	p = xgu_set_texture_address(p, unit, address, false, address, false, address, false,
		false);
	/* The format's level count limits the LOD, so the clamp stays wide open, as in pbgl. */
	p = xgu_set_texture_control0(p, unit, true, 0, XBOX_NV2A_MAX_LOD_CLAMP);
	p = xgu_set_texture_filter(p, unit, 0, XGU_TEXTURE_CONVOLUTION_QUINCUNX,
		levels > 1 ? XBOX_NV2A_FILTER_MIPMAP : XBOX_NV2A_FILTER_LINEAR, XBOX_NV2A_FILTER_LINEAR,
		false, false, false, false);
	xboxNV2ABoundImage[unit] = imageIndex;
	xboxNV2ABoundClamp[unit] = clamp;
	return p;
}

/* 2D stages never touch depth or culling; image1 is a collapsed second stage's, else white. */
static void XboxNV2AApplyStageState(const xboxNV2AStage_t *stage, int imageIndex,
	int image1, qboolean clamp1, qboolean is3D, qboolean depthTest, int cull,
	qboolean polygonOffset)
{
	int depthWrite = depthTest && stage->depthWrite;
	int depthEqual = depthTest && stage->depthEqual;
	int offset = depthTest && polygonOffset;
	uint32_t *p;

	if (!is3D)
		cull = XBOX_NV2A_CULL_NONE;

	XboxNV2AReserve(XBOX_NV2A_STATE_DWORDS);
	p = pb_begin();
	p = XboxNV2ABindTexture(p, 0, imageIndex, stage->clamp);
	p = XboxNV2ABindTexture(p, 1, image1, clamp1);
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
	if ((int)depthTest != xboxNV2ABoundDepthTest) {
		p = xgu_set_depth_test_enable(p, depthTest);
		xboxNV2ABoundDepthTest = depthTest;
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
	if (offset != xboxNV2ABoundPolygonOffset) {
		p = push_command_boolean(p, NV097_SET_POLY_OFFSET_FILL_ENABLE, offset);
		xboxNV2ABoundPolygonOffset = offset;
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

/* ioq3 R_MipMap box filter; out may be in, as each texel is written after its inputs are read. */
static void XboxNV2AHalve(const byte *in, int width, int height, byte *out)
{
	int outWidth = width > 1 ? width / 2 : 1;
	int outHeight = height > 1 ? height / 2 : 1;
	int stepX = width > 1 ? 1 : 0;
	int stepY = height > 1 ? 1 : 0;
	int x, y, k;

	for (y = 0; y < outHeight; ++y) {
		const byte *row0 = in + (size_t)(y * (stepY + 1)) * width * 4;
		const byte *row1 = row0 + (size_t)stepY * width * 4;

		for (x = 0; x < outWidth; ++x) {
			int x0 = x * (stepX + 1) * 4;
			int x1 = x0 + stepX * 4;
			byte *texel = out + ((size_t)y * outWidth + x) * 4;

			for (k = 0; k < 4; ++k)
				texel[k] = (byte)((row0[x0 + k] + row0[x1 + k] + row1[x0 + k] +
					row1[x1 + k] + 2) >> 2);
		}
	}
}

void XboxNV2A_HalveImage(byte *pic, int *width, int *height)
{
	XboxNV2AHalve(pic, *width, *height, pic);
	*width = *width > 1 ? *width / 2 : 1;
	*height = *height > 1 ? *height / 2 : 1;
}

static size_t XboxNV2ALevelBytes(int width, int height, int storage)
{
	if (storage == XBOX_NV2A_IMAGE_DXT1)
		return (size_t)width * (size_t)height / 2;
	return (size_t)width * (size_t)height * (storage == XBOX_NV2A_IMAGE_32BIT ? 4 : 2);
}

static void XboxNV2AUploadLevel(byte *memory, const byte *rgba, int width, int height,
	int storage, XguTexFormatColor format)
{
	int x, y;

	if (storage == XBOX_NV2A_IMAGE_DXT1) {
		XboxNV2ADxt_Compress(rgba, width, height, memory);
		return;
	}
	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			const byte *texel = rgba + ((size_t)y * width + x) * 4;
			unsigned int swizzled = XboxNV2ASwizzledOffset((unsigned int)x, (unsigned int)y,
				(unsigned int)width, (unsigned int)height);

			if (storage == XBOX_NV2A_IMAGE_32BIT)
				((uint32_t *)memory)[swizzled] = ((uint32_t)texel[3] << 24) |
					((uint32_t)texel[0] << 16) | ((uint32_t)texel[1] << 8) | texel[2];
			else
				((uint16_t *)memory)[swizzled] = XboxNV2APackTexel(texel, format);
		}
	}
}

/* Levels follow each other without padding, as in pbgl; DXT1 stops before a side drops below 4. */
static int XboxNV2AAddImage(const char *name, int width, int height,
	const byte *rgba, int storage, qboolean mipmap)
{
	XboxNV2AImage *image;
	XguTexFormatColor format;
	qboolean alpha = qfalse;
	size_t textureSize = 0;
	size_t offset;
	size_t pixel;
	byte *scratch = NULL;
	int levels = 0;
	int w = width, h = height;

	for (pixel = 0; pixel < (size_t)width * (size_t)height; ++pixel) {
		if (rgba[pixel * 4 + 3] != 255) {
			alpha = qtrue;
			break;
		}
	}
	/* DXT1's 1-bit alpha would turn texels black under additive or filter blends. */
	if (storage == XBOX_NV2A_IMAGE_DXT1 && (alpha || width < 4 || height < 4))
		storage = XBOX_NV2A_IMAGE_16BIT;
	if (storage == XBOX_NV2A_IMAGE_DXT1)
		format = XGU_TEXTURE_FORMAT_DXT1;
	else if (storage == XBOX_NV2A_IMAGE_32BIT)
		format = alpha ? XGU_TEXTURE_FORMAT_A8R8G8B8_SWIZZLED :
			XGU_TEXTURE_FORMAT_X8R8G8B8_SWIZZLED;
	else
		format = alpha ? XGU_TEXTURE_FORMAT_A4R4G4B4_SWIZZLED :
			XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED;
	for (;;) {
		textureSize += XboxNV2ALevelBytes(w, h, storage);
		levels++;
		if (!mipmap || (w == 1 && h == 1) ||
			(storage == XBOX_NV2A_IMAGE_DXT1 && (w < 8 || h < 8)))
			break;
		w = w > 1 ? w / 2 : 1;
		h = h > 1 ? h / 2 : 1;
	}
	offset = (xboxNV2ATexturePoolUsed + XBOX_NV2A_TEXTURE_ALIGN - 1) &
		~(size_t)(XBOX_NV2A_TEXTURE_ALIGN - 1);
	if (xboxNV2AImageCount >= XBOX_NV2A_MAX_IMAGES ||
		offset + textureSize > XBOX_NV2A_TEXTURE_POOL_BYTES) {
		Sys_XboxLog("Xbox NV2A: texture pool full, skipped %s\n", name);
		return 0;
	}
	if (levels > 1) {
		scratch = (byte *)malloc((size_t)(width > 1 ? width / 2 : 1) *
			(size_t)(height > 1 ? height / 2 : 1) * 4);
		if (!scratch)
			levels = 1;
	}

	image = &xboxNV2AImages[xboxNV2AImageCount];
	image->memory = xboxNV2ATexturePool + offset;
	image->width = width;
	image->height = height;
	image->format = format;
	image->levels = levels;
	Q_strncpyz(image->name, name, sizeof(image->name));
	XboxNV2AUploadLevel(image->memory, rgba, width, height, storage, format);
	if (levels > 1) {
		byte *memory = (byte *)image->memory + XboxNV2ALevelBytes(width, height, storage);
		int level;

		/* Level 1 comes from the caller's texels, the rest from the scratch copy in place. */
		w = width;
		h = height;
		for (level = 1; level < levels; ++level) {
			XboxNV2AHalve(level == 1 ? rgba : scratch, w, h, scratch);
			w = w > 1 ? w / 2 : 1;
			h = h > 1 ? h / 2 : 1;
			XboxNV2AUploadLevel(memory, scratch, w, h, storage, format);
			memory += XboxNV2ALevelBytes(w, h, storage);
		}
		free(scratch);
	}
	if (storage == XBOX_NV2A_IMAGE_DXT1)
		xboxNV2ADxtImageCount++;
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
	const byte *rgba, int storage, qboolean mipmap)
{
	int index;

	if (!name || !*name || !rgba || !xboxNV2AInitialized ||
		!XboxNV2AIsPowerOfTwo(width) || !XboxNV2AIsPowerOfTwo(height) ||
		width > XBOX_NV2A_MAX_TEXTURE_SIZE || height > XBOX_NV2A_MAX_TEXTURE_SIZE)
		return 0;
	index = XboxNV2A_FindImage(name);
	return index ? index : XboxNV2AAddImage(name, width, height, rgba, storage, mipmap);
}

/* ioq3 keeps stageless shaders only for skies (here: with an outer box) and fog volumes. */
static qboolean XboxNV2AShaderUsable(const XboxNV2AShader *shader)
{
	return shader->numStages > 0 || shader->isFog || (shader->isSky && shader->skyBox[0] > 0);
}

static qboolean XboxNV2AMultipliesDest(const xboxNV2AStage_t *stage)
{
	return (stage->srcBlend == GL_DST_COLOR && stage->dstBlend == GL_ZERO) ||
		(stage->srcBlend == GL_ZERO && stage->dstBlend == GL_SRC_COLOR);
}

/* ioq3 CollapseMultitexture, GL_MODULATE rows only. The pair uses stage 0's colours,
   so stage 1 must be white; ioq3 drops its rgbGen instead. */
static qboolean XboxNV2ACanCollapse(const XboxNV2AShader *shader)
{
	const xboxNV2AStage_t *a = &shader->stages[0];
	const xboxNV2AStage_t *b = &shader->stages[1];

	if (!xboxNV2AMultitexture->integer || shader->numStages < 2 || shader->isSky)
		return qfalse;
	if (a->isLightmap && b->isLightmap)
		return qfalse;
	if (a->alphaFunc != b->alphaFunc || a->depthEqual != b->depthEqual)
		return qfalse;
	if (!(a->srcBlend == GL_ONE && a->dstBlend == GL_ZERO) && !XboxNV2AMultipliesDest(a))
		return qfalse;
	if (!XboxNV2AMultipliesDest(b))
		return qfalse;
	if (b->rgbGen != XBOX_NV2A_RGBGEN_IDENTITY && b->rgbGen != XBOX_NV2A_RGBGEN_IDENTITY_LIGHTING)
		return qfalse;
	if (b->alphaGen != XBOX_NV2A_ALPHAGEN_IDENTITY || a->alphaGen == XBOX_NV2A_ALPHAGEN_WAVE ||
		b->adjustColorsForFog != XBOX_NV2A_ACFF_NONE)
		return qfalse;
	return qtrue;
}

static qboolean XboxNV2AStageTexCoordsPlain(const xboxNV2AStage_t *stage)
{
	return (stage->tcGen == XBOX_NV2A_TCGEN_TEXTURE || stage->tcGen == XBOX_NV2A_TCGEN_LIGHTMAP) &&
		!stage->numTexMods;
}

/* One pass that ComputeColors and ComputeTexCoords would only copy world vertex data for. */
static qboolean XboxNV2ACanDrawFast(const XboxNV2AShader *shader)
{
	const xboxNV2AStage_t *stage = &shader->stages[0];

	if (shader->isSky || shader->numDeforms || shader->numStages != (shader->collapsed ? 2 : 1))
		return qfalse;
	if (!XboxNV2AStageTexCoordsPlain(stage) ||
		(shader->collapsed && !XboxNV2AStageTexCoordsPlain(&shader->stages[1])))
		return qfalse;
	if (stage->rgbGen != XBOX_NV2A_RGBGEN_IDENTITY_LIGHTING &&
		stage->rgbGen != XBOX_NV2A_RGBGEN_IDENTITY && stage->rgbGen != XBOX_NV2A_RGBGEN_VERTEX &&
		stage->rgbGen != XBOX_NV2A_RGBGEN_EXACT_VERTEX)
		return qfalse;
	return stage->alphaGen == XBOX_NV2A_ALPHAGEN_IDENTITY ||
		stage->alphaGen == XBOX_NV2A_ALPHAGEN_VERTEX;
}

/* Unresolved names are kept without stages so the loader does not retry them. */
qboolean XboxNV2A_FindShader(const char *name, int flavor, qhandle_t *handle)
{
	unsigned int index;

	for (index = XBOX_NV2A_WHITE_SHADER; index < xboxNV2AShaderCount; ++index) {
		if (xboxNV2AShaders[index].flavor == flavor &&
			!Q_stricmp(xboxNV2AShaders[index].name, name)) {
			*handle = XboxNV2AShaderUsable(&xboxNV2AShaders[index]) ? (qhandle_t)index : 0;
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
	shader->polygonOffset = def->polygonOffset;
	shader->isSky = def->isSky;
	shader->cloudHeight = def->cloudHeight;
	shader->isFog = def->isFog;
	VectorCopy(def->fogColor, shader->fogColor);
	shader->fogDepth = def->fogDepth;
	shader->fogPass = def->fogPass;
	shader->portalRange = def->portalRange;
	shader->noDlight = def->noDlight;
	for (i = 0; i < XBOX_NV2A_SKY_SIDES; ++i) {
		if (def->skyBox[i] > 0 && (unsigned int)def->skyBox[i] < xboxNV2AImageCount)
			shader->skyBox[i] = def->skyBox[i];
	}
	if (def->numDeforms > 0)
		shader->deforms = (xboxNV2ADeform_t *)malloc(
			(size_t)def->numDeforms * sizeof(*shader->deforms));
	if (shader->deforms) {
		memcpy(shader->deforms, def->deforms,
			(size_t)def->numDeforms * sizeof(*shader->deforms));
		shader->numDeforms = def->numDeforms;
	}
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
		shader->collapsed = XboxNV2ACanCollapse(shader);
		if (shader->collapsed)
			xboxNV2ACollapsedCount++;
		shader->fastWorld = XboxNV2ACanDrawFast(shader);
	}
	xboxNV2AShaderCount++;
	return XboxNV2AShaderUsable(shader) ? (qhandle_t)(xboxNV2AShaderCount - 1) : 0;
}

/* Sky surfaces only mark the sky box; a stageless fog shader draws its fog pass. */
qboolean XboxNV2A_ShaderIsDrawable(qhandle_t shader)
{
	return shader > 0 && (unsigned int)shader < xboxNV2AShaderCount &&
		(xboxNV2AShaders[shader].numStages || xboxNV2AShaders[shader].isFog) &&
		!xboxNV2AShaders[shader].isSky;
}

/* ioq3 R_LoadFogs reads fogParms from the fog shader; a missing shader gives black fog. */
qboolean XboxNV2A_ShaderFogParms(qhandle_t shader, vec3_t color, float *depth)
{
	if (shader <= 0 || (unsigned int)shader >= xboxNV2AShaderCount) {
		VectorClear(color);
		*depth = 0.0f;
		return qfalse;
	}
	VectorCopy(xboxNV2AShaders[shader].fogColor, color);
	*depth = xboxNV2AShaders[shader].fogDepth;
	return qtrue;
}

qboolean XboxNV2A_ShaderIsSky(qhandle_t shader)
{
	return shader > 0 && (unsigned int)shader < xboxNV2AShaderCount &&
		xboxNV2AShaders[shader].isSky && XboxNV2AShaderUsable(&xboxNV2AShaders[shader]);
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

/* ioq3 R_FogFactor: s grows with view depth in fog, t with depth below the fog surface. */
static float XboxNV2AFogFactor(float s, float t)
{
	s -= 1.0f / 512;
	if (s < 0.0f || t < 1.0f / 32)
		return 0.0f;
	if (t < 31.0f / 32)
		s *= (t - 1.0f / 32) / (30.0f / 32);
	/* ioq3 leaves a lot of clamp range. */
	s *= 8;
	if (s > 1.0f)
		s = 1.0f;
	return xboxNV2AFogTable[(int)(s * (XBOX_NV2A_FOG_TABLE_SIZE - 1))];
}

/* ioq3 R_CreateFogImage: white texels whose alpha is the fog factor. */
static int XboxNV2ACreateFogImage(void)
{
	byte *data = (byte *)malloc(XBOX_NV2A_FOG_S * XBOX_NV2A_FOG_T * 4);
	int image, x, y;

	if (!data)
		return 0;
	for (y = 0; y < XBOX_NV2A_FOG_T; ++y) {
		for (x = 0; x < XBOX_NV2A_FOG_S; ++x) {
			byte *texel = data + (y * XBOX_NV2A_FOG_S + x) * 4;

			texel[0] = texel[1] = texel[2] = 255;
			texel[3] = (byte)(255 * XboxNV2AFogFactor((x + 0.5f) / XBOX_NV2A_FOG_S,
				(y + 0.5f) / XBOX_NV2A_FOG_T));
		}
	}
	image = XboxNV2AAddImage("*fog", XBOX_NV2A_FOG_S, XBOX_NV2A_FOG_T, data,
		XBOX_NV2A_IMAGE_32BIT, qfalse);
	free(data);
	return image;
}

/* ioq3 R_CreateDlightImage: a centred inverse-square blob, cut to 0 below 75. */
static int XboxNV2ACreateDlightImage(void)
{
	byte data[XBOX_NV2A_DLIGHT_SIZE][XBOX_NV2A_DLIGHT_SIZE][4];
	int x, y;

	for (x = 0; x < XBOX_NV2A_DLIGHT_SIZE; ++x) {
		for (y = 0; y < XBOX_NV2A_DLIGHT_SIZE; ++y) {
			float dx = XBOX_NV2A_DLIGHT_SIZE / 2 - 0.5f - x;
			float dy = XBOX_NV2A_DLIGHT_SIZE / 2 - 0.5f - y;
			int b = (int)(4000 / (dx * dx + dy * dy));

			if (b > 255)
				b = 255;
			else if (b < 75)
				b = 0;
			data[y][x][0] = data[y][x][1] = data[y][x][2] = (byte)b;
			data[y][x][3] = 255;
		}
	}
	return XboxNV2AAddImage("*dlight", XBOX_NV2A_DLIGHT_SIZE, XBOX_NV2A_DLIGHT_SIZE,
		&data[0][0][0], XBOX_NV2A_IMAGE_32BIT, qfalse);
}

static void XboxNV2AResetTextures(void)
{
	static const byte whitePixel[4] = {255, 255, 255, 255};
	xboxNV2AShaderDef_t white;
	unsigned int i;

	for (i = 0; i < xboxNV2AShaderCount; ++i) {
		free(xboxNV2AShaders[i].deforms);
		free(xboxNV2AShaders[i].stages);
	}
	XboxNV2AFreeCinematics();
	memset(xboxNV2AImages, 0, sizeof(xboxNV2AImages));
	memset(xboxNV2AShaders, 0, sizeof(xboxNV2AShaders));
	Q_strncpyz(xboxNV2AImages[0].name, "*missing", sizeof(xboxNV2AImages[0].name));
	Q_strncpyz(xboxNV2AShaders[0].name, "*missing", sizeof(xboxNV2AShaders[0].name));
	xboxNV2AImageCount = XBOX_NV2A_WHITE_IMAGE;
	xboxNV2AShaderCount = XBOX_NV2A_WHITE_SHADER;
	xboxNV2ATexturePoolUsed = 0;
	xboxNV2ADxtImageCount = 0;
	xboxNV2ACollapsedCount = 0;
	XboxNV2AAddImage("*white", 1, 1, whitePixel, XBOX_NV2A_IMAGE_16BIT, qfalse);
	xboxNV2AFogImage = XboxNV2ACreateFogImage();
	xboxNV2ADlightImage = XboxNV2ACreateDlightImage();

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
	xboxNV2ATess.numFast = 0;
	xboxNV2ATess.fastVerts = 0;
	xboxNV2ATess.fastIndexes = 0;
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
	xboxNV2APerf.draws++;
	xboxNV2APerf.verts += count;
}

/* Returns room for count vertices, rewinding the stream once the GPU is idle. */
static XboxNV2AColoredVertex *XboxNV2AStreamVertices(unsigned int count)
{
	if (xboxNV2AVertexUsed + count > XBOX_NV2A_MAX_VERTS) {
		xboxNV2APerf.midWaitTicks += XboxNV2AWaitIdle("vertex stream");
		xboxNV2APerf.streamWraps++;
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

/* ioq3 RB_CalcFogTexCoords in world space: s is view depth, t depth below the fog surface. */
static void XboxNV2ACalcFogTexCoords(float (*st)[2])
{
	const XboxNV2ATess *tess = &xboxNV2ATess;
	const xboxNV2AFog_t *fog = XboxNV2AWorld_Fog(tess->fogIndex);
	const refEntity_t *model = tess->entity && tess->entity->reType == RT_MODEL ?
		tess->entity : NULL;
	float eyeT = 1.0f;
	float eyeDepth = DotProduct(xboxNV2AViewOrigin, xboxNV2AViewAxis[0]);
	int i;

	if (!fog) {
		memset(st, 0, (size_t)tess->numVerts * sizeof(st[0]));
		return;
	}
	/* Fog without a visible surface always has the eye inside. */
	if (fog->hasSurface)
		eyeT = DotProduct(xboxNV2AViewOrigin, fog->surface) - fog->surface[3];
	for (i = 0; i < tess->numVerts; ++i) {
		vec3_t world;
		float t = 0.0f;

		if (model) {
			VectorCopy(model->origin, world);
			VectorMA(world, tess->xyz[i][0], model->axis[0], world);
			VectorMA(world, tess->xyz[i][1], model->axis[1], world);
			VectorMA(world, tess->xyz[i][2], model->axis[2], world);
		} else {
			VectorCopy(tess->xyz[i], world);
		}
		if (fog->hasSurface)
			t = DotProduct(world, fog->surface) - fog->surface[3];
		/* An eye outside the fog only counts the part of the ray below the fog plane. */
		if (eyeT < 0.0f)
			t = t < 1.0f ? 1.0f / 32 : 1.0f / 32 + 30.0f / 32 * t / (t - eyeT);
		else
			t = t < 0.0f ? 1.0f / 32 : 31.0f / 32;
		st[i][0] = (DotProduct(world, xboxNV2AViewAxis[0]) - eyeDepth) * fog->tcScale + 1.0f / 512;
		st[i][1] = t;
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
	case XBOX_NV2A_ALPHAGEN_PORTAL: {
		/* Transparent up close, opaque at portalRange and beyond. */
		float range = xboxNV2AShaders[tess->shader].portalRange;

		for (i = 0; i < n; ++i) {
			vec3_t v;
			float len;

			VectorSubtract(tess->xyz[i], tess->viewOrigin, v);
			len = range > 0.0f ? VectorLength(v) / range : 1.0f;
			tess->stageColor[i][3] = len > 1.0f ? 255 : (byte)(len * 255);
		}
		break;
	}
	}

	/* ioq3 RB_CalcModulate*ByFog; stageSt is scratch until the texcoords are computed. */
	if (tess->fogIndex && stage->adjustColorsForFog != XBOX_NV2A_ACFF_NONE) {
		XboxNV2ACalcFogTexCoords(tess->stageSt);
		for (i = 0; i < n; ++i) {
			float f = 1.0f - XboxNV2AFogFactor(tess->stageSt[i][0], tess->stageSt[i][1]);

			if (stage->adjustColorsForFog != XBOX_NV2A_ACFF_MODULATE_ALPHA) {
				tess->stageColor[i][0] = (byte)(tess->stageColor[i][0] * f);
				tess->stageColor[i][1] = (byte)(tess->stageColor[i][1] * f);
				tess->stageColor[i][2] = (byte)(tess->stageColor[i][2] * f);
			}
			if (stage->adjustColorsForFog != XBOX_NV2A_ACFF_MODULATE_RGB)
				tess->stageColor[i][3] = (byte)(tess->stageColor[i][3] * f);
		}
	}
}

static void XboxNV2ATransformTexCoords(float (*st)[2], float m00, float m01, float m10,
	float m11, float t0, float t1)
{
	int i;

	for (i = 0; i < xboxNV2ATess.numVerts; ++i) {
		float s = st[i][0];
		float t = st[i][1];

		st[i][0] = s * m00 + t * m10 + t0;
		st[i][1] = s * m01 + t * m11 + t1;
	}
}

static void XboxNV2AScrollTexCoords(float (*st)[2], const float *speed)
{
	double s = speed[0] * xboxNV2ATess.shaderTime;
	double t = speed[1] * xboxNV2ATess.shaderTime;
	int i;

	/* ioq3 keeps the offset in [0,1) so coordinates stay small. */
	s -= floor(s);
	t -= floor(t);
	for (i = 0; i < xboxNV2ATess.numVerts; ++i) {
		st[i][0] += (float)s;
		st[i][1] += (float)t;
	}
}

/* ioq3 ComputeTexCoords and the RB_Calc*TexCoords helpers, into st. */
static void XboxNV2AComputeTexCoords(const xboxNV2AStage_t *stage, float (*st)[2])
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
			st[i][0] = 0.5f + reflected[1] * 0.5f;
			st[i][1] = 0.5f - reflected[2] * 0.5f;
		}
	} else if (stage->tcGen == XBOX_NV2A_TCGEN_VECTOR) {
		for (i = 0; i < n; ++i) {
			st[i][0] = DotProduct(tess->xyz[i], stage->tcGenVectors[0]);
			st[i][1] = DotProduct(tess->xyz[i], stage->tcGenVectors[1]);
		}
	} else if (stage->tcGen == XBOX_NV2A_TCGEN_LIGHTMAP) {
		memcpy(st, tess->lightSt, (size_t)n * sizeof(tess->lightSt[0]));
	} else {
		memcpy(st, tess->st, (size_t)n * sizeof(tess->st[0]));
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

				st[i][0] += xboxNV2ASinTable[s & XBOX_NV2A_FUNCTABLE_MASK] *
					mod->wave.amplitude;
				st[i][1] += xboxNV2ASinTable[t & XBOX_NV2A_FUNCTABLE_MASK] *
					mod->wave.amplitude;
			}
			break;
		}
		case XBOX_NV2A_TCMOD_ENTITY_TRANSLATE:
			if (tess->entity)
				XboxNV2AScrollTexCoords(st, tess->entity->shaderTexCoord);
			break;
		case XBOX_NV2A_TCMOD_SCROLL:
			XboxNV2AScrollTexCoords(st, mod->scroll);
			break;
		case XBOX_NV2A_TCMOD_SCALE:
			for (i = 0; i < n; ++i) {
				st[i][0] *= mod->scale[0];
				st[i][1] *= mod->scale[1];
			}
			break;
		case XBOX_NV2A_TCMOD_STRETCH: {
			float wave = XboxNV2AEvalWave(&mod->wave, tess->shaderTime);
			float p = wave != 0.0f ? 1.0f / wave : 1.0f;

			XboxNV2ATransformTexCoords(st, p, 0.0f, 0.0f, p, 0.5f - 0.5f * p, 0.5f - 0.5f * p);
			break;
		}
		case XBOX_NV2A_TCMOD_TRANSFORM:
			XboxNV2ATransformTexCoords(st, mod->matrix[0][0], mod->matrix[0][1],
				mod->matrix[1][0], mod->matrix[1][1], mod->translate[0], mod->translate[1]);
			break;
		case XBOX_NV2A_TCMOD_ROTATE: {
			int index = (int)(-mod->rotateSpeed * tess->shaderTime *
				(XBOX_NV2A_FUNCTABLE_SIZE / 360.0f));
			float sinValue = xboxNV2ASinTable[index & XBOX_NV2A_FUNCTABLE_MASK];
			float cosValue = xboxNV2ASinTable[(index + XBOX_NV2A_FUNCTABLE_SIZE / 4) &
				XBOX_NV2A_FUNCTABLE_MASK];

			XboxNV2ATransformTexCoords(st, cosValue, sinValue, -sinValue, cosValue,
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
	memcpy(out->color, tess->stageColor[v], 4);
	out->texcoord[0] = tess->stageSt[v][0];
	out->texcoord[1] = tess->stageSt[v][1];
	out->texcoord1[0] = tess->multitexture ? tess->stageSt1[v][0] : 0.0f;
	out->texcoord1[1] = tess->multitexture ? tess->stageSt1[v][1] : 0.0f;
}

/* A clip polygon corner in floats, plus its near and portal plane distances. */
typedef struct {
	float position[3];
	float color[4];
	float st[2][2];
	float dist[2];
} XboxNV2AClipVertex;

/* Sutherland-Hodgman against the near plane, then a portal view's plane; returns the corners. */
static unsigned int XboxNV2AClipTriangle(const unsigned short *triangle,
	XboxNV2AColoredVertex *out)
{
	const XboxNV2ATess *tess = &xboxNV2ATess;
	XboxNV2AClipVertex polygons[2][XBOX_NV2A_CLIP_VERTS];
	int planes = xboxNV2AViewIsPortal ? 2 : 1;
	int count = 3;
	int current = 0;
	int p, k, j;

	for (k = 0; k < 3; ++k) {
		XboxNV2AClipVertex *c = &polygons[0][k];
		int v = triangle[k];

		for (j = 0; j < 3; ++j)
			c->position[j] = tess->xyz[v][j];
		for (j = 0; j < 4; ++j)
			c->color[j] = tess->stageColor[v][j];
		for (j = 0; j < 2; ++j) {
			c->st[0][j] = tess->stageSt[v][j];
			c->st[1][j] = tess->multitexture ? tess->stageSt1[v][j] : 0.0f;
		}
		c->dist[0] = tess->nearDist[v];
		c->dist[1] = planes > 1 ? tess->portalDist[v] : 0.0f;
	}
	for (p = 0; p < planes; ++p) {
		const XboxNV2AClipVertex *in = polygons[current];
		XboxNV2AClipVertex *clipped = polygons[!current];
		int kept = 0;

		for (k = 0; k < count; ++k) {
			const XboxNV2AClipVertex *a = &in[k];
			const XboxNV2AClipVertex *b = &in[(k + 1) % count];
			qboolean aInside = a->dist[p] >= 0.0f;

			if (aInside)
				clipped[kept++] = *a;
			if (aInside != (b->dist[p] >= 0.0f)) {
				XboxNV2AClipVertex *c = &clipped[kept++];
				float t = a->dist[p] / (a->dist[p] - b->dist[p]);

				for (j = 0; j < 3; ++j)
					c->position[j] = a->position[j] + t * (b->position[j] - a->position[j]);
				for (j = 0; j < 4; ++j)
					c->color[j] = a->color[j] + t * (b->color[j] - a->color[j]);
				for (j = 0; j < 2; ++j) {
					c->st[0][j] = a->st[0][j] + t * (b->st[0][j] - a->st[0][j]);
					c->st[1][j] = a->st[1][j] + t * (b->st[1][j] - a->st[1][j]);
					c->dist[j] = a->dist[j] + t * (b->dist[j] - a->dist[j]);
				}
			}
		}
		count = kept;
		current = !current;
		if (count < 3)
			return 0;
	}
	for (k = 0; k < count; ++k) {
		const XboxNV2AClipVertex *c = &polygons[current][k];

		for (j = 0; j < 3; ++j)
			out[k].position[j] = c->position[j];
		for (j = 0; j < 4; ++j)
			out[k].color[j] = (byte)(c->color[j] + 0.5f);
		for (j = 0; j < 2; ++j) {
			out[k].texcoord[j] = c->st[0][j];
			out[k].texcoord1[j] = c->st[1][j];
		}
	}
	return (unsigned int)count;
}

static qboolean XboxNV2AVertexClipped(int v)
{
	return xboxNV2ATess.nearDist[v] < 0.0f ||
		(xboxNV2AViewIsPortal && xboxNV2ATess.portalDist[v] < 0.0f);
}

static qboolean XboxNV2ATriangleCrosses(const unsigned short *triangle)
{
	return xboxNV2ATess.clip && (XboxNV2AVertexClipped(triangle[0]) ||
		XboxNV2AVertexClipped(triangle[1]) || XboxNV2AVertexClipped(triangle[2]));
}

/* Inline 16-bit indexes after a vertex cache break, as the NV2A has no index buffers. */
static void XboxNV2ADrawElements(const unsigned short *elements, unsigned int count)
{
	uint32_t *p;

	/* xgux_draw_elements16: begin and end, 120 index pairs per block, a last odd index alone. */
	XboxNV2AReserve(12 + count / 2 + count / 240);
	p = pb_begin();
	p = push_command_parameter(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
	pb_end(p);
	xgux_draw_elements16(XGU_TRIANGLES, elements, count);
	xboxNV2APerf.draws++;
	xboxNV2APerf.indexes += count;
}

/* Tess vertices stream once; a triangle crossing the near or portal plane adds its clipped corners. */
static void XboxNV2ADrawTriangles(const unsigned short *indexes, int numIndexes)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	unsigned short *elements = tess->drawIndexes;
	unsigned int crossing = 0;
	unsigned int count = 0;
	unsigned int base, extra;
	XboxNV2AColoredVertex *out;
	int i, k;

	for (i = 0; tess->clip && i + 2 < numIndexes; i += 3) {
		if (XboxNV2ATriangleCrosses(&indexes[i]))
			crossing++;
	}
	out = XboxNV2AStreamVertices((unsigned int)tess->numVerts +
		crossing * (XBOX_NV2A_CLIP_VERTS - 1));
	base = xboxNV2AVertexUsed;
	for (k = 0; k < tess->numVerts; ++k)
		XboxNV2AWriteVertex(&out[k], k);
	extra = (unsigned int)tess->numVerts;
	for (i = 0; i + 2 < numIndexes; i += 3) {
		const unsigned short *triangle = &indexes[i];

		if (XboxNV2ATriangleCrosses(triangle)) {
			unsigned int corners = XboxNV2AClipTriangle(triangle, &out[extra]);

			for (k = 1; k + 1 < (int)corners; ++k) {
				elements[count++] = (unsigned short)(base + extra);
				elements[count++] = (unsigned short)(base + extra + k);
				elements[count++] = (unsigned short)(base + extra + k + 1);
			}
			extra += corners;
			continue;
		}
		elements[count++] = (unsigned short)(base + triangle[0]);
		elements[count++] = (unsigned short)(base + triangle[1]);
		elements[count++] = (unsigned short)(base + triangle[2]);
	}
	xboxNV2AVertexUsed = base + extra;
	xboxNV2APerf.verts += extra;
	if (count)
		XboxNV2ADrawElements(elements, count);
}

/* A mirror view flips the culled side. */
static int XboxNV2AViewCull(int cull)
{
	if (!xboxNV2AViewIsMirror || cull == XBOX_NV2A_CULL_NONE)
		return cull;
	return cull == XBOX_NV2A_CULL_FRONT ? XBOX_NV2A_CULL_BACK : XBOX_NV2A_CULL_FRONT;
}

/* Draws the computed stage colours and texcoords; image1 is unit 1's image. */
static void XboxNV2ASubmitStage(const xboxNV2AStage_t *stage, int image, int image1,
	qboolean clamp1, int cull, qboolean polygonOffset)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	xboxNV2AStage_t skyStage;
	XboxNV2AColoredVertex *out;
	unsigned int count = (unsigned int)tess->numVerts;
	unsigned int i;

	if (tess->isSky && stage->depthWrite) {
		skyStage = *stage;
		skyStage.depthWrite = qfalse;
		stage = &skyStage;
	}
	XboxNV2AApplyStageState(stage, image, image1, clamp1, tess->is3D, tess->is3D,
		XboxNV2AViewCull(cull), polygonOffset);
	if (tess->is3D) {
		XboxNV2ADrawTriangles(tess->indexes, tess->numIndexes);
		return;
	}
	out = XboxNV2AStreamVertices(count);
	for (i = 0; i < count; ++i)
		XboxNV2AWriteVertex(&out[i], (int)i);
	XboxNV2ADrawVertices(XGU_QUADS, xboxNV2AVertexUsed, count);
	xboxNV2AVertexUsed += count;
}

/* Draws one stage, or with second a collapsed pair: stage's colours times both images. */
static void XboxNV2ADrawStage(const xboxNV2AStage_t *stage, const xboxNV2AStage_t *second,
	int cull, qboolean polygonOffset)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int image1 = XBOX_NV2A_WHITE_IMAGE;
	qboolean clamp1 = qtrue;

	XboxNV2AComputeColors(stage);
	XboxNV2AComputeTexCoords(stage, tess->stageSt);
	if (second) {
		XboxNV2AComputeTexCoords(second, tess->stageSt1);
		image1 = XboxNV2AStageImage(second, tess->shaderTime);
		clamp1 = second->clamp;
		tess->multitexture = qtrue;
	}
	XboxNV2ASubmitStage(stage, XboxNV2AStageImage(stage, tess->shaderTime), image1, clamp1,
		cull, polygonOffset);
	tess->multitexture = qfalse;
}

/* A collapsed shader draws stages 0 and 1 in one pass, as ioq3 DrawMultitextured. */
static void XboxNV2ADrawShaderStages(const XboxNV2AShader *shader, int cull,
	qboolean polygonOffset)
{
	int i;

	for (i = 0; i < shader->numStages; ++i) {
		const xboxNV2AStage_t *second = i == 0 && shader->collapsed ? &shader->stages[1] : NULL;

		XboxNV2ADrawStage(&shader->stages[i], second, cull, polygonOffset);
		if (second)
			++i;
	}
}

/* XboxNV2ADrawStage's output for a fastWorld shader, written straight from the world vertices. */
static void XboxNV2ADrawFastSurfaces(const XboxNV2AShader *shader)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const xboxNV2AStage_t *stage = &shader->stages[0];
	const xboxNV2AStage_t *second = shader->collapsed ? &shader->stages[1] : NULL;
	qboolean rgbVertex = stage->rgbGen == XBOX_NV2A_RGBGEN_VERTEX ||
		stage->rgbGen == XBOX_NV2A_RGBGEN_EXACT_VERTEX;
	/* ComputeColors keeps the vertex alpha under alphaGen identity after rgbGen vertex only. */
	qboolean alphaVertex = stage->alphaGen == XBOX_NV2A_ALPHAGEN_VERTEX ||
		stage->rgbGen == XBOX_NV2A_RGBGEN_VERTEX;
	qboolean lightSt0 = stage->tcGen == XBOX_NV2A_TCGEN_LIGHTMAP;
	qboolean lightSt1 = second && second->tcGen == XBOX_NV2A_TCGEN_LIGHTMAP;
	unsigned short *elements = tess->drawIndexes;
	XboxNV2AColoredVertex *out;
	unsigned int first, base;
	unsigned int count = 0;
	int i, k;

	xboxNV2ALastShaderName = shader->name;
	XboxNV2AApplyStageState(stage, XboxNV2AStageImage(stage, tess->shaderTime),
		second ? XboxNV2AStageImage(second, tess->shaderTime) : XBOX_NV2A_WHITE_IMAGE,
		second ? second->clamp : qtrue, qtrue, qtrue, XboxNV2AViewCull(shader->cull),
		shader->polygonOffset);
	out = XboxNV2AStreamVertices((unsigned int)tess->fastVerts);
	first = base = xboxNV2AVertexUsed;
	for (i = 0; i < tess->numFast; ++i) {
		const xboxNV2AWorldSurface_t *surface = tess->fast[i];
		const xboxNV2AWorldVert_t *in = surface->verts;

		for (k = 0; k < surface->numIndexes; ++k)
			elements[count++] = (unsigned short)(base + surface->indexes[k]);
		/* Fields in struct order, so the write-combined stream sees sequential stores. */
		for (k = 0; k < surface->numVerts; ++k, ++in, ++out) {
			const float *st0 = lightSt0 ? in->lightSt : in->st;
			const float *st1 = lightSt1 ? in->lightSt : in->st;

			out->position[0] = in->xyz[0];
			out->position[1] = in->xyz[1];
			out->position[2] = in->xyz[2];
			out->color[0] = rgbVertex ? in->color[0] : 255;
			out->color[1] = rgbVertex ? in->color[1] : 255;
			out->color[2] = rgbVertex ? in->color[2] : 255;
			out->color[3] = alphaVertex ? in->color[3] : 255;
			out->texcoord[0] = st0[0];
			out->texcoord[1] = st0[1];
			out->texcoord1[0] = second ? st1[0] : 0.0f;
			out->texcoord1[1] = second ? st1[1] : 0.0f;
		}
		base += (unsigned int)surface->numVerts;
	}
	xboxNV2AVertexUsed = base;
	xboxNV2APerf.verts += base - first;
	xboxNV2APerf.fastVerts += base - first;
	if (count)
		XboxNV2ADrawElements(elements, count);
	tess->numFast = 0;
	tess->fastVerts = 0;
	tess->fastIndexes = 0;
}

/* True when the whole box is in front of the active transform's near plane. */
static qboolean XboxNV2ABoxInFrontOfNear(const vec3_t bounds[2])
{
	float d = xboxNV2ANearPlane[3];
	int i;

	for (i = 0; i < 3; ++i)
		d += xboxNV2ANearPlane[i] * bounds[xboxNV2ANearPlane[i] >= 0.0f ? 0 : 1][i];
	return d >= 0.0f;
}

/* Queues a world surface of the current batch on the fast list; qfalse means use the tess. */
static qboolean XboxNV2AAddFastSurface(const xboxNV2AWorldSurface_t *surface)
{
	XboxNV2ATess *tess = &xboxNV2ATess;

	/* Fog, dlight and portal clipping read the tess arrays. */
	if (!xboxNV2AShaders[tess->shader].fastWorld || tess->fogIndex || tess->dlighted ||
		xboxNV2AViewIsPortal || !XboxNV2ABoxInFrontOfNear(surface->bounds))
		return qfalse;
	if (tess->numFast == XBOX_NV2A_TESS_VERTS ||
		tess->fastVerts + surface->numVerts > XBOX_NV2A_TESS_VERTS ||
		tess->fastIndexes + surface->numIndexes > XBOX_NV2A_TESS_INDEXES)
		XboxNV2ADrawFastSurfaces(&xboxNV2AShaders[tess->shader]);
	tess->fast[tess->numFast++] = surface;
	tess->fastVerts += surface->numVerts;
	tess->fastIndexes += surface->numIndexes;
	return qtrue;
}

/* ioq3 RB_FogPass: the fog image blended over the surface; opaque shaders need equal depth. */
static void XboxNV2AFogPass(const XboxNV2AShader *shader)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const xboxNV2AFog_t *fog = XboxNV2AWorld_Fog(tess->fogIndex);
	xboxNV2AStage_t stage;
	int i;

	if (!fog || !xboxNV2AFogImage)
		return;
	/* DIAGNOSTIC: the first fog pass, for the hardware test log. */
	if (!(xboxNV2ATracedTypes & (1u << 28))) {
		xboxNV2ATracedTypes |= 1u << 28;
		Sys_XboxLog("Xbox NV2A: frame %u first fog pass, fog %d shader %s\n",
			xboxNV2AFrameCount, tess->fogIndex, shader->name);
	}
	memset(&stage, 0, sizeof(stage));
	stage.clamp = qtrue;
	stage.srcBlend = GL_SRC_ALPHA;
	stage.dstBlend = GL_ONE_MINUS_SRC_ALPHA;
	stage.depthEqual = shader->fogPass == XBOX_NV2A_FOGPASS_EQUAL;
	for (i = 0; i < tess->numVerts; ++i)
		memcpy(tess->stageColor[i], fog->color, 4);
	XboxNV2ACalcFogTexCoords(tess->stageSt);
	XboxNV2ASubmitStage(&stage, xboxNV2AFogImage, XBOX_NV2A_WHITE_IMAGE, qtrue, shader->cull,
		shader->polygonOffset);
}

/* ioq3 ProjectDlightTexture: one pass per dlight over the triangles its blob reaches. */
static void XboxNV2ADlightPass(const XboxNV2AShader *shader)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const refEntity_t *e = tess->entity && tess->entity->reType == RT_MODEL ? tess->entity : NULL;
	int numDlights = xboxNV2ADlightCount - xboxNV2ASceneFirstDlight;
	byte clipBits[XBOX_NV2A_TESS_VERTS];
	xboxNV2AStage_t stage;
	int l, i, k;

	if (!xboxNV2ADlightImage)
		return;
	/* Depth equal keeps alpha-tested holes unlit; DST_COLOR ONE adds light scaled by the surface. */
	memset(&stage, 0, sizeof(stage));
	stage.clamp = qtrue;
	stage.srcBlend = GL_DST_COLOR;
	stage.dstBlend = GL_ONE;
	stage.depthEqual = qtrue;
	for (l = 0; l < numDlights; ++l) {
		const xboxNV2ADlight_t *dl = &xboxNV2ADlights[xboxNV2ASceneFirstDlight + l];
		float scale = 1.0f / dl->radius;
		vec3_t origin;
		int numIndexes = 0;

		if (!(tess->dlightBits & (1u << l)))
			continue;
		/* ioq3 R_TransformDlights into the entity's space. */
		if (e) {
			vec3_t delta;

			VectorSubtract(dl->origin, e->origin, delta);
			for (k = 0; k < 3; ++k)
				origin[k] = DotProduct(delta, e->axis[k]);
		} else {
			VectorCopy(dl->origin, origin);
		}
		for (i = 0; i < tess->numVerts; ++i) {
			vec3_t dist;
			float modulate;
			int clip = 0;

			VectorSubtract(origin, tess->xyz[i], dist);
			tess->stageSt[i][0] = 0.5f + dist[0] * scale;
			tess->stageSt[i][1] = 0.5f + dist[1] * scale;
			if (tess->stageSt[i][0] < 0.0f)
				clip |= 1;
			else if (tess->stageSt[i][0] > 1.0f)
				clip |= 2;
			if (tess->stageSt[i][1] < 0.0f)
				clip |= 4;
			else if (tess->stageSt[i][1] > 1.0f)
				clip |= 8;
			/* Full strength within half the radius above or below, fading to 0 at the radius. */
			if (dist[2] > dl->radius) {
				clip |= 16;
				modulate = 0.0f;
			} else if (dist[2] < -dl->radius) {
				clip |= 32;
				modulate = 0.0f;
			} else {
				dist[2] = Q_fabs(dist[2]);
				modulate = dist[2] < dl->radius * 0.5f ? 1.0f : 2.0f * (dl->radius - dist[2]) * scale;
			}
			clipBits[i] = (byte)clip;
			for (k = 0; k < 3; ++k) {
				float c = dl->color[k] * 255.0f * modulate;

				tess->stageColor[i][k] = (byte)(c > 255.0f ? 255 : (int)c);
			}
			tess->stageColor[i][3] = 255;
		}
		for (i = 0; i + 2 < tess->numIndexes; i += 3) {
			const unsigned short *triangle = &tess->indexes[i];

			if (clipBits[triangle[0]] & clipBits[triangle[1]] & clipBits[triangle[2]])
				continue;
			tess->dlightIndexes[numIndexes++] = triangle[0];
			tess->dlightIndexes[numIndexes++] = triangle[1];
			tess->dlightIndexes[numIndexes++] = triangle[2];
		}
		if (!numIndexes)
			continue;
		/* DIAGNOSTIC: the first dlight pass, for the hardware test log. */
		if (!(xboxNV2ATracedTypes & (1u << XBOX_NV2A_TRACE_DLIGHT))) {
			xboxNV2ATracedTypes |= 1u << XBOX_NV2A_TRACE_DLIGHT;
			Sys_XboxLog("Xbox NV2A: frame %u first dlight pass, shader %s dlight %d/%d "
				"radius %d triangles %d/%d\n", xboxNV2AFrameCount, shader->name, l, numDlights,
				(int)dl->radius, numIndexes / 3, tess->numIndexes / 3);
		}
		XboxNV2AApplyStageState(&stage, xboxNV2ADlightImage, XBOX_NV2A_WHITE_IMAGE, qtrue,
			qtrue, qtrue,
			XboxNV2AViewCull(shader->cull), shader->polygonOffset);
		XboxNV2ADrawTriangles(tess->dlightIndexes, numIndexes);
	}
}

/* Near-plane distances, plus the portal plane's in a mirror view; qfalse if nothing is left. */
static qboolean XboxNV2AComputeNearDistances(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int behindNear = 0;
	int behindPortal = 0;
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		tess->nearDist[i] = tess->xyz[i][0] * xboxNV2ANearPlane[0] +
			tess->xyz[i][1] * xboxNV2ANearPlane[1] +
			tess->xyz[i][2] * xboxNV2ANearPlane[2] + xboxNV2ANearPlane[3];
		if (tess->nearDist[i] < 0.0f)
			++behindNear;
		if (xboxNV2AViewIsPortal) {
			tess->portalDist[i] = DotProduct(tess->xyz[i], xboxNV2APortalPlaneLocal) -
				xboxNV2APortalPlaneLocal[3];
			if (tess->portalDist[i] < 0.0f)
				++behindPortal;
		}
	}
	tess->clip = behindNear > 0 || behindPortal > 0;
	return behindNear < tess->numVerts && behindPortal < tess->numVerts;
}

/* ioq3 RB_CalcDeformVertexes: with a frequency, the spread ties the phase to the position. */
static void XboxNV2ADeformWave(const xboxNV2ADeform_t *deform)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	xboxNV2AWave_t wave = deform->wave;
	float scale = XboxNV2AEvalWave(&wave, tess->shaderTime);
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		if (deform->wave.frequency != 0.0f) {
			wave.phase = deform->wave.phase + (tess->xyz[i][0] + tess->xyz[i][1] +
				tess->xyz[i][2]) * deform->spread;
			scale = XboxNV2AEvalWave(&wave, tess->shaderTime);
		}
		VectorMA(tess->xyz[i], scale, tess->normal[i], tess->xyz[i]);
	}
}

/* ioq3 RB_CalcDeformNormals: noise wiggles the normals for wavy environment maps. */
static void XboxNV2ADeformNormals(const xboxNV2ADeform_t *deform)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	double t = tess->shaderTime * deform->wave.frequency;
	int i, k;

	for (i = 0; i < tess->numVerts; ++i) {
		const float *xyz = tess->xyz[i];

		for (k = 0; k < 3; ++k)
			tess->normal[i][k] += deform->wave.amplitude * R_NoiseGet4f(100.0f * k +
				xyz[0] * 0.98f, xyz[1] * 0.98f, xyz[2] * 0.98f, t);
		VectorNormalizeFast(tess->normal[i]);
	}
}

/* ioq3 RB_CalcBulgeVertexes: runs on the refdef time, not the entity's shader time. */
static void XboxNV2ADeformBulge(const xboxNV2ADeform_t *deform)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	double now = xboxNV2AViewTime * 0.001 * deform->bulgeSpeed;
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		int off = (int)((float)(XBOX_NV2A_FUNCTABLE_SIZE / (M_PI * 2)) *
			(tess->st[i][0] * deform->bulgeWidth + now));
		float scale = xboxNV2ASinTable[off & XBOX_NV2A_FUNCTABLE_MASK] * deform->bulgeHeight;

		VectorMA(tess->xyz[i], scale, tess->normal[i], tess->xyz[i]);
	}
}

static void XboxNV2ADeformMove(const xboxNV2ADeform_t *deform)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	float scale = XboxNV2AEvalWave(&deform->wave, tess->shaderTime);
	int i;

	for (i = 0; i < tess->numVerts; ++i)
		VectorMA(tess->xyz[i], scale, deform->moveVector, tess->xyz[i]);
}

/* ioq3 GlobalVectorToLocal: a model entity's tess is in the entity's axes. */
static void XboxNV2AViewVectorToLocal(const vec3_t in, vec3_t out)
{
	const refEntity_t *e = xboxNV2ATess.entity;

	if (!e || e->reType != RT_MODEL) {
		VectorCopy(in, out);
		return;
	}
	out[0] = DotProduct(in, e->axis[0]);
	out[1] = DotProduct(in, e->axis[1]);
	out[2] = DotProduct(in, e->axis[2]);
}

/* ioq3 AutospriteDeform: each quad of the tess becomes a view-facing RB_AddQuadStamp. */
static void XboxNV2ADeformAutosprite(void)
{
	static const float corners[4][2] = {{1, 1}, {-1, 1}, {-1, -1}, {1, -1}};
	static const float st[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
	XboxNV2ATess *tess = &xboxNV2ATess;
	const refEntity_t *e = tess->entity;
	vec3_t leftDir, upDir, normal;
	int quads = tess->numVerts / 4;
	int q, v, k;

	XboxNV2AViewVectorToLocal(xboxNV2AViewAxis[1], leftDir);
	XboxNV2AViewVectorToLocal(xboxNV2AViewAxis[2], upDir);
	VectorNegate(xboxNV2AViewAxis[0], normal);
	for (q = 0; q < quads; ++q) {
		int first = q * 4;
		unsigned short *indexes = &tess->indexes[q * 6];
		vec3_t mid, delta, left, up;
		byte color[4];
		float radius;

		for (k = 0; k < 3; ++k)
			mid[k] = 0.25f * (tess->xyz[first][k] + tess->xyz[first + 1][k] +
				tess->xyz[first + 2][k] + tess->xyz[first + 3][k]);
		VectorSubtract(tess->xyz[first], mid, delta);
		radius = VectorLength(delta) * 0.707f;
		VectorScale(leftDir, radius, left);
		VectorScale(upDir, radius, up);
		if (xboxNV2AViewIsMirror)
			VectorNegate(left, left);
		if (e && e->nonNormalizedAxes) {
			float axisLength = VectorLength(e->axis[0]);

			axisLength = axisLength ? 1.0f / axisLength : 0.0f;
			VectorScale(left, axisLength, left);
			VectorScale(up, axisLength, up);
		}
		memcpy(color, tess->color[first], 4);
		for (v = 0; v < 4; ++v) {
			int n = first + v;

			for (k = 0; k < 3; ++k)
				tess->xyz[n][k] = mid[k] + corners[v][0] * left[k] + corners[v][1] * up[k];
			VectorCopy(normal, tess->normal[n]);
			tess->st[n][0] = tess->lightSt[n][0] = st[v][0];
			tess->st[n][1] = tess->lightSt[n][1] = st[v][1];
			memcpy(tess->color[n], color, 4);
		}
		indexes[0] = (unsigned short)first;
		indexes[1] = (unsigned short)(first + 1);
		indexes[2] = (unsigned short)(first + 3);
		indexes[3] = (unsigned short)(first + 3);
		indexes[4] = (unsigned short)(first + 1);
		indexes[5] = (unsigned short)(first + 2);
	}
	/* ioq3 would read past a partial quad; it is dropped here. */
	tess->numVerts = quads * 4;
	tess->numIndexes = quads * 6;
}

/* ioq3 Autosprite2Deform: each quad pivots around its long axis to face the viewer. */
static void XboxNV2ADeformAutosprite2(void)
{
	static const int edgeVerts[6][2] = {{0, 1}, {0, 2}, {0, 3}, {1, 2}, {1, 3}, {2, 3}};
	XboxNV2ATess *tess = &xboxNV2ATess;
	vec3_t forward;
	int i, j, k;

	XboxNV2AViewVectorToLocal(xboxNV2AViewAxis[0], forward);
	for (i = 0; i + 4 <= tess->numVerts && i / 4 * 6 + 6 <= tess->numIndexes; i += 4) {
		const unsigned short *indexes = &tess->indexes[i / 4 * 6];
		float lengths[2] = {999999.0f, 999999.0f};
		int nums[2] = {0, 0};
		vec3_t mid[2], major, minor;

		/* The two shortest edges are the quad's ends. */
		for (j = 0; j < 6; ++j) {
			vec3_t temp;
			float l;

			VectorSubtract(tess->xyz[i + edgeVerts[j][0]], tess->xyz[i + edgeVerts[j][1]], temp);
			l = DotProduct(temp, temp);
			if (l < lengths[0]) {
				nums[1] = nums[0];
				lengths[1] = lengths[0];
				nums[0] = j;
				lengths[0] = l;
			} else if (l < lengths[1]) {
				nums[1] = j;
				lengths[1] = l;
			}
		}
		for (j = 0; j < 2; ++j) {
			const float *v1 = tess->xyz[i + edgeVerts[nums[j]][0]];
			const float *v2 = tess->xyz[i + edgeVerts[nums[j]][1]];

			for (k = 0; k < 3; ++k)
				mid[j][k] = 0.5f * (v1[k] + v2[k]);
		}
		VectorSubtract(mid[1], mid[0], major);
		CrossProduct(major, forward, minor);
		VectorNormalize(minor);
		for (j = 0; j < 2; ++j) {
			int a = i + edgeVerts[nums[j]][0];
			int b = i + edgeVerts[nums[j]][1];
			float l = 0.5f * sqrtf(lengths[j]);

			/* The edge's direction in the index list picks the side each end goes to. */
			for (k = 0; k < 5; ++k) {
				if (indexes[k] == a && indexes[k + 1] == b)
					break;
			}
			VectorMA(mid[j], k == 5 ? l : -l, minor, tess->xyz[a]);
			VectorMA(mid[j], k == 5 ? -l : l, minor, tess->xyz[b]);
		}
	}
}

/* ioq3 RB_DeformTessGeometry, run on the CPU tess before the near-plane test. */
static void XboxNV2ADeformGeometry(const XboxNV2AShader *shader)
{
	static const char *names[] = {"wave", "normal", "bulge", "move", "autosprite",
		"autosprite2"};
	int i;

	for (i = 0; i < shader->numDeforms; ++i) {
		const xboxNV2ADeform_t *deform = &shader->deforms[i];
		unsigned int bit = 1u << (XBOX_NV2A_TRACE_DEFORM + deform->type);

		/* DIAGNOSTIC: the first deform of each type, for the hardware test log. */
		if (!(xboxNV2ATracedTypes & bit)) {
			xboxNV2ATracedTypes |= bit;
			Sys_XboxLog("Xbox NV2A: frame %u first deform %s, shader %s verts=%d indexes=%d\n",
				xboxNV2AFrameCount, names[deform->type], shader->name, xboxNV2ATess.numVerts,
				xboxNV2ATess.numIndexes);
		}
		switch (deform->type) {
		case XBOX_NV2A_DEFORM_WAVE:
			XboxNV2ADeformWave(deform);
			break;
		case XBOX_NV2A_DEFORM_NORMALS:
			XboxNV2ADeformNormals(deform);
			break;
		case XBOX_NV2A_DEFORM_BULGE:
			XboxNV2ADeformBulge(deform);
			break;
		case XBOX_NV2A_DEFORM_MOVE:
			XboxNV2ADeformMove(deform);
			break;
		case XBOX_NV2A_DEFORM_AUTOSPRITE:
			XboxNV2ADeformAutosprite();
			break;
		case XBOX_NV2A_DEFORM_AUTOSPRITE2:
			XboxNV2ADeformAutosprite2();
			break;
		}
	}
}

static void XboxNV2AEndSurface(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const XboxNV2AShader *shader = &xboxNV2AShaders[tess->shader];
	int i;

	if (tess->numFast)
		XboxNV2ADrawFastSurfaces(shader);
	/* 2D tess holds bare quads without indexes, so only 3D surfaces deform. */
	if (tess->shader > 0 && tess->is3D && tess->numVerts > 0 && tess->numIndexes > 0)
		XboxNV2ADeformGeometry(shader);
	if (tess->shader > 0 && tess->numVerts > 0 && (!tess->is3D || tess->numIndexes > 0) &&
		(!tess->is3D || XboxNV2AComputeNearDistances())) {
		xboxNV2ALastShaderName = shader->name;
		if (!tess->is3D && xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES)
			Sys_XboxLog("Xbox NV2A: frame %u 2d draw %s verts=%d stages=%d image0=%d %dx%d\n",
				xboxNV2AFrameCount, shader->name, tess->numVerts, shader->numStages,
				shader->stages[0].images[0],
				xboxNV2AImages[shader->stages[0].images[0]].width,
				xboxNV2AImages[shader->stages[0].images[0]].height);
		XboxNV2ADrawShaderStages(shader, shader->cull, shader->polygonOffset);
		/* ioq3 RB_StageIteratorGeneric: dlights after the stages, before fog, on opaque shaders. */
		if (tess->is3D && tess->dlightBits && shader->sort <= XBOX_NV2A_SORT_OPAQUE &&
			!shader->noDlight)
			XboxNV2ADlightPass(shader);
		if (tess->is3D && tess->fogIndex && shader->fogPass != XBOX_NV2A_FOGPASS_NONE)
			XboxNV2AFogPass(shader);
	}
	tess->shader = 0;
	tess->lightmap = 0;
	tess->dlightBits = 0;
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
	p = push_command_float(p, NV097_SET_POLYGON_OFFSET_SCALE_FACTOR,
		XBOX_NV2A_POLYGON_OFFSET_FACTOR);
	p = push_command_float(p, NV097_SET_POLYGON_OFFSET_BIAS, XBOX_NV2A_POLYGON_OFFSET_UNITS);
	pb_end(p);
	XboxNV2AInvalidateState();
	XboxNV2ASetTransform(xboxNV2AScreenMatrix.col, qtrue);

	for (i = 0; i < XGU_ATTRIBUTE_COUNT; ++i)
		xgux_set_attrib_pointer((XguVertexArray)i, XGU_FLOAT, 0, 0, NULL);
	xgux_set_attrib_pointer(XGU_VERTEX_ARRAY, XGU_FLOAT, 3, sizeof(*v),
		&v[0].position[0]);
	xgux_set_attrib_pointer(XGU_COLOR_ARRAY, XBOX_NV2A_VERTEX_UB_OGL, 4, sizeof(*v),
		&v[0].color[0]);
	xgux_set_attrib_pointer(XGU_TEXCOORD0_ARRAY, XGU_FLOAT, 2, sizeof(*v),
		&v[0].texcoord[0]);
	xgux_set_attrib_pointer(XGU_TEXCOORD1_ARRAY, XGU_FLOAT, 2, sizeof(*v),
		&v[0].texcoord1[0]);
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
	R_NoiseInit();

	/* 2D vertices are already screen pixels, which program mode outputs directly. */
	memset(&xboxNV2AScreenMatrix, 0, sizeof(xboxNV2AScreenMatrix));
	for (i = 0; i < 4; ++i)
		xboxNV2AScreenMatrix.col[i].f[i] = 1.0f;
	/* The xgu samples' z-buffer precision fixup. */
	xgux_set_depth_range(0.0f, XBOX_NV2A_ZMAX);

	xboxNV2AFastSky = Cvar_Get("r_fastsky", "0", CVAR_ARCHIVE);
	xboxNV2ADynamicLight = Cvar_Get("r_dynamiclight", "1", CVAR_ARCHIVE);
	/* ioq3's cvar, read when a shader is created, so a change applies from the next map. */
	xboxNV2AMultitexture = Cvar_Get("r_ext_multitexture", "1", CVAR_ARCHIVE);
	/* ioq3's cvar and default: 0 no vblank wait, 1 a wait per frame, N shows each frame N vblanks. */
	xboxNV2ASwapInterval = Cvar_Get("r_swapInterval", "0", CVAR_ARCHIVE);
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
	Sys_XboxLog("Xbox NV2A: %u images (%u DXT1), %u shaders (%u multitexture), "
		"%u KiB texture pool used\n", xboxNV2AImageCount, xboxNV2ADxtImageCount,
		xboxNV2AShaderCount, xboxNV2ACollapsedCount,
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
	xboxNV2APolyCount = 0;
	xboxNV2APolyVertCount = 0;
	xboxNV2ASceneFirstPoly = 0;
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
	LONGLONG now;

	(void)stereoFrame;
	if (!xboxNV2AInitialized)
		return;

	++xboxNV2AFrameCount;
	if (xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES ||
		xboxNV2AFrameCount % XBOX_NV2A_HEARTBEAT_FRAMES == 0)
		Sys_XboxLog("Xbox NV2A: frame %u begin\n", xboxNV2AFrameCount);
	if (xboxNV2AFrameCount % XBOX_NV2A_HEARTBEAT_FRAMES == 0)
		XboxNV2ALogPerf();
	now = XboxNV2ATicks();
	if (xboxNV2APerf.lastBegin) {
		xboxNV2APerf.frameTicks += now - xboxNV2APerf.lastBegin;
		xboxNV2APerf.frames++;
	}
	xboxNV2APerf.lastBegin = now;
	/* pbkit rotates 3 buffers; this frame's one leaves the screen with the flip queued two frames ago. */
	if (xboxNV2ASwapInterval->integer == 1) {
		pb_wait_for_vbl();
	} else if (xboxNV2AFlipsQueued >= 2) {
		while ((int)(pb_get_vbl_counter() - xboxNV2AFlipShow[0]) < 0)
			pb_wait_for_vbl();
	}
	xboxNV2APerf.vblankTicks += XboxNV2ATicks() - now;
	pb_reset();
	pb_target_back_buffer();
	xboxNV2APerf.endWaitTicks += XboxNV2AWaitIdle("begin frame");
	xboxNV2APerf.sceneStart = XboxNV2ATicks();
	xboxNV2APushedDwords = 0;
	xboxNV2AVertexUsed = 0;
	xboxNV2ATess.shader = 0;
	xboxNV2ATess.numVerts = 0;
	xboxNV2ATess.numIndexes = 0;
	xboxNV2ATess.numFast = 0;
	xboxNV2ATess.fastVerts = 0;
	xboxNV2ATess.fastIndexes = 0;
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
			XBOX_NV2A_WHITE_IMAGE, XBOX_NV2A_WHITE_IMAGE, qtrue, qfalse, qfalse,
			XBOX_NV2A_CULL_NONE, qfalse);
		XboxNV2ADrawVertices(XGU_TRIANGLES, xboxNV2AVertexUsed,
			XBOX_NV2A_TRIANGLE_VERTS);
		xboxNV2AVertexUsed += XBOX_NV2A_TRIANGLE_VERTS;
	}
}

void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec)
{
	DWORD start;
	DWORD show;
	LONGLONG waitStart;

	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (!xboxNV2AInitialized || !xboxNV2AInFrame)
		return;

	XboxNV2AEndSurface();
	xboxNV2APerf.sceneTicks += XboxNV2ATicks() - xboxNV2APerf.sceneStart;
	/* ioq3 R_InitNextFrame: scene entities, dlights and polys last one frame. */
	xboxNV2ASceneEntityCount = 0;
	xboxNV2ASceneFirstEntity = 0;
	xboxNV2ADlightCount = 0;
	xboxNV2ASceneFirstDlight = 0;
	xboxNV2APolyCount = 0;
	xboxNV2APolyVertCount = 0;
	xboxNV2ASceneFirstPoly = 0;
	xboxNV2AInFrame = qfalse;
	/* A flip queued after vblank show - 1 appears at show, so the last frame stays N vblanks. */
	if (xboxNV2ASwapInterval->integer > 1 && xboxNV2AFlipsQueued) {
		DWORD earliest = xboxNV2AFlipShow[1] + (DWORD)(xboxNV2ASwapInterval->integer - 1);

		waitStart = XboxNV2ATicks();
		while ((int)(pb_get_vbl_counter() - earliest) < 0)
			pb_wait_for_vbl();
		xboxNV2APerf.vblankTicks += XboxNV2ATicks() - waitStart;
	}
	waitStart = XboxNV2ATicks();
	XboxNV2AWaitIdle("end frame");
	if (XBOX_NV2A_DIAGNOSTICS)
		pb_draw_text_screen();
	start = GetTickCount();
	while (pb_finished()) {
		if (GetTickCount() - start > XBOX_NV2A_GPU_TIMEOUT_MS)
			Sys_Error("Xbox NV2A: flip timeout, frame %u", xboxNV2AFrameCount);
	}
	/* Idle means the flip interrupt ran, so it shows at the next vblank or after the one before it. */
	XboxNV2AWaitIdle("flip");
	show = pb_get_vbl_counter() + 1;
	if (xboxNV2AFlipsQueued && (int)(show - xboxNV2AFlipShow[1]) <= 0)
		show = xboxNV2AFlipShow[1] + 1;
	xboxNV2AFlipShow[0] = xboxNV2AFlipShow[1];
	xboxNV2AFlipShow[1] = show;
	if (xboxNV2AFlipsQueued < 2)
		xboxNV2AFlipsQueued++;
	xboxNV2APerf.endWaitTicks += XboxNV2ATicks() - waitStart;
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
		tess->fogIndex = 0;
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
	xboxNV2APerf.midWaitTicks += XboxNV2AWaitIdle("cinematic upload");
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
	xboxNV2ABoundImage[0] = xboxNV2ABoundImage[1] = -1;
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
	XboxNV2AApplyStageState(&stage, XBOX_NV2A_MAX_IMAGES + client, XBOX_NV2A_WHITE_IMAGE, qtrue,
		qfalse, qfalse, XBOX_NV2A_CULL_NONE, qfalse);

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
			out[i].color[k] = 255;
		out[i].texcoord[0] = corners[i][0] ? s2 : s1;
		out[i].texcoord[1] = corners[i][1] ? t2 : t1;
		out[i].texcoord1[0] = 0.0f;
		out[i].texcoord1[1] = 0.0f;
	}
	XboxNV2ADrawVertices(XGU_QUADS, xboxNV2AVertexUsed, 4);
	xboxNV2AVertexUsed += 4;
}


void XboxNV2A_ClearScene(void)
{
	xboxNV2ASceneFirstEntity = xboxNV2ASceneEntityCount;
	xboxNV2ASceneFirstDlight = xboxNV2ADlightCount;
	xboxNV2ASceneFirstPoly = xboxNV2APolyCount;
}

void XboxNV2A_AddRefEntity(const refEntity_t *entity)
{
	if (!entity || xboxNV2ASceneEntityCount >= XBOX_NV2A_MAX_SCENE_ENTITIES)
		return;
	xboxNV2ASceneEntities[xboxNV2ASceneEntityCount++] = *entity;
}

/* ioq3 RE_AddDynamicLightToScene; additive lights get the normal blend, as baseq3 never adds one. */
void XboxNV2A_AddLight(const vec3_t origin, float intensity, float r, float g,
	float b)
{
	xboxNV2ADlight_t *light;

	if (intensity <= 0.0f || xboxNV2ADlightCount >= XBOX_NV2A_MAX_DLIGHTS)
		return;
	light = &xboxNV2ADlights[xboxNV2ADlightCount++];
	VectorCopy(origin, light->origin);
	light->radius = intensity;
	VectorSet(light->color, r, g, b);
}

/* The current scene's dlights; bit i of a surface's dlightBits is dlights[i]. */
int XboxNV2A_SceneDlights(const xboxNV2ADlight_t **dlights)
{
	*dlights = &xboxNV2ADlights[xboxNV2ASceneFirstDlight];
	return xboxNV2ADlightCount - xboxNV2ASceneFirstDlight;
}

/* ioq3 RE_AddPolyToScene: polys last one frame and use the world transform. */
void XboxNV2A_AddPoly(qhandle_t shader, int numVerts, const polyVert_t *verts,
	int numPolys)
{
	int i;

	if (!xboxNV2AInitialized || !verts || numVerts < 3)
		return;
	for (i = 0; i < numPolys; ++i) {
		XboxNV2APoly *poly;

		if (xboxNV2APolyVertCount + numVerts > XBOX_NV2A_MAX_POLYVERTS ||
			xboxNV2APolyCount >= XBOX_NV2A_MAX_POLYS)
			return;
		poly = &xboxNV2APolys[xboxNV2APolyCount++];
		poly->shader = shader;
		poly->numVerts = numVerts;
		poly->verts = &xboxNV2APolyVerts[xboxNV2APolyVertCount];
		memcpy(&xboxNV2APolyVerts[xboxNV2APolyVertCount], &verts[numVerts * i],
			(size_t)numVerts * sizeof(*verts));
		xboxNV2APolyVertCount += numVerts;
	}
}

/* DIAGNOSTIC: logs the first surface of each generated type for the hardware test. */
static void XboxNV2ATraceSurfaceType(int bit, const char *kind, int shader)
{
	if (xboxNV2ATracedTypes & (1u << bit))
		return;
	xboxNV2ATracedTypes |= 1u << bit;
	Sys_XboxLog("Xbox NV2A: frame %u first %s, shader %s drawable=%d\n", xboxNV2AFrameCount,
		kind, shader > 0 && (unsigned int)shader < xboxNV2AShaderCount ?
		xboxNV2AShaders[shader].name : "none", XboxNV2A_ShaderIsDrawable(shader));
}

static void XboxNV2AAddDrawSurf(const md3Surface_t *md3,
	const xboxNV2AWorldSurface_t *world, const XboxNV2APoly *poly, int entity, int shader,
	int lightmap, int fogIndex)
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
	drawSurf->poly = poly;
	drawSurf->entity = entity;
	drawSurf->shader = shader;
	drawSurf->lightmap = lightmap;
	drawSurf->fogIndex = fogIndex;
	/* ioq3 keeps only whether a surface is dlit in the sort key; the bits stay on the surface. */
	drawSurf->dlighted = world && world->dlightBits;
	drawSurf->sort = xboxNV2AShaders[shader].sort;
	drawSurf->order = xboxNV2ADrawSurfCount++;
}

void XboxNV2A_AddWorldSurface(const xboxNV2AWorldSurface_t *surface, int entity)
{
	XboxNV2AAddDrawSurf(NULL, surface, NULL, entity, surface->shader, surface->lightmap,
		surface->fogIndex);
}

/* Skies stay in ascending shader order, the order ioq3's sort keys draw them in. */
void XboxNV2A_AddSkySurface(const xboxNV2AWorldSurface_t *surface)
{
	XboxNV2ASky *sky = NULL;
	int i;

	for (i = 0; i < xboxNV2ASkyCount && !sky; ++i) {
		if (xboxNV2ASkies[i].shader == surface->shader)
			sky = &xboxNV2ASkies[i];
	}
	if (!sky) {
		if (xboxNV2ASkyCount >= XBOX_NV2A_MAX_SKIES)
			return;
		for (i = xboxNV2ASkyCount; i > 0 && xboxNV2ASkies[i - 1].shader > surface->shader; --i)
			xboxNV2ASkies[i] = xboxNV2ASkies[i - 1];
		sky = &xboxNV2ASkies[i];
		sky->shader = surface->shader;
		XboxNV2ASky_Clear(&sky->bounds);
		xboxNV2ASkyCount++;
	}
	XboxNV2ASky_AddSurface(&sky->bounds, surface, xboxNV2ASkyOrigin);
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

/* ioq3 R_SpriteFogNum and R_ComputeFogNum: the first fog volume the sphere is inside. */
static int XboxNV2AFogForSphere(const refdef_t *fd, const vec3_t origin, float radius)
{
	int i, j;

	if (fd->rdflags & RDF_NOWORLDMODEL)
		return 0;
	for (i = 1; i < XboxNV2AWorld_NumFogs(); ++i) {
		const xboxNV2AFog_t *fog = XboxNV2AWorld_Fog(i);

		for (j = 0; j < 3; ++j) {
			if (origin[j] - radius >= fog->bounds[1][j] ||
				origin[j] + radius <= fog->bounds[0][j])
				break;
		}
		if (j == 3)
			return i;
	}
	return 0;
}

/* ioq3 RE_AddPolyToScene: the first fog volume whose bounds touch the poly's. */
static int XboxNV2AFogForPoly(const XboxNV2APoly *poly)
{
	vec3_t bounds[2];
	int i;

	VectorCopy(poly->verts[0].xyz, bounds[0]);
	VectorCopy(poly->verts[0].xyz, bounds[1]);
	for (i = 1; i < poly->numVerts; ++i)
		AddPointToBounds(poly->verts[i].xyz, bounds[0], bounds[1]);
	for (i = 1; i < XboxNV2AWorld_NumFogs(); ++i) {
		const xboxNV2AFog_t *fog = XboxNV2AWorld_Fog(i);

		if (bounds[1][0] >= fog->bounds[0][0] && bounds[1][1] >= fog->bounds[0][1] &&
			bounds[1][2] >= fog->bounds[0][2] && bounds[0][0] <= fog->bounds[1][0] &&
			bounds[0][1] <= fog->bounds[1][1] && bounds[0][2] <= fog->bounds[1][2])
			return i;
	}
	return 0;
}

/* ioq3 R_AddPolygonSurfaces: every scene poly is a world-entity draw surface. */
static void XboxNV2AAddPolySurfaces(void)
{
	int i;

	for (i = xboxNV2ASceneFirstPoly; i < xboxNV2APolyCount; ++i) {
		const XboxNV2APoly *poly = &xboxNV2APolys[i];

		XboxNV2ATraceSurfaceType(XBOX_NV2A_TRACE_POLY, "poly", poly->shader);
		if (XboxNV2A_ShaderIsDrawable(poly->shader))
			XboxNV2AAddDrawSurf(NULL, NULL, poly, XBOX_NV2A_WORLD_ENTITY, poly->shader, 0,
				XboxNV2AFogForPoly(poly));
	}
}

static void XboxNV2AAddEntitySurfaces(const refdef_t *fd)
{
	static const char *kinds[] = {"model", "poly entity", "sprite", "beam", "rail core",
		"rail rings", "lightning"};
	int e, s;

	for (e = xboxNV2ASceneFirstEntity; e < xboxNV2ASceneEntityCount; ++e) {
		refEntity_t *entity = &xboxNV2ASceneEntities[e];
		const md3Header_t *md3;
		const md3Surface_t *surface;
		const md3Frame_t *frame;
		vec3_t localOrigin;
		int brush, fogIndex;

		/* ioq3: the first-person weapon never shows in a mirror or portal view. */
		if ((entity->renderfx & RF_FIRST_PERSON) && xboxNV2AViewIsPortal)
			continue;
		switch (entity->reType) {
		case RT_SPRITE:
		case RT_BEAM:
		case RT_LIGHTNING:
		case RT_RAIL_CORE:
		case RT_RAIL_RINGS:
			/* ioq3 R_AddEntitySurfaces: one generated surface, never culled; RF_THIRD_PERSON hides it. */
			XboxNV2ATraceSurfaceType(entity->reType, kinds[entity->reType], entity->customShader);
			if ((entity->renderfx & RF_THIRD_PERSON) && !xboxNV2AViewIsPortal)
				continue;
			fogIndex = (entity->renderfx & RF_CROSSHAIR) ? 0 :
				XboxNV2AFogForSphere(fd, entity->origin, entity->radius);
			/* RT_BEAM ignores its shader, as ioq3 RB_SurfaceBeam draws it straight to GL. */
			if (XboxNV2A_ShaderIsDrawable(entity->customShader))
				XboxNV2AAddDrawSurf(NULL, NULL, NULL, e, entity->customShader, 0, fogIndex);
			else if (entity->reType == RT_BEAM)
				XboxNV2AAddDrawSurf(NULL, NULL, NULL, e, 0, 0, fogIndex);
			continue;
		case RT_MODEL:
			break;
		default:
			continue;
		}
		brush = XboxNV2AModel_BrushIndex(entity->hModel);
		if (brush >= 0) {
			XboxNV2AWorld_AddBrushModel(brush, e, entity);
			continue;
		}
		/* ioq3 R_AddMD3Surfaces: the player's own body only shows through portals. */
		md3 = XboxNV2AModel_Get(entity->hModel);
		if (!md3 || ((entity->renderfx & RF_THIRD_PERSON) && !xboxNV2AViewIsPortal))
			continue;
		if (entity->frame < 0 || entity->frame >= md3->numFrames ||
			entity->oldframe < 0 || entity->oldframe >= md3->numFrames) {
			entity->frame = 0;
			entity->oldframe = 0;
		}
		frame = (const md3Frame_t *)((const byte *)md3 + md3->ofsFrames) + entity->frame;
		VectorAdd(entity->origin, frame->localOrigin, localOrigin);
		fogIndex = XboxNV2AFogForSphere(fd, localOrigin, frame->radius);
		surface = (const md3Surface_t *)((const byte *)md3 + md3->ofsSurfaces);
		for (s = 0; s < md3->numSurfaces; ++s) {
			int shader = XboxNV2AMD3SurfaceShader(entity, surface);

			if (XboxNV2A_ShaderIsDrawable(shader))
				XboxNV2AAddDrawSurf(surface, NULL, NULL, e, shader, 0, fogIndex);
			surface = (const md3Surface_t *)((const byte *)surface + surface->ofsEnd);
		}
	}
}

/* ioq3 sort key order: shader sort, shader, entity, fog, dlighted, then submission order. */
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
	if (x->fogIndex != y->fogIndex)
		return x->fogIndex - y->fogIndex;
	if (x->dlighted != y->dlighted)
		return x->dlighted - y->dlighted;
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
static void XboxNV2ASetupView(const refdef_t *fd, float zFar, float *worldProjection,
	float *worldToScreen)
{
	static const float flip[16] = {
		0, 0, -1, 0,
		-1, 0, 0, 0,
		0, 1, 0, 0,
		0, 0, 0, 1
	};
	float viewer[16], world[16], projection[16], viewport[16];
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
		const xboxNV2ADlight_t *light = &xboxNV2ADlights[i];
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

/* ioq3 RF_DEPTHHACK: screen z scaled like the sky's, so the view weapon stays out of walls. */
static void XboxNV2ADepthHack(const refEntity_t *entity, float *toScreen)
{
	int i;

	if (!(entity->renderfx & RF_DEPTHHACK))
		return;
	/* DIAGNOSTIC: the first depth-hacked entity, for the hardware test log. */
	if (!(xboxNV2ATracedTypes & (1u << XBOX_NV2A_TRACE_DEPTHHACK))) {
		xboxNV2ATracedTypes |= 1u << XBOX_NV2A_TRACE_DEPTHHACK;
		Sys_XboxLog("Xbox NV2A: frame %u first depthhack entity, type %d renderfx 0x%x\n",
			xboxNV2AFrameCount, entity->reType, (unsigned int)entity->renderfx);
	}
	for (i = 0; i < 4; ++i)
		toScreen[i * 4 + 2] *= XBOX_NV2A_DEPTHHACK_RANGE;
}

/* ioq3 R_RotateForEntity; the world and non-model entities keep the view transform, unlit. */
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

	if (entityIndex == XBOX_NV2A_WORLD_ENTITY ||
		xboxNV2ASceneEntities[entityIndex].reType != RT_MODEL) {
		memcpy(modelToScreen, worldToScreen, sizeof(modelToScreen));
		if (entityIndex != XBOX_NV2A_WORLD_ENTITY)
			XboxNV2ADepthHack(&xboxNV2ASceneEntities[entityIndex], modelToScreen);
		XboxNV2ASetTransform((const XguVec4 *)modelToScreen, qfalse);
		VectorCopy(fd->vieworg, tess->viewOrigin);
		VectorClear(tess->ambientLight);
		VectorClear(tess->directedLight);
		VectorClear(tess->lightDir);
		Vector4Copy(xboxNV2APortalPlane, xboxNV2APortalPlaneLocal);
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
	XboxNV2ADepthHack(entity, modelToScreen);
	XboxNV2ASetTransform((const XguVec4 *)modelToScreen, qfalse);
	/* n.(origin + axis * v) - d, rearranged into a plane on the model-space v. */
	for (i = 0; i < 3; ++i)
		xboxNV2APortalPlaneLocal[i] = DotProduct(xboxNV2APortalPlane, entity->axis[i]);
	xboxNV2APortalPlaneLocal[3] = xboxNV2APortalPlane[3] -
		DotProduct(xboxNV2APortalPlane, entity->origin);

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
	tess->dlightBits |= surface->dlightBits;
}

/* ioq3 RB_CheckOverflow: a full tess is drawn, then refilled under the same shader. */
static void XboxNV2ACheckOverflow(int verts, int indexes)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int shader = tess->shader;
	int lightmap = tess->lightmap;

	if (tess->numVerts + verts < XBOX_NV2A_TESS_VERTS &&
		tess->numIndexes + indexes < XBOX_NV2A_TESS_INDEXES)
		return;
	XboxNV2AEndSurface();
	tess->shader = shader;
	tess->lightmap = lightmap;
}

static int XboxNV2ATessVertex(const vec3_t xyz, float s, float t, const byte *color,
	const vec3_t normal)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int v = tess->numVerts++;

	VectorCopy(xyz, tess->xyz[v]);
	VectorCopy(normal, tess->normal[v]);
	tess->st[v][0] = s;
	tess->st[v][1] = t;
	tess->lightSt[v][0] = 0.0f;
	tess->lightSt[v][1] = 0.0f;
	memcpy(tess->color[v], color, 4);
	return v;
}

static void XboxNV2ATessTriangle(int a, int b, int c)
{
	XboxNV2ATess *tess = &xboxNV2ATess;

	tess->indexes[tess->numIndexes++] = (unsigned short)a;
	tess->indexes[tess->numIndexes++] = (unsigned short)b;
	tess->indexes[tess->numIndexes++] = (unsigned short)c;
}

/* ioq3 RB_SurfacePolychain: the poly is fanned into triangles. */
static void XboxNV2ATessPoly(const XboxNV2APoly *poly)
{
	int first = xboxNV2ATess.numVerts;
	int i;

	for (i = 0; i < poly->numVerts; ++i)
		XboxNV2ATessVertex(poly->verts[i].xyz, poly->verts[i].st[0], poly->verts[i].st[1],
			poly->verts[i].modulate, vec3_origin);
	for (i = 0; i < poly->numVerts - 2; ++i)
		XboxNV2ATessTriangle(first, first + i + 1, first + i + 2);
}

/* ioq3 RB_SurfaceSprite and RB_AddQuadStamp: a view-facing quad turned by rotation degrees. */
static void XboxNV2ATessSprite(const refdef_t *fd, const refEntity_t *e)
{
	vec3_t left, up, normal, corner;
	float radius = e->radius;
	int v;

	if (e->rotation == 0.0f) {
		VectorScale(fd->viewaxis[1], radius, left);
		VectorScale(fd->viewaxis[2], radius, up);
	} else {
		float angle = (float)M_PI * e->rotation / 180.0f;
		float s = sinf(angle);
		float c = cosf(angle);

		VectorScale(fd->viewaxis[1], c * radius, left);
		VectorMA(left, -s * radius, fd->viewaxis[2], left);
		VectorScale(fd->viewaxis[2], c * radius, up);
		VectorMA(up, s * radius, fd->viewaxis[1], up);
	}
	if (xboxNV2AViewIsMirror)
		VectorNegate(left, left);
	VectorNegate(fd->viewaxis[0], normal);
	XboxNV2ACheckOverflow(4, 6);
	VectorAdd(e->origin, left, corner);
	VectorAdd(corner, up, corner);
	v = XboxNV2ATessVertex(corner, 0.0f, 0.0f, e->shaderRGBA, normal);
	VectorSubtract(e->origin, left, corner);
	VectorAdd(corner, up, corner);
	XboxNV2ATessVertex(corner, 1.0f, 0.0f, e->shaderRGBA, normal);
	VectorSubtract(e->origin, left, corner);
	VectorSubtract(corner, up, corner);
	XboxNV2ATessVertex(corner, 1.0f, 1.0f, e->shaderRGBA, normal);
	VectorAdd(e->origin, left, corner);
	VectorSubtract(corner, up, corner);
	XboxNV2ATessVertex(corner, 0.0f, 1.0f, e->shaderRGBA, normal);
	XboxNV2ATessTriangle(v, v + 1, v + 3);
	XboxNV2ATessTriangle(v + 3, v + 1, v + 2);
}

/* ioq3 DoRailCore: a quad along the beam whose start edge has a quarter of the colour. */
static void XboxNV2ATessRailCore(const refEntity_t *e, const vec3_t start, const vec3_t end,
	const vec3_t up, float len, float spanWidth)
{
	byte dim[4], full[4];
	vec3_t point;
	float t = len / 256.0f;
	int v, k;

	XboxNV2ACheckOverflow(4, 6);
	for (k = 0; k < 3; ++k) {
		dim[k] = (byte)(e->shaderRGBA[k] * 0.25f);
		full[k] = e->shaderRGBA[k];
	}
	/* ioq3 leaves the alpha unset; the additive rail and lightning shaders never read it. */
	dim[3] = full[3] = 255;
	VectorMA(start, spanWidth, up, point);
	v = XboxNV2ATessVertex(point, 0.0f, 0.0f, dim, vec3_origin);
	VectorMA(start, -spanWidth, up, point);
	XboxNV2ATessVertex(point, 0.0f, 1.0f, full, vec3_origin);
	VectorMA(end, spanWidth, up, point);
	XboxNV2ATessVertex(point, t, 0.0f, full, vec3_origin);
	VectorMA(end, -spanWidth, up, point);
	XboxNV2ATessVertex(point, t, 1.0f, full, vec3_origin);
	XboxNV2ATessTriangle(v, v + 1, v + 2);
	XboxNV2ATessTriangle(v + 2, v + 1, v + 3);
}

/* The side vector of ioq3's rail core and lightning: across the beam as the viewer sees it. */
static void XboxNV2ABeamSide(const refdef_t *fd, const vec3_t start, const vec3_t end,
	vec3_t right)
{
	vec3_t v1, v2;

	VectorSubtract(start, fd->vieworg, v1);
	VectorNormalize(v1);
	VectorSubtract(end, fd->vieworg, v2);
	VectorNormalize(v2);
	CrossProduct(v1, v2, right);
	VectorNormalize(right);
}

/* ioq3 RB_SurfaceRailCore; like ioq3, the length is truncated to whole units. */
static void XboxNV2ATessRail(const refdef_t *fd, const refEntity_t *e)
{
	vec3_t vec, right;
	int len;

	VectorSubtract(e->origin, e->oldorigin, vec);
	len = (int)VectorNormalize(vec);
	XboxNV2ABeamSide(fd, e->oldorigin, e->origin, right);
	XboxNV2ATessRailCore(e, e->oldorigin, e->origin, right, (float)len,
		XBOX_NV2A_RAIL_CORE_WIDTH);
}

/* ioq3 RB_SurfaceLightningBolt: four 8-unit cores, 45 degrees apart around the bolt. */
static void XboxNV2ATessLightning(const refdef_t *fd, const refEntity_t *e)
{
	vec3_t vec, right, temp;
	int len, i;

	VectorSubtract(e->oldorigin, e->origin, vec);
	len = (int)VectorNormalize(vec);
	XboxNV2ABeamSide(fd, e->origin, e->oldorigin, right);
	for (i = 0; i < 4; ++i) {
		XboxNV2ATessRailCore(e, e->origin, e->oldorigin, right, (float)len, 8.0f);
		RotatePointAroundVector(temp, vec, right, 45.0f);
		VectorCopy(temp, right);
	}
}

/* ioq3 RB_SurfaceRailRings and DoRailDiscs: square rings one segment apart. */
static void XboxNV2ATessRailRings(const refEntity_t *e)
{
	vec3_t dir, right, up, pos[4];
	byte color[4];
	int len, numSegs, i, j;

	VectorSubtract(e->origin, e->oldorigin, dir);
	len = (int)VectorNormalize(dir);
	MakeNormalVectors(dir, right, up);
	numSegs = (int)(len / XBOX_NV2A_RAIL_SEGMENT_LENGTH);
	if (numSegs <= 0)
		numSegs = 1;
	VectorScale(dir, XBOX_NV2A_RAIL_SEGMENT_LENGTH, dir);
	/* ioq3 starts long shots one segment out and draws one ring fewer. */
	if (numSegs > 1)
		numSegs--;
	for (i = 0; i < 4; ++i) {
		float c = cosf(DEG2RAD(45 + i * 90));
		float s = sinf(DEG2RAD(45 + i * 90));

		for (j = 0; j < 3; ++j)
			pos[i][j] = e->oldorigin[j] + (right[j] * c + up[j] * s) * 0.25f *
				XBOX_NV2A_RAIL_WIDTH + (numSegs > 1 ? dir[j] : 0.0f);
	}
	memcpy(color, e->shaderRGBA, 3);
	color[3] = 255;
	for (i = 0; i < numSegs; ++i) {
		int first;

		XboxNV2ACheckOverflow(4, 6);
		first = xboxNV2ATess.numVerts;
		for (j = 0; j < 4; ++j) {
			XboxNV2ATessVertex(pos[j], (float)(j < 2), (float)(j && j != 3), color, vec3_origin);
			VectorAdd(pos[j], dir, pos[j]);
		}
		XboxNV2ATessTriangle(first, first + 1, first + 3);
		XboxNV2ATessTriangle(first + 3, first + 1, first + 2);
	}
}

/* ioq3 RB_SurfaceBeam skips the shader: a red additive tube on the white image, drawn at once. */
static void XboxNV2ADrawBeam(const refEntity_t *e)
{
	static const byte white[4] = {255, 255, 255, 255};
	XboxNV2ATess *tess = &xboxNV2ATess;
	vec3_t direction, normalized, perp, start, end;
	xboxNV2AStage_t stage;
	int i;

	VectorSubtract(e->oldorigin, e->origin, direction);
	VectorCopy(direction, normalized);
	if (VectorNormalize(normalized) == 0.0f)
		return;
	PerpendicularVector(perp, normalized);
	VectorScale(perp, 4.0f, perp);
	/* ioq3 has the origin add commented out, so the tube starts at the world origin. */
	for (i = 0; i <= XBOX_NV2A_BEAM_SEGS; ++i) {
		RotatePointAroundVector(start, normalized, perp,
			(360.0f / XBOX_NV2A_BEAM_SEGS) * (i % XBOX_NV2A_BEAM_SEGS));
		VectorAdd(start, direction, end);
		XboxNV2ATessVertex(start, 0.0f, 0.0f, white, vec3_origin);
		XboxNV2ATessVertex(end, 0.0f, 0.0f, white, vec3_origin);
	}
	/* The GL triangle strip as triangles; the cull is off, so winding does not matter. */
	for (i = 0; i < 2 * XBOX_NV2A_BEAM_SEGS; ++i)
		XboxNV2ATessTriangle(i, i + 1, i + 2);
	memset(&stage, 0, sizeof(stage));
	stage.images[0] = XBOX_NV2A_WHITE_IMAGE;
	stage.numImages = 1;
	stage.srcBlend = GL_ONE;
	stage.dstBlend = GL_ONE;
	stage.rgbGen = XBOX_NV2A_RGBGEN_CONST;
	stage.constant[0] = 255;
	if (XboxNV2AComputeNearDistances())
		XboxNV2ADrawStage(&stage, NULL, XBOX_NV2A_CULL_NONE, qfalse);
	tess->numVerts = 0;
	tess->numIndexes = 0;
}

/* ioq3 RB_SurfaceEntity. */
static void XboxNV2ATessEntity(const refdef_t *fd, const refEntity_t *entity)
{
	switch (entity->reType) {
	case RT_SPRITE:
		XboxNV2ATessSprite(fd, entity);
		break;
	case RT_BEAM:
		XboxNV2ADrawBeam(entity);
		break;
	case RT_RAIL_CORE:
		XboxNV2ATessRail(fd, entity);
		break;
	case RT_RAIL_RINGS:
		XboxNV2ATessRailRings(entity);
		break;
	case RT_LIGHTNING:
		XboxNV2ATessLightning(fd, entity);
		break;
	default:
		break;
	}
}

/* ioq3 RB_RenderDrawSurfList: world surfaces and polys with equal shader, entity and fog batch. */
static void XboxNV2ADrawSurfaces(const refdef_t *fd, const float *worldToScreen, int first,
	int last)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int currentEntity = XBOX_NV2A_WORLD_ENTITY - 1;
	int i;

	for (i = first; i < last; ++i) {
		const XboxNV2ADrawSurf *drawSurf = &xboxNV2ADrawSurfs[i];
		const refEntity_t *entity = drawSurf->entity == XBOX_NV2A_WORLD_ENTITY ? NULL :
			&xboxNV2ASceneEntities[drawSurf->entity];
		/* A new entity always restarts the tess, even for a beam whose shader is 0. */
		qboolean restart = drawSurf->md3 != NULL;
		int verts = 0;
		int indexes = 0;

		if (drawSurf->entity != currentEntity) {
			XboxNV2AEndSurface();
			XboxNV2ASetupEntity(fd, drawSurf->entity, worldToScreen);
			currentEntity = drawSurf->entity;
			restart = qtrue;
		}
		if (drawSurf->world) {
			verts = drawSurf->world->numVerts;
			indexes = drawSurf->world->numIndexes;
		} else if (drawSurf->poly) {
			verts = drawSurf->poly->numVerts;
			indexes = 3 * (drawSurf->poly->numVerts - 2);
		}
		if (restart || tess->shader != drawSurf->shader ||
			tess->lightmap != drawSurf->lightmap || tess->fogIndex != drawSurf->fogIndex ||
			tess->dlighted != drawSurf->dlighted ||
			tess->numVerts + verts > XBOX_NV2A_TESS_VERTS ||
			tess->numIndexes + indexes > XBOX_NV2A_TESS_INDEXES) {
			XboxNV2AEndSurface();
			tess->shader = drawSurf->shader;
			tess->lightmap = drawSurf->lightmap;
			tess->fogIndex = drawSurf->fogIndex;
			tess->dlighted = drawSurf->dlighted;
			tess->is3D = qtrue;
			tess->entity = entity;
			tess->shaderTime = fd->time * 0.001 - (entity ? entity->shaderTime : 0.0f);
		}
		if (drawSurf->md3) {
			XboxNV2ATessSurface(entity, drawSurf->md3);
			XboxNV2AEndSurface();
		} else if (drawSurf->world) {
			if (!XboxNV2AAddFastSurface(drawSurf->world))
				XboxNV2ATessWorldSurface(drawSurf->world);
		} else if (drawSurf->poly) {
			if (verts <= XBOX_NV2A_TESS_VERTS)
				XboxNV2ATessPoly(drawSurf->poly);
		} else {
			XboxNV2ATessEntity(fd, entity);
		}
	}
	XboxNV2AEndSurface();
}

/* Moves a sky grid in the tess around the view origin; qfalse when nothing is left to draw. */
static qboolean XboxNV2APrepareSkyTess(void)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	int i;

	for (i = 0; i < tess->numVerts; ++i) {
		VectorAdd(tess->xyz[i], xboxNV2ASkyOrigin, tess->xyz[i]);
		VectorClear(tess->normal[i]);
		memset(tess->color[i], 0xff, 4);
	}
	return tess->numVerts > 0 && tess->numIndexes > 0 && XboxNV2AComputeNearDistances();
}

/* ioq3 RB_StageIteratorSky: the outer box with clamped side images, then the cloud stages. */
static void XboxNV2ADrawSky(const refdef_t *fd, float zFar, const XboxNV2ASky *sky)
{
	XboxNV2ATess *tess = &xboxNV2ATess;
	const XboxNV2AShader *shader = &xboxNV2AShaders[sky->shader];
	/* ioq3 MakeSkyVec: the box corners stay inside zFar (div sqrt(3)). */
	float boxSize = zFar / 1.75f;
	int i;

	tess->is3D = qtrue;
	tess->isSky = qtrue;
	tess->entity = NULL;
	tess->lightmap = 0;
	tess->fogIndex = 0;
	tess->shaderTime = fd->time * 0.001;
	xboxNV2ALastShaderName = shader->name;
	if (shader->skyBox[0] > 0) {
		xboxNV2AStage_t stage;

		memset(&stage, 0, sizeof(stage));
		stage.numImages = 1;
		stage.clamp = qtrue;
		stage.srcBlend = GL_ONE;
		stage.dstBlend = GL_ZERO;
		stage.rgbGen = XBOX_NV2A_RGBGEN_IDENTITY;
		for (i = 0; i < XBOX_NV2A_SKY_SIDES; ++i) {
			int image = shader->skyBox[xboxNV2ASkyTexOrder[i]];

			tess->numVerts = XboxNV2ASky_BoxSide(&sky->bounds, i, boxSize, tess->xyz, tess->st,
				tess->indexes, &tess->numIndexes);
			if (!XboxNV2APrepareSkyTess())
				continue;
			/* ioq3 draws tr.defaultImage for a missing side; white stands in for it here. */
			stage.images[0] = image > 0 ? image : XBOX_NV2A_WHITE_IMAGE;
			XboxNV2ADrawStage(&stage, NULL, XBOX_NV2A_CULL_NONE, qfalse);
		}
	}
	tess->numVerts = XboxNV2ASky_Clouds(&sky->bounds, shader->cloudHeight, boxSize, tess->xyz,
		tess->st, tess->indexes, &tess->numIndexes);
	if (XboxNV2APrepareSkyTess()) {
		XboxNV2ADrawShaderStages(shader, XBOX_NV2A_CULL_NONE, qfalse);
	}
	tess->isSky = qfalse;
	tess->shader = 0;
	tess->numVerts = 0;
	tess->numIndexes = 0;
}

/* ioq3 R_PlaneForSurface: faces keep their plane, soups use a triangle, grids get +X. */
static void XboxNV2APlaneForSurface(const xboxNV2AWorldSurface_t *surface, cplane_t *plane)
{
	vec4_t plane4;

	memset(plane, 0, sizeof(*plane));
	if (surface->type == XBOX_NV2A_SURFACE_FACE) {
		*plane = surface->plane;
		return;
	}
	if (surface->type == XBOX_NV2A_SURFACE_TRIANGLES && surface->numIndexes >= 3 &&
		PlaneFromPoints(plane4, surface->verts[surface->indexes[0]].xyz,
		surface->verts[surface->indexes[1]].xyz, surface->verts[surface->indexes[2]].xyz)) {
		VectorCopy(plane4, plane->normal);
		plane->dist = plane4[3];
		return;
	}
	plane->normal[0] = 1.0f;
}

/* ioq3 IsMirror: the nearest portal entity within 64 units is its own camera. */
static qboolean XboxNV2AIsMirror(const XboxNV2ADrawSurf *drawSurf)
{
	cplane_t plane;
	int i;

	XboxNV2APlaneForSurface(drawSurf->world, &plane);
	if (drawSurf->entity != XBOX_NV2A_WORLD_ENTITY)
		plane.dist += DotProduct(plane.normal, xboxNV2ASceneEntities[drawSurf->entity].origin);
	for (i = xboxNV2ASceneFirstEntity; i < xboxNV2ASceneEntityCount; ++i) {
		const refEntity_t *e = &xboxNV2ASceneEntities[i];
		float d;

		if (e->reType != RT_PORTALSURFACE)
			continue;
		d = DotProduct(e->origin, plane.normal) - plane.dist;
		if (d > 64.0f || d < -64.0f)
			continue;
		return VectorCompare(e->oldorigin, e->origin);
	}
	return qfalse;
}

/* ioq3 SurfIsOffscreen: world-space clip test, backface test and the portal range. */
static qboolean XboxNV2ASurfIsOffscreen(const refdef_t *fd, const XboxNV2ADrawSurf *drawSurf,
	const float *worldProjection)
{
	const xboxNV2AWorldSurface_t *surface = drawSurf->world;
	unsigned int pointAnd = ~0u;
	float shortest = 100000000.0f;
	float range;
	int numTriangles, i, j;

	for (i = 0; i < surface->numVerts; ++i) {
		const float *v = surface->verts[i].xyz;
		unsigned int pointFlags = 0;
		float clip[4];

		for (j = 0; j < 4; ++j)
			clip[j] = v[0] * worldProjection[j] + v[1] * worldProjection[4 + j] +
				v[2] * worldProjection[8 + j] + worldProjection[12 + j];
		for (j = 0; j < 3; ++j) {
			if (clip[j] >= clip[3])
				pointFlags |= 1u << (j * 2);
			else if (clip[j] <= -clip[3])
				pointFlags |= 1u << (j * 2 + 1);
		}
		pointAnd &= pointFlags;
	}
	if (pointAnd)
		return qtrue;
	numTriangles = surface->numIndexes / 3;
	for (i = 0; i + 2 < surface->numIndexes; i += 3) {
		const xboxNV2AWorldVert_t *v = &surface->verts[surface->indexes[i]];
		vec3_t toVertex;
		float len;

		VectorSubtract(v->xyz, fd->vieworg, toVertex);
		len = VectorLengthSquared(toVertex);
		if (len < shortest)
			shortest = len;
		if (DotProduct(toVertex, v->normal) >= 0.0f)
			numTriangles--;
	}
	if (!numTriangles)
		return qtrue;
	/* Mirrors do not fade with distance, so only portals have a range. */
	if (XboxNV2AIsMirror(drawSurf))
		return qfalse;
	range = xboxNV2AShaders[drawSurf->shader].portalRange;
	return shortest > range * range;
}

/* ioq3 R_GetPortalOrientations: the surface frame and the camera frame it maps onto. */
static qboolean XboxNV2AGetPortalOrientations(const refdef_t *fd,
	const XboxNV2ADrawSurf *drawSurf, orientation_t *surface, orientation_t *camera,
	vec3_t pvsOrigin, qboolean *mirror)
{
	cplane_t originalPlane, plane;
	int i, j;

	XboxNV2APlaneForSurface(drawSurf->world, &originalPlane);
	plane = originalPlane;
	if (drawSurf->entity != XBOX_NV2A_WORLD_ENTITY) {
		const refEntity_t *e = &xboxNV2ASceneEntities[drawSurf->entity];

		/* The rotated plane frames the view; the unrotated one matches portal entities. */
		for (i = 0; i < 3; ++i)
			plane.normal[i] = originalPlane.normal[0] * e->axis[0][i] +
				originalPlane.normal[1] * e->axis[1][i] + originalPlane.normal[2] * e->axis[2][i];
		plane.dist = originalPlane.dist + DotProduct(plane.normal, e->origin);
		originalPlane.dist += DotProduct(originalPlane.normal, e->origin);
	}
	VectorCopy(plane.normal, surface->axis[0]);
	PerpendicularVector(surface->axis[1], surface->axis[0]);
	CrossProduct(surface->axis[0], surface->axis[1], surface->axis[2]);

	for (i = xboxNV2ASceneFirstEntity; i < xboxNV2ASceneEntityCount; ++i) {
		const refEntity_t *e = &xboxNV2ASceneEntities[i];
		vec3_t transformed;
		float d;

		if (e->reType != RT_PORTALSURFACE)
			continue;
		d = DotProduct(e->origin, originalPlane.normal) - originalPlane.dist;
		if (d > 64.0f || d < -64.0f)
			continue;
		VectorCopy(e->oldorigin, pvsOrigin);
		if (VectorCompare(e->oldorigin, e->origin)) {
			VectorScale(plane.normal, plane.dist, surface->origin);
			VectorCopy(surface->origin, camera->origin);
			VectorNegate(surface->axis[0], camera->axis[0]);
			VectorCopy(surface->axis[1], camera->axis[1]);
			VectorCopy(surface->axis[2], camera->axis[2]);
			*mirror = qtrue;
			return qtrue;
		}
		/* The entity origin projected onto the plane is the point the view turns around. */
		d = DotProduct(e->origin, plane.normal) - plane.dist;
		VectorMA(e->origin, -d, surface->axis[0], surface->origin);
		VectorCopy(e->oldorigin, camera->origin);
		for (j = 0; j < 3; ++j)
			VectorCopy(e->axis[j], camera->axis[j]);
		VectorNegate(camera->axis[0], camera->axis[0]);
		VectorNegate(camera->axis[1], camera->axis[1]);
		/* oldframe turns the camera: frame is a speed, else skinNum offsets a bob. */
		d = 0.0f;
		if (e->oldframe)
			d = e->frame ? (fd->time / 1000.0f) * e->frame :
				e->skinNum + sinf(fd->time * 0.003f) * 4;
		else if (e->skinNum)
			d = (float)e->skinNum;
		if (e->oldframe || e->skinNum) {
			VectorCopy(camera->axis[1], transformed);
			RotatePointAroundVector(camera->axis[1], camera->axis[0], transformed, d);
			CrossProduct(camera->axis[0], camera->axis[1], camera->axis[2]);
		}
		*mirror = qfalse;
		return qtrue;
	}
	/* No portal entity yet: the snapshot may not have arrived, so draw nothing. */
	return qfalse;
}

/* ioq3 R_MirrorPoint and R_MirrorVector: surface-frame coordinates reused in the camera frame. */
static void XboxNV2AMirrorVector(const vec3_t in, const orientation_t *surface,
	const orientation_t *camera, vec3_t out)
{
	int i;

	VectorClear(out);
	for (i = 0; i < 3; ++i)
		VectorMA(out, DotProduct(in, surface->axis[i]), camera->axis[i], out);
}

static void XboxNV2AMirrorPoint(const vec3_t in, const orientation_t *surface,
	const orientation_t *camera, vec3_t out)
{
	vec3_t local;

	VectorSubtract(in, surface->origin, local);
	XboxNV2AMirrorVector(local, surface, camera, out);
	VectorAdd(out, camera->origin, out);
}

static void XboxNV2ARenderView(const refdef_t *fd, const vec3_t pvsOrigin);

/* ioq3 R_MirrorViewBySurface: renders the mirror or portal camera's view before this one. */
static qboolean XboxNV2AMirrorViewBySurface(const refdef_t *fd,
	const XboxNV2ADrawSurf *drawSurf, const float *worldProjection)
{
	XboxNV2ASky skies[XBOX_NV2A_MAX_SKIES];
	orientation_t surface, camera;
	vec3_t pvsOrigin, skyOrigin;
	refdef_t portal;
	qboolean mirror = qfalse;
	int skyCount, bit, i;

	if (!drawSurf->world || xboxNV2AShaders[drawSurf->shader].sort != XBOX_NV2A_SORT_PORTAL ||
		XboxNV2ASurfIsOffscreen(fd, drawSurf, worldProjection) ||
		!XboxNV2AGetPortalOrientations(fd, drawSurf, &surface, &camera, pvsOrigin, &mirror))
		return qfalse;
	portal = *fd;
	XboxNV2AMirrorPoint(fd->vieworg, &surface, &camera, portal.vieworg);
	for (i = 0; i < 3; ++i)
		XboxNV2AMirrorVector(fd->viewaxis[i], &surface, &camera, portal.viewaxis[i]);
	/* ioq3's clip plane keeps what lies in front of the camera plane. */
	VectorNegate(camera.axis[0], xboxNV2APortalPlane);
	xboxNV2APortalPlane[3] = DotProduct(camera.origin, xboxNV2APortalPlane);

	/* DIAGNOSTIC: the first mirror and portal view, for the hardware test log. */
	bit = mirror ? 29 : 30;
	if (!(xboxNV2ATracedTypes & (1u << bit))) {
		xboxNV2ATracedTypes |= 1u << bit;
		Sys_XboxLog("Xbox NV2A: frame %u first %s view through %s\n", xboxNV2AFrameCount,
			mirror ? "mirror" : "portal", xboxNV2AShaders[drawSurf->shader].name);
	}

	/* The portal view rebuilds the sky bounds, so this view's are kept around it. */
	memcpy(skies, xboxNV2ASkies, sizeof(skies));
	skyCount = xboxNV2ASkyCount;
	VectorCopy(xboxNV2ASkyOrigin, skyOrigin);
	xboxNV2AViewIsPortal = qtrue;
	xboxNV2AViewIsMirror = mirror;
	XboxNV2ARenderView(&portal, pvsOrigin);
	xboxNV2AViewIsPortal = qfalse;
	xboxNV2AViewIsMirror = qfalse;
	memcpy(xboxNV2ASkies, skies, sizeof(skies));
	xboxNV2ASkyCount = skyCount;
	VectorCopy(skyOrigin, xboxNV2ASkyOrigin);
	return qtrue;
}

/* ioq3 R_RenderView: gathers and sorts one view, renders a portal view first, then draws. */
static void XboxNV2ARenderView(const refdef_t *fd, const vec3_t pvsOrigin)
{
	float worldProjection[16], worldToScreen[16], skyToScreen[16];
	float zFar = XBOX_NV2A_ZFAR;
	qboolean world = !(fd->rdflags & RDF_NOWORLDMODEL) && XboxNV2AWorld_Loaded();
	/* ioq3 r_fastsky: a black clear instead of the sky, and no portal views at 1. */
	qboolean fastSky = world && xboxNV2AFastSky && xboxNV2AFastSky->integer;
	int first = xboxNV2ADrawSurfCount;
	int worldSurfaces, signature, split, x, y, w, h, i;
	uint32_t *p;

	xboxNV2ASkyCount = 0;
	VectorCopy(fd->vieworg, xboxNV2ASkyOrigin);
	if (world)
		zFar = XboxNV2AWorld_AddSurfaces(fd, pvsOrigin);
	worldSurfaces = xboxNV2ADrawSurfCount - first;
	XboxNV2AAddPolySurfaces();
	XboxNV2AAddEntitySurfaces(fd);
	/* One line per new kind of scene: menu model, lit player model, world view. */
	signature = (worldSurfaces > 0) | ((xboxNV2ADlightCount - xboxNV2ASceneFirstDlight) << 1);
	if (!xboxNV2AViewIsPortal && xboxNV2ASceneCount < XBOX_NV2A_TRACE_SCENES &&
		xboxNV2ADrawSurfCount > first && signature != xboxNV2ALastSceneSignature) {
		xboxNV2ASceneCount++;
		xboxNV2ALastSceneSignature = signature;
		Sys_XboxLog("Xbox NV2A: frame %u scene %dx%d world=%d entities=%d dlights=%d "
			"surfaces=%d skies=%d zfar=%d fogs=%d\n", xboxNV2AFrameCount, fd->width, fd->height,
			worldSurfaces, xboxNV2ASceneEntityCount - xboxNV2ASceneFirstEntity,
			xboxNV2ADlightCount - xboxNV2ASceneFirstDlight, xboxNV2ADrawSurfCount - first,
			xboxNV2ASkyCount, (int)zFar, XboxNV2AWorld_NumFogs());
	}
	if (xboxNV2ADrawSurfCount == first && !xboxNV2ASkyCount)
		return;
	qsort(&xboxNV2ADrawSurfs[first], (size_t)(xboxNV2ADrawSurfCount - first),
		sizeof(xboxNV2ADrawSurfs[0]), XboxNV2ACompareDrawSurfs);
	XboxNV2ASetupView(fd, zFar, worldProjection, worldToScreen);
	if (!xboxNV2AViewIsPortal && !(fastSky && xboxNV2AFastSky->integer == 1)) {
		for (i = first; i < xboxNV2ADrawSurfCount &&
			xboxNV2ADrawSurfs[i].sort <= XBOX_NV2A_SORT_PORTAL; ++i) {
			if (XboxNV2AMirrorViewBySurface(fd, &xboxNV2ADrawSurfs[i], worldProjection))
				break;
		}
	}
	VectorCopy(fd->vieworg, xboxNV2AViewOrigin);
	for (i = 0; i < 3; ++i)
		VectorCopy(fd->viewaxis[i], xboxNV2AViewAxis[i]);
	xboxNV2AViewTime = fd->time;

	/* ioq3 RB_BeginDrawingView clears depth inside the view before drawing it. */
	x = fd->x < 0 ? 0 : fd->x;
	y = fd->y < 0 ? 0 : fd->y;
	w = (fd->x + fd->width > xboxNV2AWidth ? xboxNV2AWidth : fd->x + fd->width) - x;
	h = (fd->y + fd->height > xboxNV2AHeight ? xboxNV2AHeight : fd->y + fd->height) - y;
	if (w > 0 && h > 0) {
		XboxNV2AReserve(20);
		xgux_set_clear_rect((unsigned int)x, (unsigned int)y, (unsigned int)w,
			(unsigned int)h);
		p = pb_begin();
		p = xgu_set_zstencil_clear_value(p, 0xffffff00);
		p = xgu_set_color_clear_value(p, 0xff000000);
		p = xgu_clear_surface(p, fastSky ? XGU_CLEAR_Z | XGU_CLEAR_STENCIL | XGU_CLEAR_COLOR :
			XGU_CLEAR_Z | XGU_CLEAR_STENCIL);
		pb_end(p);
	}
	/* Portal surfaces write depth before the sky, which then stays out of them. */
	for (split = first; split < xboxNV2ADrawSurfCount &&
		xboxNV2ADrawSurfs[split].sort < XBOX_NV2A_SORT_ENVIRONMENT; ++split)
		;
	XboxNV2ADrawSurfaces(fd, worldToScreen, first, split);
	if (!fastSky && xboxNV2ASkyCount > 0) {
		/* z = SKY_DEPTH * w, so the sky lands just in front of the cleared far value. */
		memcpy(skyToScreen, worldToScreen, sizeof(skyToScreen));
		for (i = 0; i < 4; ++i)
			skyToScreen[i * 4 + 2] = skyToScreen[i * 4 + 3] * XBOX_NV2A_SKY_DEPTH;
		XboxNV2ASetupEntity(fd, XBOX_NV2A_WORLD_ENTITY, skyToScreen);
		for (i = 0; i < xboxNV2ASkyCount; ++i)
			XboxNV2ADrawSky(fd, zFar, &xboxNV2ASkies[i]);
	}
	XboxNV2ADrawSurfaces(fd, worldToScreen, split, xboxNV2ADrawSurfCount);
	xboxNV2ADrawSurfCount = first;
}

void XboxNV2A_RenderScene(const refdef_t *fd)
{
	if (!xboxNV2AInFrame || !fd)
		return;
	XboxNV2AEndSurface();
	/* ioq3 RE_RenderScene: r_dynamiclight 0 drops the scene's dlights, entity lighting included. */
	if (!xboxNV2ADynamicLight->integer)
		xboxNV2ASceneFirstDlight = xboxNV2ADlightCount;
	xboxNV2ADrawSurfCount = 0;
	xboxNV2AViewIsPortal = qfalse;
	xboxNV2AViewIsMirror = qfalse;
	XboxNV2ARenderView(fd, fd->vieworg);
	xboxNV2ASceneFirstEntity = xboxNV2ASceneEntityCount;
	xboxNV2ASceneFirstDlight = xboxNV2ADlightCount;
	xboxNV2ASceneFirstPoly = xboxNV2APolyCount;
}
