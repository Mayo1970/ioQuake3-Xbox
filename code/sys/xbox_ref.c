/* Minimal in-process ref API for the native Xbox renderer shell. */
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"
#include "../renderernv2a/xbox_nv2a.h"

#include <stdlib.h>
#include <string.h>

/* The ioq3 image loaders (tr_image_*.c) read this global. */
refimport_t ri;

static refexport_t xboxRefExport;

/* Decoded images are transient; keep them out of the 8 MiB zone. */
static void *XboxRefMalloc(int bytes)
{
	void *memory = malloc((size_t)(bytes > 0 ? bytes : 1));

	if (!memory)
		ri.Error(ERR_DROP, "Xbox ref: out of memory for %d bytes", bytes);
	return memory;
}

static void XboxRefFree(void *memory)
{
	free(memory);
}

static qhandle_t XboxRefRegisterShader(const char *name)
{
	return XboxNV2AShader_Register(name, qtrue);
}

static qhandle_t XboxRefRegisterShaderNoMip(const char *name)
{
	return XboxNV2AShader_Register(name, qfalse);
}

static void XboxRefShutdown(qboolean destroyWindow)
{
	XboxNV2A_Shutdown(destroyWindow);
}

static void XboxRefBeginRegistration(glconfig_t *config)
{
	XboxNV2A_BeginRegistration(config);
}

static qhandle_t XboxRefRegisterModel(const char *name)
{
	return XboxNV2AModel_Register(name);
}

static qhandle_t XboxRefRegisterSkin(const char *name)
{
	return XboxNV2ASkin_Register(name);
}

static void XboxRefNoop(void) {}
static void XboxRefLoadWorld(const char *name)
{
	XboxNV2AWorld_Load(name);
}
/* The world reads PVS rows through ri.CM_ClusterPVS instead. */
static void XboxRefSetWorldVisData(const byte *vis) { (void)vis; }
static void XboxRefClearScene(void)
{
	XboxNV2A_ClearScene();
}
static void XboxRefAddRefEntity(const refEntity_t *entity)
{
	XboxNV2A_AddRefEntity(entity);
}
static void XboxRefAddPoly(qhandle_t shader, int numVerts,
	const polyVert_t *verts, int num)
{
	(void)shader; (void)numVerts; (void)verts; (void)num;
}
static int XboxRefLightForPoint(vec3_t point, vec3_t ambient,
	vec3_t directed, vec3_t direction)
{
	return XboxNV2AWorld_LightGrid(point, ambient, directed, direction);
}
static void XboxRefAddLight(const vec3_t org, float intensity,
	float r, float g, float b)
{
	XboxNV2A_AddLight(org, intensity, r, g, b);
}
static void XboxRefRenderScene(const refdef_t *fd)
{
	XboxNV2A_RenderScene(fd);
}
static void XboxRefSetColor(const float *rgba)
{
	XboxNV2A_SetColor(rgba);
}
static void XboxRefDrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader)
{
	XboxNV2A_DrawStretchPic(x, y, w, h, s1, t1, s2, t2, shader);
}
static void XboxRefDrawStretchRaw(int x, int y, int w, int h, int cols,
	int rows, const byte *data, int client, qboolean dirty)
{
	XboxNV2A_DrawStretchRaw(x, y, w, h, cols, rows, data, client, dirty);
}
static void XboxRefUploadCinematic(int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty)
{
	(void)w; (void)h;
	XboxNV2A_UploadCinematic(cols, rows, data, client, dirty);
}
static void XboxRefBeginFrame(stereoFrame_t stereo)
{
	XboxNV2A_BeginFrame(stereo);
}
static void XboxRefEndFrame(int *front, int *back)
{
	XboxNV2A_EndFrame(front, back);
}
static int XboxRefMarkFragments(int numPoints, const vec3_t *points,
	const vec3_t projection, int maxPoints, vec3_t pointBuffer,
	int maxFragments, markFragment_t *fragmentBuffer)
{
	(void)numPoints; (void)points; (void)projection; (void)maxPoints;
	(void)pointBuffer; (void)maxFragments; (void)fragmentBuffer;
	return 0;
}
static int XboxRefLerpTag(orientation_t *tag, qhandle_t model, int start,
	int end, float frac, const char *tagName)
{
	return XboxNV2AModel_LerpTag(tag, model, start, end, frac, tagName);
}
static void XboxRefModelBounds(qhandle_t model, vec3_t mins, vec3_t maxs)
{
	XboxNV2AModel_Bounds(model, mins, maxs);
}
static void XboxRefRegisterFont(const char *name, int size, fontInfo_t *font)
{
	(void)name; (void)size; memset(font, 0, sizeof(*font));
}
static void XboxRefRemapShader(const char *oldShader, const char *newShader,
	const char *offsetTime)
{
	(void)oldShader; (void)newShader; (void)offsetTime;
}
static qboolean XboxRefGetEntityToken(char *buffer, int size)
{
	return XboxNV2AWorld_GetEntityToken(buffer, size);
}
static qboolean XboxRefInPVS(const vec3_t p1, const vec3_t p2)
{
	return XboxNV2AWorld_InPVS(p1, p2);
}
static void XboxRefTakeVideoFrame(int h, int w, byte *capture,
	byte *encode, qboolean motionJpeg)
{
	(void)h; (void)w; (void)capture; (void)encode; (void)motionJpeg;
}

refexport_t *GetRefAPI(int apiVersion, refimport_t *rimp)
{
	if (apiVersion != REF_API_VERSION || !rimp)
		return NULL;
	ri = *rimp;
	ri.Malloc = XboxRefMalloc;
	ri.Free = XboxRefFree;
	if (!XboxNV2A_Init())
		return NULL;

	memset(&xboxRefExport, 0, sizeof(xboxRefExport));
	xboxRefExport.Shutdown = XboxRefShutdown;
	xboxRefExport.BeginRegistration = XboxRefBeginRegistration;
	xboxRefExport.RegisterModel = XboxRefRegisterModel;
	xboxRefExport.RegisterSkin = XboxRefRegisterSkin;
	xboxRefExport.RegisterShader = XboxRefRegisterShader;
	xboxRefExport.RegisterShaderNoMip = XboxRefRegisterShaderNoMip;
	xboxRefExport.LoadWorld = XboxRefLoadWorld;
	xboxRefExport.SetWorldVisData = XboxRefSetWorldVisData;
	xboxRefExport.EndRegistration = XboxRefNoop;
	xboxRefExport.ClearScene = XboxRefClearScene;
	xboxRefExport.AddRefEntityToScene = XboxRefAddRefEntity;
	xboxRefExport.AddPolyToScene = XboxRefAddPoly;
	xboxRefExport.LightForPoint = XboxRefLightForPoint;
	xboxRefExport.AddLightToScene = XboxRefAddLight;
	xboxRefExport.AddAdditiveLightToScene = XboxRefAddLight;
	xboxRefExport.RenderScene = XboxRefRenderScene;
	xboxRefExport.SetColor = XboxRefSetColor;
	xboxRefExport.DrawStretchPic = XboxRefDrawStretchPic;
	xboxRefExport.DrawStretchRaw = XboxRefDrawStretchRaw;
	xboxRefExport.UploadCinematic = XboxRefUploadCinematic;
	xboxRefExport.BeginFrame = XboxRefBeginFrame;
	xboxRefExport.EndFrame = XboxRefEndFrame;
	xboxRefExport.MarkFragments = XboxRefMarkFragments;
	xboxRefExport.LerpTag = XboxRefLerpTag;
	xboxRefExport.ModelBounds = XboxRefModelBounds;
	xboxRefExport.RegisterFont = XboxRefRegisterFont;
	xboxRefExport.RemapShader = XboxRefRemapShader;
	xboxRefExport.GetEntityToken = XboxRefGetEntityToken;
	xboxRefExport.inPVS = XboxRefInPVS;
	xboxRefExport.TakeVideoFrame = XboxRefTakeVideoFrame;
	return &xboxRefExport;
}
