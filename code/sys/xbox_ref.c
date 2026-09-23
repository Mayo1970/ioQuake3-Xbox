/* Minimal in-process ref API for the native Xbox renderer shell. */
#include "../qcommon/q_shared.h"
#include "../renderercommon/tr_public.h"
#include "../renderernv2a/xbox_nv2a.h"

#include <stdlib.h>
#include <string.h>

static refexport_t xboxRefExport;
static refimport_t xboxRefImport;

static unsigned int XboxRefReadLE16(const byte *p)
{
	return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static qboolean XboxRefIsPowerOfTwo(unsigned int value)
{
	return value != 0 && (value & (value - 1)) == 0;
}

static qhandle_t XboxRefRegisterShader(const char *name)
{
	char path[MAX_QPATH];
	void *fileBuffer = NULL;
	byte *rgba = NULL;
	const byte *file;
	long fileLength;
	unsigned int idLength, colorMapType, imageType, width, height, bits;
	unsigned int pixelBytes, descriptor, x, pixel;
	size_t outputBytes;
	qhandle_t handle = 0;

	if (!name || !*name || !xboxRefImport.FS_ReadFile ||
		!xboxRefImport.FS_FreeFile)
		return 0;
	if (!Q_stricmp(name, "white"))
		return XBOX_NV2A_WHITE_TEXTURE;
	Q_strncpyz(path, name, sizeof(path));
	if (!strrchr(path, '.') || Q_stricmp(strrchr(path, '.'), ".tga"))
		Q_strcat(path, sizeof(path), ".tga");
	fileLength = xboxRefImport.FS_ReadFile(path, &fileBuffer);
	if (fileLength < 18 || !fileBuffer) {
		if (fileBuffer)
			xboxRefImport.FS_FreeFile(fileBuffer);
		return 0;
	}
	file = (const byte *)fileBuffer;
	idLength = file[0];
	colorMapType = file[1];
	imageType = file[2];
	width = XboxRefReadLE16(file + 12);
	height = XboxRefReadLE16(file + 14);
	bits = file[16];
	descriptor = file[17];
	pixelBytes = bits / 8;
	if (colorMapType != 0 || (imageType != 2 && imageType != 10) ||
		(width == 0) || (height == 0) || (bits != 24 && bits != 32) ||
		!XboxRefIsPowerOfTwo(width) || !XboxRefIsPowerOfTwo(height) ||
		width > XBOX_NV2A_MAX_TEXTURE_SIZE || height > XBOX_NV2A_MAX_TEXTURE_SIZE ||
		fileLength < 18 + (long)idLength) {
		xboxRefImport.FS_FreeFile(fileBuffer);
		return 0;
	}
	outputBytes = (size_t)width * height * 4;
	rgba = (byte *)malloc(outputBytes);
	if (!rgba) {
		xboxRefImport.FS_FreeFile(fileBuffer);
		return 0;
	}
	file += 18 + idLength;
	fileLength -= 18 + idLength;
	pixel = 0;
	while (pixel < width * height) {
		unsigned int runLength = 1;
		qboolean runPacket = qfalse;
		unsigned int packet;
		if (imageType == 10) {
			if (fileLength < 1)
				goto tga_done;
			packet = *file++;
			--fileLength;
			runPacket = (packet & 0x80) != 0;
			runLength = (packet & 0x7f) + 1;
			if (runLength > width * height - pixel)
				goto tga_done;
		}
		for (x = 0; x < runLength; ++x) {
			const byte *source;
			unsigned int targetPixel = pixel + x;
			unsigned int sourceX = targetPixel % width;
			unsigned int sourceY = targetPixel / width;
			unsigned int targetX = (descriptor & 0x10) ? width - 1 - sourceX : sourceX;
			unsigned int targetY = (descriptor & 0x20) ? sourceY : height - 1 - sourceY;
			byte *target = rgba + ((size_t)targetY * width + targetX) * 4;
			if (runPacket && x != 0) {
				source = file - pixelBytes;
			} else {
				if (fileLength < (long)pixelBytes)
					goto tga_done;
				source = file;
				file += pixelBytes;
				fileLength -= pixelBytes;
			}
			target[0] = source[2];
			target[1] = source[1];
			target[2] = source[0];
			target[3] = (pixelBytes == 4) ? source[3] : 255;
		}
		pixel += runLength;
	}
	handle = XboxNV2A_RegisterTexture(path, (int)width, (int)height, rgba);
tga_done:
	free(rgba);
	xboxRefImport.FS_FreeFile(fileBuffer);
	return handle;
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
	(void)x; (void)y; (void)w; (void)h; (void)cols; (void)rows;
	(void)data; (void)client; (void)dirty;
}
static void XboxRefUploadCinematic(int w, int h, int cols, int rows,
	const byte *data, int client, qboolean dirty)
{
	(void)w; (void)h; (void)cols; (void)rows; (void)data;
	(void)client; (void)dirty;
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
	(void)model; (void)start; (void)end; (void)frac; (void)tagName;
	if (tag) {
		VectorClear(tag->origin);
		AxisClear(tag->axis);
	}
	return qfalse;
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
	xboxRefImport = *rimp;
	if (!XboxNV2A_Init())
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
