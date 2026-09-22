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

typedef struct {
	float position[3];
	float color[3];
} XboxNV2AColoredVertex;

static const XboxNV2AColoredVertex xboxNV2AVertices[] = {
	{{-0.72f, -0.70f, 1.0f}, {1.0f, 0.08f, 0.04f}},
	{{ 0.00f,  0.76f, 1.0f}, {0.08f, 1.0f, 0.16f}},
	{{ 0.72f, -0.70f, 1.0f}, {0.08f, 0.24f, 1.0f}},
};

static uint32_t *xboxNV2AVertexMemory;
static float xboxNV2AViewport[4][4];
static int xboxNV2AWidth;
static int xboxNV2AHeight;
static qboolean xboxNV2AInitialized;
static byte xboxNV2AColor[4] = {255, 255, 255, 255};

static byte XboxNV2AFloatToByte(float value)
{
	if (value <= 0.0f)
		return 0;
	if (value >= 1.0f)
		return 255;
	return (byte)(value * 255.0f + 0.5f);
}

static void XboxNV2ASetScissor(int x, int y, int width, int height)
{
	uint32_t *p;

	if (x < 0) {
		width += x;
		x = 0;
	}
	if (y < 0) {
		height += y;
		y = 0;
	}
	if (x + width > xboxNV2AWidth)
		width = xboxNV2AWidth - x;
	if (y + height > xboxNV2AHeight)
		height = xboxNV2AHeight - y;
	if (width < 0)
		width = 0;
	if (height < 0)
		height = 0;

	p = pb_begin();
	p = pb_push1(p, NV097_SET_SURFACE_CLIP_HORIZONTAL,
		((uint32_t)width << 16) | ((uint32_t)x & 0xffff));
	p = pb_push1(p, NV097_SET_SURFACE_CLIP_VERTICAL,
		((uint32_t)height << 16) | ((uint32_t)y & 0xffff));
	pb_end(p);
}

static void XboxNV2AMatrixViewport(float out[4][4], float x, float y,
	float width, float height, float zMin, float zMax)
{
	memset(out, 0, sizeof(float) * 16);
	out[0][0] = width / 2.0f;
	out[1][1] = height / -2.0f;
	out[2][2] = zMax - zMin;
	out[3][3] = 1.0f;
	out[3][0] = x + width / 2.0f;
	out[3][1] = y + height / 2.0f;
	out[3][2] = zMin;
}

static void XboxNV2ASetAttributePointer(unsigned int index, unsigned int format,
	unsigned int size, unsigned int stride, const void *data)
{
	uint32_t *p = pb_begin();

	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_FORMAT + index * 4,
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE, format) |
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_SIZE, size) |
		MASK(NV097_SET_VERTEX_DATA_ARRAY_FORMAT_STRIDE, stride));
	p = pb_push1(p, NV097_SET_VERTEX_DATA_ARRAY_OFFSET + index * 4,
		(uint32_t)data & 0x03ffffff);
	pb_end(p);
}

static void XboxNV2ADrawTriangle(void)
{
	uint32_t *p = pb_begin();

	p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_TRIANGLES);
	p = pb_push1(p, 0x40000000 | NV097_DRAW_ARRAYS,
		MASK(NV097_DRAW_ARRAYS_COUNT, 2) |
		MASK(NV097_DRAW_ARRAYS_START_INDEX, 0));
	p = pb_push1(p, NV097_SET_BEGIN_END, NV097_SET_BEGIN_END_OP_END);
	pb_end(p);
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
	uint32_t *p;
	int i;

	p = pb_begin();
	p = pb_push1(p, NV097_SET_TRANSFORM_CONSTANT_LOAD, 96);
	pb_push(p++, NV097_SET_TRANSFORM_CONSTANT, 16);
	memcpy(p, xboxNV2AViewport, sizeof(xboxNV2AViewport));
	p += 16;
	pb_end(p);

	p = pb_begin();
	pb_push(p++, NV097_SET_VERTEX_DATA_ARRAY_FORMAT, 16);
	for (i = 0; i < 16; ++i)
		*(p++) = NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F;
	pb_end(p);

	XboxNV2ASetAttributePointer(0, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
		3, sizeof(XboxNV2AColoredVertex), &xboxNV2AVertexMemory[0]);
	XboxNV2ASetAttributePointer(3, NV097_SET_VERTEX_DATA_ARRAY_FORMAT_TYPE_F,
		3, sizeof(XboxNV2AColoredVertex), &xboxNV2AVertexMemory[3]);
}

qboolean XboxNV2A_Init(void)
{
	int status;

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
		pb_kill();
		return qfalse;
	}

	xboxNV2AVertexMemory = MmAllocateContiguousMemoryEx(
		sizeof(xboxNV2AVertices), 0, 0x03ffb000, 0,
		PAGE_READWRITE | PAGE_WRITECOMBINE);
	if (!xboxNV2AVertexMemory) {
		Sys_XboxLog("Xbox NV2A: vertex allocation failed\n");
		pb_kill();
		return qfalse;
	}
	memcpy(xboxNV2AVertexMemory, xboxNV2AVertices,
		sizeof(xboxNV2AVertices));

	XboxNV2AMatrixViewport(xboxNV2AViewport, 0.0f, 0.0f,
		(float)xboxNV2AWidth, (float)xboxNV2AHeight, 0.0f, 65536.0f);
	XboxNV2AInitShader();
	xboxNV2AInitialized = qtrue;
	Sys_XboxLog("Xbox NV2A: pbkit initialized at %dx%d\n",
		xboxNV2AWidth, xboxNV2AHeight);
	return qtrue;
}

void XboxNV2A_Shutdown(qboolean destroyWindow)
{
	(void)destroyWindow;
	if (!xboxNV2AInitialized)
		return;

	if (xboxNV2AVertexMemory) {
		MmFreeContiguousMemory(xboxNV2AVertexMemory);
		xboxNV2AVertexMemory = NULL;
	}
	pb_show_debug_screen();
	pb_kill();
	xboxNV2AInitialized = qfalse;
}

void XboxNV2A_BeginRegistration(glconfig_t *config)
{
	memset(config, 0, sizeof(*config));
	config->vidWidth = xboxNV2AWidth;
	config->vidHeight = xboxNV2AHeight;
	config->windowAspect = (float)xboxNV2AWidth / (float)xboxNV2AHeight;
	config->maxTextureSize = 4096;
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
	pb_target_back_buffer();
	pb_erase_depth_stencil_buffer(0, 0, xboxNV2AWidth, xboxNV2AHeight);
	pb_fill(0, 0, xboxNV2AWidth, xboxNV2AHeight, 0xff101820);
	XboxNV2ASetScissor(0, 0, xboxNV2AWidth, xboxNV2AHeight);
	pb_erase_text_screen();
	pb_printat(0, 0, "ioQuake3 Xbox NV2A %dx%d", xboxNV2AWidth, xboxNV2AHeight);
	pb_printat(1, 0, "pbkit G3.2 2D/scissor diagnostics");

	while (pb_busy())
		;

	XboxNV2ASetFrameState();
	XboxNV2ADrawTriangle();
}

void XboxNV2A_EndFrame(int *frontEndMsec, int *backEndMsec)
{
	if (frontEndMsec)
		*frontEndMsec = 0;
	if (backEndMsec)
		*backEndMsec = 0;
	if (!xboxNV2AInitialized)
		return;

	while (pb_busy())
		;
	pb_draw_text_screen();
	while (pb_finished())
		;
}

void XboxNV2A_SetColor(const float *rgba)
{
	if (!rgba) {
		xboxNV2AColor[0] = 255;
		xboxNV2AColor[1] = 255;
		xboxNV2AColor[2] = 255;
		xboxNV2AColor[3] = 255;
		return;
	}

	xboxNV2AColor[0] = XboxNV2AFloatToByte(rgba[0]);
	xboxNV2AColor[1] = XboxNV2AFloatToByte(rgba[1]);
	xboxNV2AColor[2] = XboxNV2AFloatToByte(rgba[2]);
	xboxNV2AColor[3] = XboxNV2AFloatToByte(rgba[3]);
}

void XboxNV2A_DrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader)
{
	int left;
	int top;
	int width;
	int height;
	DWORD color;

	(void)s1;
	(void)t1;
	(void)s2;
	(void)t2;
	(void)shader;
	if (!xboxNV2AInitialized || w <= 0.0f || h <= 0.0f)
		return;

	left = (int)x;
	top = (int)y;
	width = (int)(w + 0.5f);
	height = (int)(h + 0.5f);
	color = ((DWORD)xboxNV2AColor[3] << 24) |
		((DWORD)xboxNV2AColor[0] << 16) |
		((DWORD)xboxNV2AColor[1] << 8) |
		(DWORD)xboxNV2AColor[2];

	/* G3.2 has no texture upload yet; this is the solid 2D blit path. */
	pb_fill(left, top, width, height, color);
}
