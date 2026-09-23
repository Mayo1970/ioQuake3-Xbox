#include "xbox_nv2a.h"

#include "../sys/sys_xbox.h"

#include <hal/video.h>
#include <pbkit/pbkit.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#define MASK(mask, value) (((value) << (ffs(mask) - 1)) & (mask))

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
	unsigned int format;
} XboxNV2ATexture;

#define XBOX_NV2A_MAX_TEXTURES 512
#define XBOX_NV2A_TEXTURE_POOL_BYTES (6u * 1024u * 1024u)
#define XBOX_NV2A_TEXTURE_ALIGN 128u
#define XBOX_NV2A_TRIANGLE_VERTS 3
#define XBOX_NV2A_MAX_QUADS 2048
/* Release pbkit does not check for overflow; flush well below its 512 KiB limit. */
#define XBOX_NV2A_PUSH_LIMIT_DWORDS (96u * 1024u)
#define XBOX_NV2A_QUAD_MAX_DWORDS 32u

/* Texture address/filter fields missing from pbkit's nv_regs.h (values from xgu). */
#define XBOX_NV2A_TEXADDRESS_U 0x00000007
#define XBOX_NV2A_TEXADDRESS_V 0x00000700
#define XBOX_NV2A_TEXADDRESS_P 0x00070000
#define XBOX_NV2A_TEXADDRESS_CLAMP_TO_EDGE 3
#define XBOX_NV2A_TEXFILTER_KERNEL 0x0000E000
#define XBOX_NV2A_TEXFILTER_KERNEL_QUINCUNX 1

static const XboxNV2AColoredVertex xboxNV2ATriangle[XBOX_NV2A_TRIANGLE_VERTS] = {
	{{0.0f, 0.0f, 1.0f}, {1.0f, 0.08f, 0.04f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 1.0f, 0.16f, 1.0f}, {0.0f, 0.0f}},
	{{0.0f, 0.0f, 1.0f}, {0.08f, 0.24f, 1.0f, 1.0f}, {0.0f, 0.0f}},
};

static XboxNV2AColoredVertex *xboxNV2AVertexMemory;
static byte *xboxNV2ATexturePool;
static size_t xboxNV2ATexturePoolUsed;
static XboxNV2ATexture xboxNV2ATextures[XBOX_NV2A_MAX_TEXTURES];
static unsigned int xboxNV2ATextureCount;
static int xboxNV2ABoundTexture = -1;
static unsigned int xboxNV2AQuadCount;
static unsigned int xboxNV2APushedDwords;
static float xboxNV2AScreenMatrix[4][4];
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

static void XboxNV2AEnd(const uint32_t *start, uint32_t *end)
{
	xboxNV2APushedDwords += (unsigned int)(end - start);
	pb_end(end);
}

/* The GPU is idle after this, so the vertex ring and pushbuffer restart at 0. */
static void XboxNV2AFlush(void)
{
	while (pb_busy())
		;
	pb_reset();
	xboxNV2APushedDwords = 0;
	xboxNV2AQuadCount = 0;
}

static void XboxNV2ASetTexture(int handle)
{
	const XboxNV2ATexture *texture = &xboxNV2ATextures[handle];
	uint32_t *start = pb_begin();
	uint32_t *p = start;
	unsigned int widthShift = 0;
	unsigned int heightShift = 0;

	while ((1u << widthShift) < (unsigned int)texture->width) ++widthShift;
	while ((1u << heightShift) < (unsigned int)texture->height) ++heightShift;
	p = pb_push1(p, NV097_SET_TEXTURE_OFFSET,
		(uint32_t)texture->memory & 0x03ffffff);
	p = pb_push1(p, NV097_SET_TEXTURE_FORMAT,
		MASK(NV097_SET_TEXTURE_FORMAT_CONTEXT_DMA, 2) |
		MASK(NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE,
			NV097_SET_TEXTURE_FORMAT_BORDER_SOURCE_COLOR) |
		MASK(NV097_SET_TEXTURE_FORMAT_DIMENSIONALITY, 2) |
		MASK(NV097_SET_TEXTURE_FORMAT_COLOR, texture->format) |
		MASK(NV097_SET_TEXTURE_FORMAT_MIPMAP_LEVELS, 1) |
		MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_U, widthShift) |
		MASK(NV097_SET_TEXTURE_FORMAT_BASE_SIZE_V, heightShift));
	/* U, V and P must all hold a valid mode (1-5); 0 raises a GPU invalid-data error. */
	p = pb_push1(p, NV097_SET_TEXTURE_ADDRESS,
		MASK(XBOX_NV2A_TEXADDRESS_U, XBOX_NV2A_TEXADDRESS_CLAMP_TO_EDGE) |
		MASK(XBOX_NV2A_TEXADDRESS_V, XBOX_NV2A_TEXADDRESS_CLAMP_TO_EDGE) |
		MASK(XBOX_NV2A_TEXADDRESS_P, XBOX_NV2A_TEXADDRESS_CLAMP_TO_EDGE));
	p = pb_push1(p, NV097_SET_TEXTURE_CONTROL0,
		NV097_SET_TEXTURE_CONTROL0_ENABLE);
	p = pb_push1(p, NV097_SET_TEXTURE_FILTER,
		MASK(XBOX_NV2A_TEXFILTER_KERNEL, XBOX_NV2A_TEXFILTER_KERNEL_QUINCUNX) |
		MASK(NV097_SET_TEXTURE_FILTER_MIN, 2) |
		MASK(NV097_SET_TEXTURE_FILTER_MAG, 2));
	XboxNV2AEnd(start, p);
	xboxNV2ABoundTexture = handle;
}

static uint16_t XboxNV2APackTexel(const byte *rgba, unsigned int format)
{
	if (format == NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5)
		return (uint16_t)(((rgba[0] >> 3) << 11) | ((rgba[1] >> 2) << 5) |
			(rgba[2] >> 3));
	return (uint16_t)(((rgba[3] >> 4) << 12) | ((rgba[0] >> 4) << 8) |
		((rgba[1] >> 4) << 4) | (rgba[2] >> 4));
}

static qhandle_t XboxNV2AAddTexture(const char *name, int width, int height,
	const byte *rgba)
{
	XboxNV2ATexture *texture;
	uint16_t *destination;
	unsigned int format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_R5G6B5;
	size_t textureSize = (size_t)width * (size_t)height * 2;
	size_t offset;
	size_t pixel;
	int x, y;

	offset = (xboxNV2ATexturePoolUsed + XBOX_NV2A_TEXTURE_ALIGN - 1) &
		~(size_t)(XBOX_NV2A_TEXTURE_ALIGN - 1);
	if (xboxNV2ATextureCount >= XBOX_NV2A_MAX_TEXTURES ||
		offset + textureSize > XBOX_NV2A_TEXTURE_POOL_BYTES) {
		Sys_XboxLog("Xbox NV2A: texture pool full, skipped %s\n", name);
		return 0;
	}
	for (pixel = 0; pixel < (size_t)width * (size_t)height; ++pixel) {
		if (rgba[pixel * 4 + 3] != 255) {
			format = NV097_SET_TEXTURE_FORMAT_COLOR_SZ_A4R4G4B4;
			break;
		}
	}

	texture = &xboxNV2ATextures[xboxNV2ATextureCount];
	texture->memory = xboxNV2ATexturePool + offset;
	texture->width = width;
	texture->height = height;
	texture->format = format;
	Q_strncpyz(texture->name, name, sizeof(texture->name));
	destination = (uint16_t *)texture->memory;
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
	return (qhandle_t)xboxNV2ATextureCount++;
}

qhandle_t XboxNV2A_RegisterTexture(const char *name, int width, int height,
	const byte *rgba)
{
	unsigned int index;

	if (!name || !*name || !rgba || !xboxNV2AInitialized ||
		!XboxNV2AIsPowerOfTwo(width) || !XboxNV2AIsPowerOfTwo(height) ||
		width > XBOX_NV2A_MAX_TEXTURE_SIZE || height > XBOX_NV2A_MAX_TEXTURE_SIZE)
		return 0;
	for (index = XBOX_NV2A_WHITE_TEXTURE + 1; index < xboxNV2ATextureCount; ++index) {
		if (!Q_stricmp(xboxNV2ATextures[index].name, name))
			return (qhandle_t)index;
	}
	return XboxNV2AAddTexture(name, width, height, rgba);
}

static void XboxNV2AResetTextures(void)
{
	static const byte whitePixel[4] = {255, 255, 255, 255};

	memset(xboxNV2ATextures, 0, sizeof(xboxNV2ATextures));
	Q_strncpyz(xboxNV2ATextures[0].name, "*missing", sizeof(xboxNV2ATextures[0].name));
	xboxNV2ATextureCount = XBOX_NV2A_WHITE_TEXTURE;
	xboxNV2ATexturePoolUsed = 0;
	XboxNV2AAddTexture("white", 1, 1, whitePixel);
	xboxNV2ABoundTexture = -1;
}

static void XboxNV2ASetAttributePointer(unsigned int index, unsigned int format,
	unsigned int size, unsigned int stride, const void *data)
{
	uint32_t *start = pb_begin();
	uint32_t *p = start;

	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format) |
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
		(uint32_t)data & 0x03ffffff);
	XboxNV2AEnd(start, p);
}

static void XboxNV2ADrawArrays(unsigned int mode, unsigned int first,
	unsigned int count)
{
	uint32_t *start = pb_begin();
	uint32_t *p = start;

	/* The vertex cache survives frames; drop lines fetched before the CPU wrote them. */
	p = pb_push1(p, NV097_BREAK_VERTEX_BUFFER_CACHE, 0);
	p = pb_push1(p, NV097_SET_BEGIN_END, mode);
	p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK(NV097_DRAW_ARRAYS_COUNT, count - 1) |
		MASK(NV097_DRAW_ARRAYS_START_INDEX, first));
	p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
	XboxNV2AEnd(start, p);
}

static void XboxNV2AInitShader(void)
{
	uint32_t *p;
	int i;
	static const uint32_t vertexProgram[] = {
#include "xbox_nv2a_vp.inl"
	};

	p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_START, 0);
	p = pb_push1(p, NV097_SET_TRANSFORM_EXECUTION_MODE,
		MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_MODE,
			NV097_SET_TRANSFORM_EXECUTION_MODE_MODE_PROGRAM) |
		MASK(NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE,
			NV097_SET_TRANSFORM_EXECUTION_MODE_RANGE_MODE_PRIV));
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_CXT_WRITE_EN, 0);
	pb_end(p);

	p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_PROGRAM_LOAD, 0);
	pb_end(p);

	for (i = 0; i < (int)(sizeof(vertexProgram) / 16); ++i) {
		p = pb_begin();
		pb_push(p++, NV097_SET_TRANSFORM_PROGRAM, 4);
		memcpy(p, &vertexProgram[i * 4], 4 * sizeof(uint32_t));
		p += 4;
		pb_end(p);
	}

	p = pb_begin();
#include "xbox_nv2a_fp.inl"
	pb_end(p);
}

static void XboxNV2ASetFrameState(void)
{
	uint32_t *start;
	uint32_t *p;
	int i;
	const XboxNV2AColoredVertex *v = xboxNV2AVertexMemory;

	start = p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);
	pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
	memcpy(p, xboxNV2AScreenMatrix, sizeof(xboxNV2AScreenMatrix));
	p += 16;
	XboxNV2AEnd(start, p);

	/* 2D draws need no culling or depth; pbkit enables both by default. */
	start = p = pb_begin();
	p = pb_push1(p, NV097_SET_CULL_FACE_ENABLE, 0);
	p = pb_push1(p, NV097_SET_DEPTH_TEST_ENABLE, 0);
	p = pb_push1(p, NV097_SET_DEPTH_MASK, 0);
	p = pb_push1(p, NV097_SET_STENCIL_TEST_ENABLE, 0);
	p = pb_push1(p, NV097_SET_ALPHA_TEST_ENABLE, 0);
	p = pb_push1(p, NV097_SET_BLEND_ENABLE, 1);
	p = pb_push1(p, NV097_SET_BLEND_FUNC_SFACTOR,
		NV097_SET_BLEND_FUNC_SFACTOR_V_SRC_ALPHA);
	p = pb_push1(p, NV097_SET_BLEND_FUNC_DFACTOR,
		NV097_SET_BLEND_FUNC_DFACTOR_V_ONE_MINUS_SRC_ALPHA);
	XboxNV2AEnd(start, p);

	start = p = pb_begin();
	pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
	for (i = 0; i < 16; ++i)
		*(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	XboxNV2AEnd(start, p);

	XboxNV2ASetAttributePointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
		3, sizeof(*v), &v[0].position[0]);
	XboxNV2ASetAttributePointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
		4, sizeof(*v), &v[0].color[0]);
	XboxNV2ASetAttributePointer(9, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
		2, sizeof(*v), &v[0].texcoord[0]);
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

	xboxNV2AWidth = (int)pb_back_buffer_width();
	xboxNV2AHeight = (int)pb_back_buffer_height();
	if (xboxNV2AWidth <= 0 || xboxNV2AHeight <= 0) {
		Sys_XboxLog("Xbox NV2A: invalid back buffer %dx%d\n",
			xboxNV2AWidth, xboxNV2AHeight);
		pb_kill();
		return qfalse;
	}

	xboxNV2AVertexMemory = MmAllocateContiguousMemoryEx(
		(XBOX_NV2A_TRIANGLE_VERTS + XBOX_NV2A_MAX_QUADS * 4) *
		sizeof(XboxNV2AColoredVertex), 0, 0x03ffb000, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	xboxNV2ATexturePool = MmAllocateContiguousMemoryEx(
		XBOX_NV2A_TEXTURE_POOL_BYTES, 0, 0x03ffb000, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!xboxNV2AVertexMemory || !xboxNV2ATexturePool) {
		Sys_XboxLog("Xbox NV2A: vertex/texture allocation failed\n");
		XboxNV2AReleaseMemory();
		pb_kill();
		return qfalse;
	}
	memcpy(xboxNV2AVertexMemory, xboxNV2ATriangle, sizeof(xboxNV2ATriangle));
	xboxNV2AVertexMemory[0].position[0] = xboxNV2AWidth * 0.18f;
	xboxNV2AVertexMemory[0].position[1] = xboxNV2AHeight * 0.70f;
	xboxNV2AVertexMemory[1].position[0] = xboxNV2AWidth * 0.50f;
	xboxNV2AVertexMemory[1].position[1] = xboxNV2AHeight * 0.20f;
	xboxNV2AVertexMemory[2].position[0] = xboxNV2AWidth * 0.82f;
	xboxNV2AVertexMemory[2].position[1] = xboxNV2AHeight * 0.70f;

	xboxNV2AInitialized = qtrue;
	XboxNV2AResetTextures();

	/* Vertices are already screen pixels, which program mode outputs directly. */
	memset(xboxNV2AScreenMatrix, 0, sizeof(xboxNV2AScreenMatrix));
	for (i = 0; i < 4; ++i)
		xboxNV2AScreenMatrix[i][i] = 1.0f;
	XboxNV2AInitShader();

	pb_show_front_screen();
	xboxNV2ADebugScreen = qfalse;
	Sys_XboxLog("Xbox NV2A: pbkit initialized at %dx%d\n",
		xboxNV2AWidth, xboxNV2AHeight);
	return qtrue;
}

/* pbkit stays alive; the client calls this on every map load and vid_restart. */
void XboxNV2A_Shutdown(qboolean destroyWindow)
{
	(void)destroyWindow;
	if (!xboxNV2AInitialized)
		return;
	while (pb_busy())
		;
	XboxNV2AResetTextures();
}

void XboxNV2A_Kill(void)
{
	if (!xboxNV2AInitialized)
		return;
	while (pb_busy())
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

void XboxNV2A_BeginFrame(stereoFrame_t stereoFrame)
{
	(void)stereoFrame;
	if (!xboxNV2AInitialized)
		return;

	pb_wait_for_vbl();
	pb_reset();
	xboxNV2APushedDwords = 0;
	xboxNV2AQuadCount = 0;
	xboxNV2ABoundTexture = -1;
	pb_target_back_buffer();
	pb_erase_depth_stencil_buffer(0, 0, xboxNV2AWidth, xboxNV2AHeight);
	pb_fill(0, 0, xboxNV2AWidth, xboxNV2AHeight, 0xff101820);
	if (XBOX_NV2A_DIAGNOSTICS) {
		pb_erase_text_screen();
		pb_printat(0, 0, "ioQuake3 Xbox NV2A %dx%d", xboxNV2AWidth, xboxNV2AHeight);
		pb_printat(1, 0, "pbkit textured 2D UI path");
	}

	while (pb_busy())
		;

	XboxNV2ASetFrameState();
	xboxNV2AInFrame = qtrue;
	if (XBOX_NV2A_DIAGNOSTICS) {
		XboxNV2ASetTexture(XBOX_NV2A_WHITE_TEXTURE);
		XboxNV2ADrawArrays(NV097_SET_BEGIN_END_OP_TRIANGLES, 0,
			XBOX_NV2A_TRIANGLE_VERTS);
	}
}

void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec)
{
	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (!xboxNV2AInitialized || !xboxNV2AInFrame)
		return;

	xboxNV2AInFrame = qfalse;
	while (pb_busy())
		;
	if (XBOX_NV2A_DIAGNOSTICS) {
		pb_draw_text_screen();
	}
	while (pb_finished())
		;
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
	unsigned int first;
	const float *c = xboxNV2AColor;

	/* Handle 0 marks a shader the loader could not resolve; draw nothing. */
	if (!xboxNV2AInFrame || w <= 0.0f || h <= 0.0f || shader <= 0 ||
		(unsigned int)shader >= xboxNV2ATextureCount)
		return;
	if (xboxNV2AQuadCount >= XBOX_NV2A_MAX_QUADS ||
		xboxNV2APushedDwords + XBOX_NV2A_QUAD_MAX_DWORDS > XBOX_NV2A_PUSH_LIMIT_DWORDS)
		XboxNV2AFlush();
	if (shader != xboxNV2ABoundTexture)
		XboxNV2ASetTexture(shader);

	first = XBOX_NV2A_TRIANGLE_VERTS + xboxNV2AQuadCount * 4;
	v = &xboxNV2AVertexMemory[first];
	v[0] = (XboxNV2AColoredVertex){{x, y, 1.0f}, {c[0], c[1], c[2], c[3]}, {s1, t1}};
	v[1] = (XboxNV2AColoredVertex){{x + w, y, 1.0f}, {c[0], c[1], c[2], c[3]}, {s2, t1}};
	v[2] = (XboxNV2AColoredVertex){{x + w, y + h, 1.0f}, {c[0], c[1], c[2], c[3]}, {s2, t2}};
	v[3] = (XboxNV2AColoredVertex){{x, y + h, 1.0f}, {c[0], c[1], c[2], c[3]}, {s1, t2}};
	__asm__ __volatile__("sfence" ::: "memory");
	xboxNV2AQuadCount++;
	XboxNV2ADrawArrays(NV097_SET_BEGIN_END_OP_QUADS, first, 4);
}
