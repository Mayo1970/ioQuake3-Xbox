/* MD3 models for the NV2A renderer, following ioq3 tr_model.c and tr_surface.c. */
#include "xbox_nv2a.h"
#include "../renderercommon/tr_common.h"
#include "../sys/sys_xbox.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_NV2A_MAX_MODELS 256

typedef struct {
	char name[MAX_QPATH];
	md3Header_t *md3;
} xboxNV2AModel_t;

/* Index 0 is the bad model; failed loads keep a NULL entry so they are not retried. */
static xboxNV2AModel_t xboxNV2AModels[XBOX_NV2A_MAX_MODELS];
static int xboxNV2AModelCount = 1;

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

static md3Header_t *XboxModelLoadMD3(const char *name)
{
	void *buffer = NULL;
	md3Header_t *md3;
	md3Surface_t *surface;
	long length;
	int i, j;

	length = ri.FS_ReadFile(name, &buffer);
	if (length <= 0 || !buffer)
		return NULL;
	if (!XboxModelValidate((const md3Header_t *)buffer, (int)length, name)) {
		ri.FS_FreeFile(buffer);
		return NULL;
	}
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

		for (j = 0; j < surface->numShaders; ++j, ++shader) {
			shader->name[sizeof(shader->name) - 1] = '\0';
			shader->shaderIndex = XboxNV2AShader_RegisterModel(shader->name);
		}
		surface = (md3Surface_t *)((byte *)surface + surface->ofsEnd);
	}
	Sys_XboxLog("Xbox model: %s frames=%d surfaces=%d tags=%d\n", name,
		md3->numFrames, md3->numSurfaces, md3->numTags);
	return md3;
}

qhandle_t XboxNV2AModel_Register(const char *name)
{
	xboxNV2AModel_t *model;
	int i;

	if (!name || !*name || strlen(name) >= MAX_QPATH)
		return 0;
	for (i = 1; i < xboxNV2AModelCount; ++i) {
		if (!Q_stricmp(xboxNV2AModels[i].name, name))
			return xboxNV2AModels[i].md3 ? i : 0;
	}
	if (xboxNV2AModelCount >= XBOX_NV2A_MAX_MODELS) {
		Sys_XboxLog("Xbox model: table full, skipped %s\n", name);
		return 0;
	}
	model = &xboxNV2AModels[xboxNV2AModelCount++];
	Q_strncpyz(model->name, name, sizeof(model->name));
	model->md3 = XboxModelLoadMD3(name);
	return model->md3 ? xboxNV2AModelCount - 1 : 0;
}

void XboxNV2AModel_FreeAll(void)
{
	int i;

	for (i = 1; i < xboxNV2AModelCount; ++i)
		free(xboxNV2AModels[i].md3);
	memset(xboxNV2AModels, 0, sizeof(xboxNV2AModels));
	xboxNV2AModelCount = 1;
}

const md3Header_t *XboxNV2AModel_Get(qhandle_t handle)
{
	if (handle <= 0 || handle >= xboxNV2AModelCount)
		return NULL;
	return xboxNV2AModels[handle].md3;
}

/* ioq3 LerpMeshVertexes: decode 1/64 unit positions and lat/long normals. */
void XboxNV2AModel_LerpSurface(const md3Surface_t *surface, int frame,
	int oldFrame, float backlerp, float (*xyz)[3], float (*normal)[3])
{
	const md3XyzNormal_t *newVerts = (const md3XyzNormal_t *)((const byte *)surface +
		surface->ofsXyzNormals) + frame * surface->numVerts;
	const md3XyzNormal_t *oldVerts = (const md3XyzNormal_t *)((const byte *)surface +
		surface->ofsXyzNormals) + oldFrame * surface->numVerts;
	float frontlerp = 1.0f - backlerp;
	int i, k;

	for (i = 0; i < surface->numVerts; ++i) {
		vec3_t n[2];
		const md3XyzNormal_t *v[2];

		v[0] = &newVerts[i];
		v[1] = &oldVerts[i];
		for (k = 0; k < 2; ++k) {
			float lat = ((v[k]->normal >> 8) & 0xff) * (2.0f * (float)M_PI / 256.0f);
			float lng = (v[k]->normal & 0xff) * (2.0f * (float)M_PI / 256.0f);

			n[k][0] = cosf(lat) * sinf(lng);
			n[k][1] = sinf(lat) * sinf(lng);
			n[k][2] = cosf(lng);
		}
		for (k = 0; k < 3; ++k) {
			xyz[i][k] = (v[0]->xyz[k] * frontlerp + v[1]->xyz[k] * backlerp) *
				(float)MD3_XYZ_SCALE;
			normal[i][k] = n[0][k] * frontlerp + n[1][k] * backlerp;
		}
		if (backlerp != 0.0f)
			VectorNormalize(normal[i]);
	}
}

void XboxNV2AModel_Bounds(qhandle_t handle, vec3_t mins, vec3_t maxs)
{
	const md3Header_t *md3 = XboxNV2AModel_Get(handle);
	const md3Frame_t *frame;

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
