/* In-process ref API used until the native NV2A renderer lands in G3. */
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"

#include <string.h>

static refexport_t xboxRefExport;
static qhandle_t xboxRefNextShader = 1;

static void XboxRefShutdown(qboolean destroyWindow) { (void)destroyWindow; }

static void XboxRefBeginRegistration(glconfig_t *config)
{
	memset(config, 0, sizeof(*config));
	config->vidWidth = 640;
	config->vidHeight = 480;
	config->windowAspect = 4.0f / 3.0f;
	Q_strncpyz(config->renderer_string, "native Xbox shell (no renderer)",
		sizeof(config->renderer_string));
}

static qhandle_t XboxRefRegisterShader(const char *name)
{
	(void)name;
	return xboxRefNextShader++;
}

static qhandle_t XboxRefRegisterModel(const char *name)
{
	(void)name;
	return 0;
}

static qhandle_t XboxRefRegisterSkin(const char *name)
{
	(void)name;
	return 0;
}

static void XboxRefNoop(void) {}
static void XboxRefLoadWorld(const char *name) { (void)name; }
static void XboxRefSetWorldVisData(const byte *vis) { (void)vis; }
static void XboxRefClearScene(void) {}
static void XboxRefAddRefEntity(const refEntity_t *entity) { (void)entity; }
static void XboxRefAddPoly(qhandle_t shader, int numVerts,
	const polyVert_t *verts, int num)
{
	(void)shader; (void)numVerts; (void)verts; (void)num;
}
static int XboxRefLightForPoint(vec3_t point, vec3_t ambient,
	vec3_t directed, vec3_t direction)
{
	(void)point;
	VectorClear(ambient);
	VectorClear(directed);
	VectorClear(direction);
	return 0;
}
static void XboxRefAddLight(const vec3_t org, float intensity,
	float r, float g, float b)
{
	(void)org; (void)intensity; (void)r; (void)g; (void)b;
}
static void XboxRefRenderScene(const refdef_t *fd) { (void)fd; }
static void XboxRefSetColor(const float *rgba) { (void)rgba; }
static void XboxRefDrawStretchPic(float x, float y, float w, float h,
	float s1, float t1, float s2, float t2, qhandle_t shader)
{
	(void)x; (void)y; (void)w; (void)h; (void)s1; (void)t1;
	(void)s2; (void)t2; (void)shader;
}
static void XboxRefDrawStretchRaw(int x, int y, int w, int h, int cols,
	int rows, const byte *data, int client, qboolean dirty)
{
	(void)x; (void)y; (void)w; (void)h; (void)cols; (void)rows;
	(void)data; (void)client; (void)dirty;
}
static void XboxRefUploadCinematic(int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty)
{
	(void)w; (void)h; (void)cols; (void)rows; (void)data;
	(void)client; (void)dirty;
}
static void XboxRefBeginFrame(stereoFrame_t stereo) { (void)stereo; }
static void XboxRefEndFrame(int *front, int *back)
{
	if (front) *front = 0;
	if (back) *back = 0;
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
	(void)tag; (void)model; (void)start; (void)end; (void)frac; (void)tagName;
	return -1;
}
static void XboxRefModelBounds(qhandle_t model, vec3_t mins, vec3_t maxs)
{
	(void)model; VectorClear(mins); VectorClear(maxs);
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
	if (buffer && size > 0) buffer[0] = '\0';
	return qfalse;
}
static qboolean XboxRefInPVS(const vec3_t p1, const vec3_t p2)
{
	(void)p1; (void)p2; return qtrue;
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

	memset(&xboxRefExport, 0, sizeof(xboxRefExport));
	xboxRefExport.Shutdown = XboxRefShutdown;
	xboxRefExport.BeginRegistration = XboxRefBeginRegistration;
	xboxRefExport.RegisterModel = XboxRefRegisterModel;
	xboxRefExport.RegisterSkin = XboxRefRegisterSkin;
	xboxRefExport.RegisterShader = XboxRefRegisterShader;
	xboxRefExport.RegisterShaderNoMip = XboxRefRegisterShader;
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
