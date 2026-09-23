#include "xbox_nv2a.h"

#include "../qcommon/qcommon.h"
#include "../sys/sys_xbox.h"

#include <hal/video.h>
#include <stdint.h>
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
	xboxNV2AShaderState_t state;
} XboxNV2AShader;

#define XBOX_NV2A_MAX_IMAGES 512
#define XBOX_NV2A_MAX_SHADERS 1024
#define XBOX_NV2A_TEXTURE_POOL_BYTES (6u * 1024u * 1024u)
#define XBOX_NV2A_TEXTURE_ALIGN 128u
/* Highest physical address the xgu samples allow for GPU memory. */
#define XBOX_NV2A_MAX_RAM 0x03FFAFFF
#define XBOX_NV2A_TRIANGLE_VERTS 3
#define XBOX_NV2A_MAX_VERTS (2048u * 4u)
#define XBOX_NV2A_CLEAR_COLOR 0xff101820
/* Release pbkit does not check for overflow; restart well below its 512 KiB limit. */
#define XBOX_NV2A_PUSH_LIMIT_DWORDS (96u * 1024u)
#define XBOX_NV2A_STATE_DWORDS 32u

static XboxNV2AColoredVertex xboxNV2ATriangle[XBOX_NV2A_TRIANGLE_VERTS] = {
	{{0.0f, 0.0f, 1.0f}, {1.0f, 0.08f, 0.04f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 1.0f, 0.16f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 0.24f, 1.0f, 1.0f}, {0.0f, 0.0f}},
};

/* Vertices stream into contiguous memory; the GPU reads them with DRAW_ARRAYS. */
static XboxNV2AColoredVertex *xboxNV2AVertexMemory;
static unsigned int xboxNV2AVertexUsed;
static unsigned int xboxNV2ABatchQuads;
static int xboxNV2ABatchShader = -1;
static byte *xboxNV2ATexturePool;
static size_t xboxNV2ATexturePoolUsed;
static XboxNV2AImage xboxNV2AImages[XBOX_NV2A_MAX_IMAGES];
static unsigned int xboxNV2AImageCount;
static XboxNV2AShader xboxNV2AShaders[XBOX_NV2A_MAX_SHADERS];
static unsigned int xboxNV2AShaderCount;
static int xboxNV2ABoundImage;
static qboolean xboxNV2ABoundClamp;
static unsigned int xboxNV2ABoundSrcBlend;
static unsigned int xboxNV2ABoundDstBlend;
static int xboxNV2ABoundAlphaFunc;
static unsigned int xboxNV2APushedDwords;
static XguMatrix4x4 xboxNV2AScreenMatrix;
static int xboxNV2AWidth;
static int xboxNV2AHeight;
static qboolean xboxNV2AInitialized;
static qboolean xboxNV2AInFrame;
static qboolean xboxNV2ADebugScreen;
static float xboxNV2AColor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

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

/* DIAGNOSTIC: frame trace and GPU-wait timeout for the menu crash; remove after. */
#define XBOX_NV2A_TRACE_FRAMES 3
#define XBOX_NV2A_HEARTBEAT_FRAMES 600
#define XBOX_NV2A_GPU_TIMEOUT_MS 2000
static unsigned int xboxNV2AFrameCount;
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

static void XboxNV2AApplyShaderState(const xboxNV2AShaderState_t *state)
{
	uint32_t *p;

	XboxNV2AReserve(XBOX_NV2A_STATE_DWORDS);
	p = pb_begin();
	if (state->image != xboxNV2ABoundImage || state->clamp != xboxNV2ABoundClamp) {
		const XboxNV2AImage *image = &xboxNV2AImages[state->image];
		XguTextureAddress address = state->clamp ? XGU_CLAMP_TO_EDGE : XGU_WRAP;

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
		xboxNV2ABoundImage = state->image;
		xboxNV2ABoundClamp = state->clamp;
	}
	if (state->srcBlend != xboxNV2ABoundSrcBlend ||
		state->dstBlend != xboxNV2ABoundDstBlend) {
		p = xgu_set_blend_func_sfactor(p, XboxNV2ABlendFactor(state->srcBlend));
		p = xgu_set_blend_func_dfactor(p, XboxNV2ABlendFactor(state->dstBlend));
		xboxNV2ABoundSrcBlend = state->srcBlend;
		xboxNV2ABoundDstBlend = state->dstBlend;
	}
	if (state->alphaFunc != xboxNV2ABoundAlphaFunc) {
		p = xgu_set_alpha_test_enable(p, state->alphaFunc != XBOX_NV2A_ALPHA_NONE);
		if (state->alphaFunc == XBOX_NV2A_ALPHA_GT0) {
			p = xgu_set_alpha_func(p, XGU_FUNC_GREATER);
			p = xgu_set_alpha_ref(p, 0);
		} else if (state->alphaFunc == XBOX_NV2A_ALPHA_LT128) {
			p = xgu_set_alpha_func(p, XGU_FUNC_LESS);
			p = xgu_set_alpha_ref(p, 128);
		} else if (state->alphaFunc == XBOX_NV2A_ALPHA_GE128) {
			p = xgu_set_alpha_func(p, XGU_FUNC_GREATER_OR_EQUAL);
			p = xgu_set_alpha_ref(p, 128);
		}
		xboxNV2ABoundAlphaFunc = state->alphaFunc;
	}
	pb_end(p);
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
	const byte *rgba)
{
	XboxNV2AImage *image;
	uint16_t *destination;
	XguTexFormatColor format = XGU_TEXTURE_FORMAT_R5G6B5_SWIZZLED;
	size_t textureSize = (size_t)width * (size_t)height * 2;
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
	for (pixel = 0; pixel < (size_t)width * (size_t)height; ++pixel) {
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
	destination = (uint16_t *)image->memory;
	for (y = 0; y < height; ++y) {
		for (x = 0; x < width; ++x) {
			destination[XboxNV2ASwizzledOffset((unsigned int)x, (unsigned int)y,
				(unsigned int)width, (unsigned int)height)] =
				XboxNV2APackTexel(rgba + ((size_t)y * width + x) * 4, format);
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
	const byte *rgba)
{
	int index;

	if (!name || !*name || !rgba || !xboxNV2AInitialized ||
		!XboxNV2AIsPowerOfTwo(width) || !XboxNV2AIsPowerOfTwo(height) ||
		width > XBOX_NV2A_MAX_TEXTURE_SIZE || height > XBOX_NV2A_MAX_TEXTURE_SIZE)
		return 0;
	index = XboxNV2A_FindImage(name);
	return index ? index : XboxNV2AAddImage(name, width, height, rgba);
}

/* Unresolved names are kept with image 0 so the loader does not retry them. */
qboolean XboxNV2A_FindShader(const char *name, qhandle_t *handle)
{
	unsigned int index;

	for (index = XBOX_NV2A_WHITE_SHADER; index < xboxNV2AShaderCount; ++index) {
		if (!Q_stricmp(xboxNV2AShaders[index].name, name)) {
			*handle = xboxNV2AShaders[index].state.image ? (qhandle_t)index : 0;
			return qtrue;
		}
	}
	*handle = 0;
	return qfalse;
}

qhandle_t XboxNV2A_CreateShader(const char *name,
	const xboxNV2AShaderState_t *state)
{
	XboxNV2AShader *shader;

	if (!xboxNV2AInitialized || xboxNV2AShaderCount >= XBOX_NV2A_MAX_SHADERS) {
		Sys_XboxLog("Xbox NV2A: shader table full, skipped %s\n", name);
		return 0;
	}
	shader = &xboxNV2AShaders[xboxNV2AShaderCount];
	Q_strncpyz(shader->name, name, sizeof(shader->name));
	shader->state = *state;
	if (shader->state.image < 0 || (unsigned int)shader->state.image >= xboxNV2AImageCount)
		shader->state.image = 0;
	xboxNV2AShaderCount++;
	return shader->state.image ? (qhandle_t)(xboxNV2AShaderCount - 1) : 0;
}

static void XboxNV2AResetTextures(void)
{
	static const byte whitePixel[4] = {255, 255, 255, 255};
	xboxNV2AShaderState_t white;

	memset(xboxNV2AImages, 0, sizeof(xboxNV2AImages));
	memset(xboxNV2AShaders, 0, sizeof(xboxNV2AShaders));
	Q_strncpyz(xboxNV2AImages[0].name, "*missing", sizeof(xboxNV2AImages[0].name));
	Q_strncpyz(xboxNV2AShaders[0].name, "*missing", sizeof(xboxNV2AShaders[0].name));
	xboxNV2AImageCount = XBOX_NV2A_WHITE_IMAGE;
	xboxNV2AShaderCount = XBOX_NV2A_WHITE_SHADER;
	xboxNV2ATexturePoolUsed = 0;
	XboxNV2AAddImage("*white", 1, 1, whitePixel);

	white.image = XBOX_NV2A_WHITE_IMAGE;
	white.srcBlend = GL_SRC_ALPHA;
	white.dstBlend = GL_ONE_MINUS_SRC_ALPHA;
	white.alphaFunc = XBOX_NV2A_ALPHA_NONE;
	white.clamp = qtrue;
	white.vertexColor = qtrue;
	white.vertexAlpha = qtrue;
	XboxNV2A_CreateShader("white", &white);

	XboxNV2AInvalidateState();
	xboxNV2ABatchQuads = 0;
	xboxNV2ABatchShader = -1;
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

static void XboxNV2AFlushBatch(void)
{
	const xboxNV2AShaderState_t *state;
	unsigned int count = xboxNV2ABatchQuads * 4;

	if (!count)
		return;
	state = &xboxNV2AShaders[xboxNV2ABatchShader].state;
	xboxNV2ALastShaderName = xboxNV2AShaders[xboxNV2ABatchShader].name;
	if (xboxNV2AFrameCount <= XBOX_NV2A_TRACE_FRAMES)
		Sys_XboxLog("Xbox NV2A: frame %u draw %s quads=%u first=%u image=%d %dx%d "
			"fmt=%u blend=%x/%x alpha=%d clamp=%d\n", xboxNV2AFrameCount,
			xboxNV2ALastShaderName, xboxNV2ABatchQuads, xboxNV2AVertexUsed,
			state->image, xboxNV2AImages[state->image].width,
			xboxNV2AImages[state->image].height,
			(unsigned int)xboxNV2AImages[state->image].format, state->srcBlend,
			state->dstBlend, state->alphaFunc, state->clamp);
	XboxNV2AApplyShaderState(state);
	XboxNV2ADrawVertices(XGU_QUADS, xboxNV2AVertexUsed, count);
	xboxNV2AVertexUsed += count;
	xboxNV2ABatchQuads = 0;
}

/* Returns room for count more vertices, rewinding the stream once the GPU is idle. */
static XboxNV2AColoredVertex *XboxNV2AStreamVertices(unsigned int count)
{
	if (xboxNV2AVertexUsed + xboxNV2ABatchQuads * 4 + count > XBOX_NV2A_MAX_VERTS) {
		XboxNV2AFlushBatch();
		XboxNV2AWaitIdle("vertex stream");
		xboxNV2AVertexUsed = 0;
	}
	return &xboxNV2AVertexMemory[xboxNV2AVertexUsed + xboxNV2ABatchQuads * 4];
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
	p = xgu_set_color_clear_value(p, XBOX_NV2A_CLEAR_COLOR);
	p = xgu_set_zstencil_clear_value(p, 0xffffff00);
	p = xgu_clear_surface(p, XGU_CLEAR_Z | XGU_CLEAR_STENCIL | XGU_CLEAR_COLOR);
	/* 2D draws need no culling or depth; pbkit enables both by default. */
	p = xgu_set_cull_face_enable(p, false);
	p = xgu_set_depth_test_enable(p, false);
	p = xgu_set_depth_mask(p, false);
	p = xgu_set_stencil_test_enable(p, false);
	p = xgu_set_alpha_test_enable(p, false);
	p = xgu_set_blend_enable(p, true);
	p = xgu_set_transform_constant_load(p, 96);
	p = xgu_set_transform_constant(p, xboxNV2AScreenMatrix.col, 4);
	pb_end(p);
	XboxNV2AInvalidateState();

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

	/* Vertices are already screen pixels, which program mode outputs directly. */
	memset(&xboxNV2AScreenMatrix, 0, sizeof(xboxNV2AScreenMatrix));
	for (i = 0; i < 4; ++i)
		xboxNV2AScreenMatrix.col[i].f[i] = 1.0f;
	/* The xgu samples' z-buffer precision fixup. */
	xgux_set_depth_range(0.0f, (float)0xFFFFFF);

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
	XboxNV2AResetTextures();
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
	xboxNV2ABatchQuads = 0;
	xboxNV2ABatchShader = -1;
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
		XboxNV2AApplyShaderState(&xboxNV2AShaders[XBOX_NV2A_WHITE_SHADER].state);
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

	XboxNV2AFlushBatch();
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

void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader)
{
	XboxNV2AColoredVertex *v;
	const xboxNV2AShaderState_t *state;
	float c[4];

	/* Handle 0 marks a shader the loader could not resolve; draw nothing. */
	if (!xboxNV2AInFrame || w <= 0.0f || h <= 0.0f || shader <= 0 ||
		(unsigned int)shader >= xboxNV2AShaderCount ||
		!xboxNV2AShaders[shader].state.image)
		return;
	state = &xboxNV2AShaders[shader].state;
	c[0] = state->vertexColor ? xboxNV2AColor[0] : 1.0f;
	c[1] = state->vertexColor ? xboxNV2AColor[1] : 1.0f;
	c[2] = state->vertexColor ? xboxNV2AColor[2] : 1.0f;
	c[3] = state->vertexAlpha ? xboxNV2AColor[3] : 1.0f;
	if (xboxNV2ABatchQuads && shader != xboxNV2ABatchShader)
		XboxNV2AFlushBatch();

	v = XboxNV2AStreamVertices(4);
	xboxNV2ABatchShader = shader;
	xboxNV2ABatchQuads++;
	v[0] = (XboxNV2AColoredVertex){{x, y, 1.0f}, {c[0], c[1], c[2], c[3]}, {s1, t1}};
	v[1] = (XboxNV2AColoredVertex){{x + w, y, 1.0f}, {c[0], c[1], c[2], c[3]}, {s2, t1}};
	v[2] = (XboxNV2AColoredVertex){{x + w, y + h, 1.0f}, {c[0], c[1], c[2], c[3]}, {s2, t2}};
	v[3] = (XboxNV2AColoredVertex){{x, y + h, 1.0f}, {c[0], c[1], c[2], c[3]}, {s1, t2}};
}
