/* MD3 models and skins for the NV2A renderer, following ioq3 tr_model.c and tr_image.c. */
#include "xbox_nv2a.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_common.h"
#include "../sys/sys_xbox.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_NV2A_MAX_MODELS 256
#define XBOX_NV2A_MAX_SKINS 256
/* ioq3 MAX_SKIN_SURFACES. */
#define XBOX_NV2A_MAX_SKIN_SURFACES 256
/* q3dm11's five players take 4057 KiB at LOD 1 (5150 at LOD 0, 3099 at LOD 2); the heap has ~5 MiB. */
#define XBOX_NV2A_PLAYER_LOD 1
#if defined(STANDALONEOA) || defined(MISSIONPACK)
/* Hunk kept free after a hunk model: the next MD3 or TGA read (up to ~2 MiB) and sounds use temp hunk. */
#define XBOX_NV2A_MODEL_HUNK_RESERVE (3 * 1024 * 1024)
#endif

typedef struct {
	char name[MAX_QPATH];
	md3Header_t *md3;
	/* Hunk models go with the hunk at map change; only heap ones are freed. */
	qboolean onHunk;
	/* World submodel index for "*N" names, or -1. */
	int brush;
} xboxNV2AModel_t;

typedef struct {
	char name[MAX_QPATH];
	int shader;
} xboxNV2ASkinSurface_t;

typedef struct {
	char name[MAX_QPATH];
	int numSurfaces;
	xboxNV2ASkinSurface_t *surfaces;
} xboxNV2ASkin_t;

/* Index 0 is the bad model; failed loads keep an empty entry so they are not retried. */
static xboxNV2AModel_t xboxNV2AModels[XBOX_NV2A_MAX_MODELS];
static int xboxNV2AModelCount = 1;
static xboxNV2ASkin_t xboxNV2ASkins[XBOX_NV2A_MAX_SKINS];
static int xboxNV2ASkinCount = 1;

static qboolean XboxModelRange(int length, int offset, int bytes)
{
	return offset >= 0 && bytes >= 0 && offset <= length && bytes <= length - offset;
}

/* Rejects files whose offsets or indexes would read outside the buffer. */
static qboolean XboxModelValidate(const md3Header_t *md3, int length,
	const char *name)
{
	const md3Surface_t *surface;
	int i, j;

	if (length < (int)sizeof(*md3) || md3->ident != MD3_IDENT ||
		md3->version != MD3_VERSION) {
		Sys_XboxLog("Xbox model: %s is not an MD3 v%d file\n", name, MD3_VERSION);
		return qfalse;
	}
	if (md3->numFrames < 1 || md3->numFrames > MD3_MAX_FRAMES ||
		md3->numSurfaces < 0 || md3->numSurfaces > MD3_MAX_SURFACES ||
		md3->numTags < 0 || md3->numTags > MD3_MAX_TAGS ||
		!XboxModelRange(length, md3->ofsFrames,
			md3->numFrames * (int)sizeof(md3Frame_t)) ||
		!XboxModelRange(length, md3->ofsTags,
			md3->numFrames * md3->numTags * (int)sizeof(md3Tag_t)) ||
		!XboxModelRange(length, md3->ofsSurfaces, 0)) {
		Sys_XboxLog("Xbox model: %s has a bad header\n", name);
		return qfalse;
	}
	surface = (const md3Surface_t *)((const byte *)md3 + md3->ofsSurfaces);
	for (i = 0; i < md3->numSurfaces; ++i) {
		int base = (int)((const byte *)surface - (const byte *)md3);
		const md3Triangle_t *triangles;

		if (!XboxModelRange(length, base, (int)sizeof(*surface)) ||
			surface->ofsEnd <= 0 || !XboxModelRange(length, base, surface->ofsEnd) ||
			surface->numFrames != md3->numFrames ||
			surface->numVerts < 0 || surface->numVerts > XBOX_NV2A_TESS_VERTS ||
			surface->numTriangles < 0 ||
			surface->numTriangles > XBOX_NV2A_TESS_INDEXES / 3 ||
			surface->numShaders < 0 || surface->numShaders > MD3_MAX_SHADERS ||
			!XboxModelRange(surface->ofsEnd, surface->ofsShaders,
				surface->numShaders * (int)sizeof(md3Shader_t)) ||
			!XboxModelRange(surface->ofsEnd, surface->ofsTriangles,
				surface->numTriangles * (int)sizeof(md3Triangle_t)) ||
			!XboxModelRange(surface->ofsEnd, surface->ofsSt,
				surface->numVerts * (int)sizeof(md3St_t)) ||
			!XboxModelRange(surface->ofsEnd, surface->ofsXyzNormals,
				surface->numVerts * surface->numFrames * (int)sizeof(md3XyzNormal_t))) {
			Sys_XboxLog("Xbox model: %s surface %d is out of range\n", name, i);
			return qfalse;
		}
		triangles = (const md3Triangle_t *)((const byte *)surface + surface->ofsTriangles);
		for (j = 0; j < surface->numTriangles; ++j) {
			if ((unsigned int)triangles[j].indexes[0] >= (unsigned int)surface->numVerts ||
				(unsigned int)triangles[j].indexes[1] >= (unsigned int)surface->numVerts ||
				(unsigned int)triangles[j].indexes[2] >= (unsigned int)surface->numVerts) {
				Sys_XboxLog("Xbox model: %s surface %d has a bad index\n", name, i);
				return qfalse;
			}
		}
		surface = (const md3Surface_t *)((const byte *)surface + surface->ofsEnd);
	}
	return qtrue;
}

static md3Header_t *XboxModelLoadMD3(const char *name, qboolean *onHunk)
{
	void *buffer = NULL;
	md3Header_t *md3;
	md3Surface_t *surface;
	long length;
	int i, j;

	*onHunk = qfalse;
	length = ri.FS_ReadFile(name, &buffer);
	if (length <= 0 || !buffer)
		return NULL;
	if (!XboxModelValidate((const md3Header_t *)buffer, (int)length, name)) {
		ri.FS_FreeFile(buffer);
		return NULL;
	}
#if defined(STANDALONEOA) || defined(MISSIONPACK)
	/* ioq3 R_LoadMD3 keeps models on the hunk; OA maps and TA menus leave most of it idle. */
	if (Hunk_MemoryRemaining() >= length + XBOX_NV2A_MODEL_HUNK_RESERVE) {
		md3 = (md3Header_t *)ri.Hunk_Alloc((int)length, h_low);
		*onHunk = qtrue;
	} else
#endif
	md3 = (md3Header_t *)malloc((size_t)length);
	if (!md3) {
		ri.FS_FreeFile(buffer);
		Sys_XboxLog("Xbox model: no memory for %s\n", name);
		return NULL;
	}
	memcpy(md3, buffer, (size_t)length);
	ri.FS_FreeFile(buffer);

	/* ioq3 R_LoadMD3 stores the registered shader handle in shaderIndex. */
	surface = (md3Surface_t *)((byte *)md3 + md3->ofsSurfaces);
	for (i = 0; i < md3->numSurfaces; ++i) {
		md3Shader_t *shader = (md3Shader_t *)((byte *)surface + surface->ofsShaders);
		size_t nameLength;

		/* ioq3 lowercases surface names for skins and drops a q3data "_1"/"_2" suffix. */
		surface->name[sizeof(surface->name) - 1] = '\0';
		Q_strlwr(surface->name);
		nameLength = strlen(surface->name);
		if (nameLength > 2 && surface->name[nameLength - 2] == '_')
			surface->name[nameLength - 2] = '\0';
		for (j = 0; j < surface->numShaders; ++j, ++shader) {
			shader->name[sizeof(shader->name) - 1] = '\0';
			shader->shaderIndex = XboxNV2AShader_RegisterModel(shader->name);
		}
		surface = (md3Surface_t *)((byte *)surface + surface->ofsEnd);
	}
	Sys_XboxLog("Xbox model: %s frames=%d surfaces=%d tags=%d bytes=%ld%s\n", name,
		md3->numFrames, md3->numSurfaces, md3->numTags, length, *onHunk ? " hunk" : "");
	return md3;
}

/* Player MD3s load only the name_N.md3 LOD; a missing one falls back toward LOD 0, as in ioq3. */
static md3Header_t *XboxModelLoadLod(const char *name, qboolean *onHunk)
{
	char base[MAX_QPATH];
	char path[MAX_QPATH];
	md3Header_t *md3;
	int lod;

	if (Q_stricmpn(name, "models/players/", 15) || Q_stricmp(COM_GetExtension(name), "md3"))
		return XboxModelLoadMD3(name, onHunk);
	COM_StripExtension(name, base, sizeof(base));
	for (lod = XBOX_NV2A_PLAYER_LOD; lod > 0; --lod) {
		Com_sprintf(path, sizeof(path), "%s_%d.md3", base, lod);
		md3 = XboxModelLoadMD3(path, onHunk);
		if (md3)
			return md3;
	}
	return XboxModelLoadMD3(name, onHunk);
}

static xboxNV2AModel_t *XboxModelFind(const char *name)
{
	int i;

	for (i = 1; i < xboxNV2AModelCount; ++i) {
		if (!Q_stricmp(xboxNV2AModels[i].name, name))
			return &xboxNV2AModels[i];
	}
	return NULL;
}

static xboxNV2AModel_t *XboxModelAlloc(const char *name)
{
	xboxNV2AModel_t *model;

	if (xboxNV2AModelCount >= XBOX_NV2A_MAX_MODELS) {
		Sys_XboxLog("Xbox model: table full, skipped %s\n", name);
		return NULL;
	}
	model = &xboxNV2AModels[xboxNV2AModelCount++];
	Q_strncpyz(model->name, name, sizeof(model->name));
	model->md3 = NULL;
	model->onHunk = qfalse;
	model->brush = -1;
	return model;
}

/* Brush models only exist once the world is loaded, so "*N" names never hit the file system. */
qhandle_t XboxNV2AModel_Register(const char *name)
{
	xboxNV2AModel_t *model;

	if (!name || !*name || strlen(name) >= MAX_QPATH)
		return 0;
	model = XboxModelFind(name);
	if (!model) {
		model = XboxModelAlloc(name);
		if (!model)
			return 0;
		if (name[0] != '*')
			model->md3 = XboxModelLoadLod(name, &model->onHunk);
	}
	return (model->md3 || model->brush >= 0) ? (qhandle_t)(model - xboxNV2AModels) : 0;
}

/* ioq3 R_LoadSubmodels registers "*N" so cgame can look inline models up by name. */
void XboxNV2AModel_RegisterBrush(const char *name, int submodel)
{
	xboxNV2AModel_t *model = XboxModelFind(name);

	if (!model)
		model = XboxModelAlloc(name);
	if (model)
		model->brush = submodel;
}

void XboxNV2AModel_FreeAll(void)
{
	int i;

	for (i = 1; i < xboxNV2AModelCount; ++i) {
		if (!xboxNV2AModels[i].onHunk)
			free(xboxNV2AModels[i].md3);
	}
	memset(xboxNV2AModels, 0, sizeof(xboxNV2AModels));
	xboxNV2AModelCount = 1;
}

const md3Header_t *XboxNV2AModel_Get(qhandle_t handle)
{
	if (handle <= 0 || handle >= xboxNV2AModelCount)
		return NULL;
	return xboxNV2AModels[handle].md3;
}

int XboxNV2AModel_BrushIndex(qhandle_t handle)
{
	if (handle <= 0 || handle >= xboxNV2AModelCount)
		return -1;
	return xboxNV2AModels[handle].brush;
}

/* sin and cos of the 256 MD3 lat/long steps, the angles ioq3 reads from tr.sinTable. */
static float xboxModelSin[256];
static float xboxModelCos[256];
static qboolean xboxModelTables;

static void XboxModelDecodeNormal(short packed, float *out)
{
	int lat = (packed >> 8) & 0xff;
	int lng = packed & 0xff;

	out[0] = xboxModelCos[lat] * xboxModelSin[lng];
	out[1] = xboxModelSin[lat] * xboxModelSin[lng];
	out[2] = xboxModelCos[lng];
}

/* ioq3 LerpMeshVertexes: decode 1/64 unit positions and lat/long normals; backlerp 0 reads one frame. */
void XboxNV2AModel_LerpSurface(const md3Surface_t *surface, int frame,
	int oldFrame, float backlerp, float (*xyz)[3], float (*normal)[3])
{
	const md3XyzNormal_t *newVerts = (const md3XyzNormal_t *)((const byte *)surface +
		surface->ofsXyzNormals) + frame * surface->numVerts;
	const md3XyzNormal_t *oldVerts = (const md3XyzNormal_t *)((const byte *)surface +
		surface->ofsXyzNormals) + oldFrame * surface->numVerts;
	float frontlerp = 1.0f - backlerp;
	int i, k;

	if (!xboxModelTables) {
		for (i = 0; i < 256; ++i) {
			xboxModelSin[i] = sinf(i * (2.0f * (float)M_PI / 256.0f));
			xboxModelCos[i] = cosf(i * (2.0f * (float)M_PI / 256.0f));
		}
		xboxModelTables = qtrue;
	}
	if (backlerp == 0.0f) {
		for (i = 0; i < surface->numVerts; ++i) {
			for (k = 0; k < 3; ++k)
				xyz[i][k] = newVerts[i].xyz[k] * (float)MD3_XYZ_SCALE;
			XboxModelDecodeNormal(newVerts[i].normal, normal[i]);
		}
		return;
	}
	for (i = 0; i < surface->numVerts; ++i) {
		vec3_t n[2];

		XboxModelDecodeNormal(newVerts[i].normal, n[0]);
		XboxModelDecodeNormal(oldVerts[i].normal, n[1]);
		for (k = 0; k < 3; ++k) {
			xyz[i][k] = (newVerts[i].xyz[k] * frontlerp + oldVerts[i].xyz[k] * backlerp) *
				(float)MD3_XYZ_SCALE;
			normal[i][k] = n[0][k] * frontlerp + n[1][k] * backlerp;
		}
		VectorNormalize(normal[i]);
	}
}

void XboxNV2AModel_Bounds(qhandle_t handle, vec3_t mins, vec3_t maxs)
{
	const md3Header_t *md3 = XboxNV2AModel_Get(handle);
	const md3Frame_t *frame;

	if (XboxNV2AModel_BrushIndex(handle) >= 0) {
		XboxNV2AWorld_SubmodelBounds(XboxNV2AModel_BrushIndex(handle), mins, maxs);
		return;
	}
	if (!md3) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	frame = (const md3Frame_t *)((const byte *)md3 + md3->ofsFrames);
	VectorCopy(frame->bounds[0], mins);
	VectorCopy(frame->bounds[1], maxs);
}

static const md3Tag_t *XboxModelGetTag(const md3Header_t *md3, int frame,
	const char *tagName)
{
	const md3Tag_t *tag;
	int i;

	if (frame >= md3->numFrames)
		frame = md3->numFrames - 1;
	if (frame < 0)
		frame = 0;
	tag = (const md3Tag_t *)((const byte *)md3 + md3->ofsTags) + frame * md3->numTags;
	for (i = 0; i < md3->numTags; ++i, ++tag) {
		if (!strcmp(tag->name, tagName))
			return tag;
	}
	return NULL;
}

/* ioq3 R_LerpTag. */
int XboxNV2AModel_LerpTag(orientation_t *tag, qhandle_t handle, int startFrame,
	int endFrame, float frac, const char *tagName)
{
	const md3Header_t *md3 = XboxNV2AModel_Get(handle);
	const md3Tag_t *start;
	const md3Tag_t *end;
	float backLerp = 1.0f - frac;
	int i;

	start = md3 ? XboxModelGetTag(md3, startFrame, tagName) : NULL;
	end = md3 ? XboxModelGetTag(md3, endFrame, tagName) : NULL;
	if (!start || !end) {
		AxisClear(tag->axis);
		VectorClear(tag->origin);
		return qfalse;
	}
	for (i = 0; i < 3; ++i) {
		tag->origin[i] = start->origin[i] * backLerp + end->origin[i] * frac;
		tag->axis[0][i] = start->axis[0][i] * backLerp + end->axis[0][i] * frac;
		tag->axis[1][i] = start->axis[1][i] * backLerp + end->axis[1][i] * frac;
		tag->axis[2][i] = start->axis[2][i] * backLerp + end->axis[2][i] * frac;
	}
	VectorNormalize(tag->axis[0]);
	VectorNormalize(tag->axis[1]);
	VectorNormalize(tag->axis[2]);
	return qtrue;
}

/* ioq3 CommaParse: like COM_Parse, but a comma also ends a word. */
static char *XboxSkinCommaParse(char **text)
{
	static char token[MAX_TOKEN_CHARS];
	char *data = *text;
	int length = 0;
	int c = 0;

	token[0] = '\0';
	if (!data) {
		*text = NULL;
		return token;
	}
	for (;;) {
		while ((c = *data) <= ' ' && c)
			data++;
		if (c == '/' && data[1] == '/') {
			while (*data && *data != '\n')
				data++;
		} else if (c == '/' && data[1] == '*') {
			data += 2;
			while (*data && (*data != '*' || data[1] != '/'))
				data++;
			if (*data)
				data += 2;
		} else {
			break;
		}
	}
	if (!c)
		return token;
	if (c == '"') {
		data++;
		while ((c = *data++) != '"' && c) {
			if (length < MAX_TOKEN_CHARS - 1)
				token[length++] = (char)c;
		}
		token[length] = '\0';
		*text = data;
		return token;
	}
	do {
		if (length < MAX_TOKEN_CHARS - 1)
			token[length++] = (char)c;
		c = *++data;
	} while (c > 32 && c != ',');
	token[length] = '\0';
	*text = data;
	return token;
}

/* ioq3 RE_RegisterSkin; failed loads keep an empty slot so they are not retried. */
qhandle_t XboxNV2ASkin_Register(const char *name)
{
	xboxNV2ASkinSurface_t parsed[XBOX_NV2A_MAX_SKIN_SURFACES];
	xboxNV2ASkin_t *skin;
	size_t nameLength;
	void *buffer = NULL;
	char *text;
	char *token;
	int handle;

	if (!name || !*name || (nameLength = strlen(name)) >= MAX_QPATH)
		return 0;
	for (handle = 1; handle < xboxNV2ASkinCount; ++handle) {
		if (!Q_stricmp(xboxNV2ASkins[handle].name, name))
			return xboxNV2ASkins[handle].numSurfaces ? handle : 0;
	}
	if (xboxNV2ASkinCount >= XBOX_NV2A_MAX_SKINS) {
		Sys_XboxLog("Xbox skin: table full, skipped %s\n", name);
		return 0;
	}
	handle = xboxNV2ASkinCount++;
	skin = &xboxNV2ASkins[handle];
	Q_strncpyz(skin->name, name, sizeof(skin->name));
	skin->numSurfaces = 0;
	skin->surfaces = NULL;

	/* A name without ".skin" is a single shader that matches no MD3 surface, as in ioq3. */
	if (nameLength < 5 || Q_stricmp(name + nameLength - 5, ".skin")) {
		parsed[0].name[0] = '\0';
		parsed[0].shader = XboxNV2AShader_RegisterModel(name);
		skin->numSurfaces = 1;
	} else {
		if (ri.FS_ReadFile(name, &buffer) <= 0 || !buffer)
			return 0;
		text = (char *)buffer;
		while (text && *text) {
			char surfaceName[MAX_QPATH];

			token = XboxSkinCommaParse(&text);
			if (!token[0])
				break;
			Q_strncpyz(surfaceName, token, sizeof(surfaceName));
			Q_strlwr(surfaceName);
			if (*text == ',')
				text++;
			if (strstr(surfaceName, "tag_"))
				continue;
			token = XboxSkinCommaParse(&text);
			if (skin->numSurfaces < XBOX_NV2A_MAX_SKIN_SURFACES) {
				xboxNV2ASkinSurface_t *surface = &parsed[skin->numSurfaces++];

				Q_strncpyz(surface->name, surfaceName, sizeof(surface->name));
				surface->shader = XboxNV2AShader_RegisterModel(token);
			}
		}
		ri.FS_FreeFile(buffer);
		if (!skin->numSurfaces)
			return 0;
	}
	skin->surfaces = (xboxNV2ASkinSurface_t *)malloc((size_t)skin->numSurfaces *
		sizeof(*skin->surfaces));
	if (!skin->surfaces) {
		skin->numSurfaces = 0;
		return 0;
	}
	memcpy(skin->surfaces, parsed, (size_t)skin->numSurfaces * sizeof(*skin->surfaces));
	Sys_XboxLog("Xbox skin: %s surfaces=%d\n", name, skin->numSurfaces);
	return handle;
}

/* ioq3 R_AddMD3Surfaces: a surface missing from the skin gets no shader. */
int XboxNV2ASkin_Shader(qhandle_t handle, const char *surfaceName)
{
	const xboxNV2ASkin_t *skin;
	int i;

	if (handle <= 0 || handle >= xboxNV2ASkinCount)
		return 0;
	skin = &xboxNV2ASkins[handle];
	for (i = 0; i < skin->numSurfaces; ++i) {
		if (!strcmp(skin->surfaces[i].name, surfaceName))
			return skin->surfaces[i].shader;
	}
	return 0;
}

void XboxNV2ASkin_FreeAll(void)
{
	int i;

	for (i = 1; i < xboxNV2ASkinCount; ++i)
		free(xboxNV2ASkins[i].surfaces);
	memset(xboxNV2ASkins, 0, sizeof(xboxNV2ASkins));
	xboxNV2ASkinCount = 1;
}
