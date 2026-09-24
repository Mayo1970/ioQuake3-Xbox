/* Q3 BSP world for the NV2A renderer, following ioq3 tr_bsp.c, tr_world.c and tr_light.c. */
#include "xbox_nv2a.h"
#include "../qcommon/qcommon.h"
#include "../renderercommon/tr_common.h"
#include "../sys/sys_xbox.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/* ioq3 r_mapOverBrightBits 2 with tr.overbrightBits 0, as without hardware gamma. */
#define XBOX_WORLD_LIGHT_SHIFT 2
/* Equal Bezier steps on every 3x3 patch section keep shared patch edges crack-free. */
#define XBOX_WORLD_PATCH_STEPS 4
/* ioq3 R_CullSurface epsilon for face plane culling. */
#define XBOX_WORLD_FACE_CULL_EPSILON 8.0f
#define XBOX_WORLD_MAX_LIGHTMAPS 256
#define XBOX_WORLD_DEFAULT_ZFAR 2048.0f
#define XBOX_WORLD_MIN_ZFAR 64.0f
/* ioq3 r_ambientScale and r_directedScale defaults. */
#define XBOX_WORLD_AMBIENT_SCALE 0.6f
#define XBOX_WORLD_DIRECTED_SCALE 1.0f
/* ioq3 tr_marks.c MAX_VERTS_ON_POLY and its 64-surface list. */
#define XBOX_WORLD_MARK_VERTS 64
#define XBOX_WORLD_MARK_SURFACES 64

enum { XBOX_WORLD_FRONT, XBOX_WORLD_BACK, XBOX_WORLD_ON };

/* ioq3 mnode_t: contents is -1 for decision nodes and 0 for leafs. */
typedef struct xboxWorldNode_s {
	int contents;
	int visFrame;
	vec3_t mins;
	vec3_t maxs;
	struct xboxWorldNode_s *parent;
	cplane_t *plane;
	struct xboxWorldNode_s *children[2];
	int cluster;
	int area;
	int firstMark;
	int numMarks;
} xboxWorldNode_t;

typedef struct {
	vec3_t bounds[2];
	int firstSurface;
	int numSurfaces;
} xboxWorldModel_t;

/* Arrays live on the hunk, which the client clears after the renderer shuts down. */
typedef struct {
	qboolean loaded;
	char name[MAX_QPATH];
	cplane_t *planes;
	int numPlanes;
	xboxWorldNode_t *nodes;
	int numNodes;
	int numDecisionNodes;
	int *marks;
	int numMarks;
	xboxNV2AWorldSurface_t *surfaces;
	int numSurfaces;
	xboxWorldModel_t *models;
	int numModels;
	int numClusters;
	qboolean vised;
	byte *lightGrid;
	vec3_t gridOrigin;
	vec3_t gridSize;
	vec3_t gridInverseSize;
	int gridBounds[3];
	char *entityString;
	char *entityParse;
	int lightmaps[XBOX_WORLD_MAX_LIGHTMAPS];
	int numLightmaps;
	xboxNV2AFog_t *fogs;
	int numFogs;
	vec3_t sunDirection;
	int visCount;
	int viewCount;
	cplane_t frustum[4];
	vec3_t visBounds[2];
	/* Scene dlights, with origins in the space of the surfaces being added. */
	const xboxNV2ADlight_t *dlights;
	int numDlights;
	vec3_t dlightOrigins[XBOX_NV2A_MAX_DLIGHTS];
} xboxWorld_t;

static xboxWorld_t xboxWorld;

/* The BSP is read one lump at a time, so no step holds the whole file in hunk temp memory. */
typedef struct {
	fileHandle_t file;
	int length;
	int position;
	dheader_t header;
	/* Lowest Hunk_MemoryRemaining seen during the load, for the budget log. */
	int lowestFree;
} xboxWorldStream_t;

/* dsurface_t fields still needed after the drawverts and surfaces lumps are freed. */
typedef struct {
	qboolean parsed;
	int shaderNum;
	int lightmapNum;
	int fogNum;
	int firstIndex;
} xboxWorldPending_t;

static xboxWorldStream_t xboxWorldStream;
static xboxWorldPending_t *xboxWorldPending;
/* The shader and fog lumps, copied to the heap for the shader registration. */
static byte *xboxWorldLists;

/* XboxNV2AWorld_Free runs this too; the renderer shutdown after an ERR_DROP reaches it. */
static void XboxWorldEndLoad(void)
{
	if (xboxWorldStream.file)
		FS_FCloseFile(xboxWorldStream.file);
	xboxWorldStream.file = 0;
	free(xboxWorldPending);
	xboxWorldPending = NULL;
	free(xboxWorldLists);
	xboxWorldLists = NULL;
}

static void XboxWorldNoteFree(void)
{
	int remaining = Hunk_MemoryRemaining();

	if (remaining < xboxWorldStream.lowestFree)
		xboxWorldStream.lowestFree = remaining;
}

/* Returns the element count of a lump that fits the file and holds whole elements. */
static int XboxWorldLumpCount(int lump, int elementSize)
{
	const lump_t *l = &xboxWorldStream.header.lumps[lump];

	if (l->fileofs < 0 || l->filelen < 0 || l->fileofs > xboxWorldStream.length ||
		l->filelen > xboxWorldStream.length - l->fileofs || l->filelen % elementSize)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: lump %d is corrupt in %s", lump,
			xboxWorld.name);
	return l->filelen / elementSize;
}

/* A pk3 FS_SEEK_SET inflates again from the file start, so forward moves skip from here. */
static void XboxWorldRead(int offset, void *buffer, int bytes)
{
	xboxWorldStream_t *stream = &xboxWorldStream;

	if (offset > stream->position)
		FS_Seek(stream->file, offset - stream->position, FS_SEEK_CUR);
	else if (offset < stream->position)
		FS_Seek(stream->file, offset, FS_SEEK_SET);
	stream->position = offset;
	if (bytes > 0 && FS_Read(buffer, bytes, stream->file) != bytes)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: short read in %s", xboxWorld.name);
	stream->position += bytes;
}

/* Hunk temp memory is a stack: free lumps in the reverse order of reading. */
static void *XboxWorldReadLump(int lump, int elementSize, int *count)
{
	const lump_t *l = &xboxWorldStream.header.lumps[lump];
	void *data;

	*count = XboxWorldLumpCount(lump, elementSize);
	data = ri.Hunk_AllocateTempMemory(l->filelen);
	XboxWorldRead(l->fileofs, data, l->filelen);
	XboxWorldNoteFree();
	return data;
}

static void XboxWorldFreeLump(void *data)
{
	XboxWorldNoteFree();
	ri.Hunk_FreeTempMemory(data);
}

/* ioq3 R_ColorShiftLightingBytes: scale up, then normalise by the brightest channel. */
static void XboxWorldShiftColor(const byte *in, byte *out)
{
	int r = in[0] << XBOX_WORLD_LIGHT_SHIFT;
	int g = in[1] << XBOX_WORLD_LIGHT_SHIFT;
	int b = in[2] << XBOX_WORLD_LIGHT_SHIFT;

	if ((r | g | b) > 255) {
		int max = r > g ? r : g;

		max = max > b ? max : b;
		r = r * 255 / max;
		g = g * 255 / max;
		b = b * 255 / max;
	}
	out[0] = (byte)r;
	out[1] = (byte)g;
	out[2] = (byte)b;
}

static void XboxWorldCopyVert(const drawVert_t *in, xboxNV2AWorldVert_t *out)
{
	VectorCopy(in->xyz, out->xyz);
	out->st[0] = in->st[0];
	out->st[1] = in->st[1];
	out->lightSt[0] = in->lightmap[0];
	out->lightSt[1] = in->lightmap[1];
	VectorCopy(in->normal, out->normal);
	XboxWorldShiftColor(in->color, out->color);
	out->color[3] = in->color[3];
}

static void XboxWorldSurfaceBounds(xboxNV2AWorldSurface_t *surface)
{
	int i;

	ClearBounds(surface->bounds[0], surface->bounds[1]);
	for (i = 0; i < surface->numVerts; ++i)
		AddPointToBounds(surface->verts[i].xyz, surface->bounds[0], surface->bounds[1]);
}

/* Reads one lightmap at a time, so the lump is never in memory whole. */
static void XboxWorldLoadLightmaps(void)
{
	const int lightmapBytes = LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 3;
	const lump_t *l = &xboxWorldStream.header.lumps[LUMP_LIGHTMAPS];
	byte *source;
	byte *rgba;
	int count, i, j;

	count = XboxWorldLumpCount(LUMP_LIGHTMAPS, 1) / lightmapBytes;
	if (count > XBOX_WORLD_MAX_LIGHTMAPS) {
		Sys_XboxLog("Xbox world: %d lightmaps, only %d kept\n", count,
			XBOX_WORLD_MAX_LIGHTMAPS);
		count = XBOX_WORLD_MAX_LIGHTMAPS;
	}
	if (!count)
		return;
	source = (byte *)ri.Hunk_AllocateTempMemory(lightmapBytes);
	rgba = (byte *)ri.Hunk_AllocateTempMemory(LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT * 4);
	for (i = 0; i < count; ++i) {
		char name[MAX_QPATH];

		XboxWorldRead(l->fileofs + i * lightmapBytes, source, lightmapBytes);
		for (j = 0; j < LIGHTMAP_WIDTH * LIGHTMAP_HEIGHT; ++j) {
			XboxWorldShiftColor(source + j * 3, rgba + j * 4);
			rgba[j * 4 + 3] = 255;
		}
		Com_sprintf(name, sizeof(name), "*lightmap%d", i);
		/* R5G6B5 like the textures: half the pool of the old 32-bit lightmaps, some banding. */
		xboxWorld.lightmaps[i] = XboxNV2A_CreateImage(name, LIGHTMAP_WIDTH,
			LIGHTMAP_HEIGHT, rgba, XBOX_NV2A_IMAGE_16BIT, qfalse);
	}
	XboxWorldFreeLump(rgba);
	XboxWorldFreeLump(source);
	xboxWorld.numLightmaps = count;
}

static void XboxWorldLoadPlanes(const dplane_t *in, int count)
{
	int i;

	xboxWorld.numPlanes = count;
	xboxWorld.planes = (cplane_t *)ri.Hunk_Alloc(count * sizeof(cplane_t), h_low);
	for (i = 0; i < count; ++i) {
		cplane_t *out = &xboxWorld.planes[i];

		VectorCopy(in[i].normal, out->normal);
		out->dist = in[i].dist;
		out->type = PlaneTypeForNormal(out->normal);
		SetPlaneSignbits(out);
	}
}

/* ioq3 R_LoadFogs geometry: fog 0 means none; the bounds come from the brush's six axial sides. */
static void XboxWorldLoadFogVolumes(const dfog_t *fogs, int count, const dbrush_t *brushes,
	int numBrushes, const dbrushside_t *sides, int numSides)
{
	int i, k;

	xboxWorld.numFogs = count + 1;
	xboxWorld.fogs = (xboxNV2AFog_t *)ri.Hunk_Alloc(xboxWorld.numFogs *
		sizeof(*xboxWorld.fogs), h_low);
	for (i = 0; i < count; ++i) {
		xboxNV2AFog_t *out = &xboxWorld.fogs[i + 1];
		const dbrush_t *brush;
		int firstSide, side;

		if (fogs[i].brushNum < 0 || fogs[i].brushNum >= numBrushes)
			ri.Error(ERR_DROP, "fog brushNumber out of range");
		brush = &brushes[fogs[i].brushNum];
		firstSide = brush->firstSide;
		if (firstSide < 0 || firstSide > numSides - 6)
			ri.Error(ERR_DROP, "fog brush sideNumber out of range");
		/* Brush sides start with the axial ones: -x, +x, -y, +y, -z, +z. */
		for (k = 0; k < 6; ++k) {
			if (sides[firstSide + k].planeNum < 0 ||
				sides[firstSide + k].planeNum >= xboxWorld.numPlanes)
				ri.Error(ERR_DROP, "fog brush plane out of range");
		}
		for (k = 0; k < 3; ++k) {
			out->bounds[0][k] = -xboxWorld.planes[sides[firstSide + k * 2].planeNum].dist;
			out->bounds[1][k] = xboxWorld.planes[sides[firstSide + k * 2 + 1].planeNum].dist;
		}
		/* The visible side's plane, turned to face into the fog. */
		side = fogs[i].visibleSide;
		if (side < 0 || firstSide + side >= numSides || sides[firstSide + side].planeNum < 0 ||
			sides[firstSide + side].planeNum >= xboxWorld.numPlanes) {
			out->hasSurface = qfalse;
		} else {
			const cplane_t *plane = &xboxWorld.planes[sides[firstSide + side].planeNum];

			out->hasSurface = qtrue;
			VectorNegate(plane->normal, out->surface);
			out->surface[3] = -plane->dist;
		}
	}
}

/* ioq3 R_LoadFogs colour and depth, from the fog shader's fogParms. */
static void XboxWorldRegisterFogs(const dfog_t *fogs, int count)
{
	int i, k;

	for (i = 0; i < count; ++i) {
		xboxNV2AFog_t *out = &xboxWorld.fogs[i + 1];
		char name[MAX_QPATH];
		vec3_t color;
		float depth;

		Q_strncpyz(name, fogs[i].shader, sizeof(name));
		XboxNV2A_ShaderFogParms(XboxNV2AShader_RegisterWorld(name, XBOX_NV2A_SHADER_VERTEX),
			color, &depth);
		for (k = 0; k < 3; ++k)
			out->color[k] = (byte)(255 * Com_Clamp(0.0f, 1.0f, color[k]));
		out->color[3] = 255;
		out->tcScale = 1.0f / ((depth < 1.0f ? 1.0f : depth) * 8);
	}
}

/* Copies a planar face's or triangle soup's vertices; the indexes come with the drawindexes lump. */
static qboolean XboxWorldParseTriangles(const dsurface_t *ds, const drawVert_t *verts,
	int numVerts, int numIndexes, xboxNV2AWorldSurface_t *surface)
{
	int i;

	if (ds->firstVert < 0 || ds->numVerts <= 0 || ds->firstVert > numVerts ||
		ds->numVerts > numVerts - ds->firstVert || ds->firstIndex < 0 ||
		ds->numIndexes <= 0 || ds->numIndexes % 3 || ds->firstIndex > numIndexes ||
		ds->numIndexes > numIndexes - ds->firstIndex ||
		ds->numVerts > XBOX_NV2A_TESS_VERTS || ds->numIndexes > XBOX_NV2A_TESS_INDEXES)
		return qfalse;
	surface->numVerts = ds->numVerts;
	surface->numIndexes = ds->numIndexes;
	surface->verts = (xboxNV2AWorldVert_t *)ri.Hunk_Alloc(surface->numVerts *
		sizeof(*surface->verts), h_low);
	surface->indexes = (unsigned short *)ri.Hunk_Alloc(surface->numIndexes *
		sizeof(*surface->indexes), h_low);
	for (i = 0; i < surface->numVerts; ++i)
		XboxWorldCopyVert(&verts[ds->firstVert + i], &surface->verts[i]);
	XboxWorldSurfaceBounds(surface);
	return qtrue;
}

/* Quadratic Bezier through points[0], points[stride] and points[2 * stride]. */
static void XboxWorldBezier(const xboxNV2AWorldVert_t *points, int stride, float t,
	xboxNV2AWorldVert_t *out)
{
	const xboxNV2AWorldVert_t *a = &points[0];
	const xboxNV2AWorldVert_t *b = &points[stride];
	const xboxNV2AWorldVert_t *c = &points[stride * 2];
	float wa = (1.0f - t) * (1.0f - t);
	float wb = 2.0f * t * (1.0f - t);
	float wc = t * t;
	int k;

	for (k = 0; k < 3; ++k) {
		out->xyz[k] = a->xyz[k] * wa + b->xyz[k] * wb + c->xyz[k] * wc;
		out->normal[k] = a->normal[k] * wa + b->normal[k] * wb + c->normal[k] * wc;
	}
	for (k = 0; k < 2; ++k) {
		out->st[k] = a->st[k] * wa + b->st[k] * wb + c->st[k] * wc;
		out->lightSt[k] = a->lightSt[k] * wa + b->lightSt[k] * wb + c->lightSt[k] * wc;
	}
	for (k = 0; k < 4; ++k)
		out->color[k] = (byte)(a->color[k] * wa + b->color[k] * wb + c->color[k] * wc + 0.5f);
}

/* Step index to (section, t): the last step of a row ends its last section at t = 1. */
static float XboxWorldPatchParam(int step, int steps, int sections, int *section)
{
	*section = step / steps;
	if (*section >= sections)
		*section = sections - 1;
	return (float)(step - *section * steps) / (float)steps;
}

/* Uniform tessellation instead of ioq3's error-driven R_SubdividePatchToGrid. */
static qboolean XboxWorldParsePatch(const dsurface_t *ds, const drawVert_t *verts,
	int numVerts, xboxNV2AWorldSurface_t *surface)
{
	int width = ds->patchWidth;
	int height = ds->patchHeight;
	int sectionsX, sectionsY, steps, outWidth, outHeight;
	xboxNV2AWorldVert_t *control;
	xboxNV2AWorldVert_t *rows;
	int x, y, i, n;

	if (width < 3 || height < 3 || !(width & 1) || !(height & 1) ||
		ds->numVerts != width * height || ds->firstVert < 0 ||
		ds->firstVert > numVerts || ds->numVerts > numVerts - ds->firstVert)
		return qfalse;
	sectionsX = (width - 1) / 2;
	sectionsY = (height - 1) / 2;
	for (steps = XBOX_WORLD_PATCH_STEPS; ; --steps) {
		outWidth = sectionsX * steps + 1;
		outHeight = sectionsY * steps + 1;
		if (outWidth * outHeight <= XBOX_NV2A_TESS_VERTS &&
			(outWidth - 1) * (outHeight - 1) * 6 <= XBOX_NV2A_TESS_INDEXES)
			break;
		if (steps == 1) {
			Sys_XboxLog("Xbox world: skipped a %dx%d patch\n", width, height);
			return qfalse;
		}
	}

	control = (xboxNV2AWorldVert_t *)malloc((size_t)width * height * sizeof(*control));
	rows = (xboxNV2AWorldVert_t *)malloc((size_t)height * outWidth * sizeof(*rows));
	if (!control || !rows) {
		free(control);
		free(rows);
		ri.Error(ERR_DROP, "RE_LoadWorldMap: no memory for a patch");
	}
	for (i = 0; i < width * height; ++i)
		XboxWorldCopyVert(&verts[ds->firstVert + i], &control[i]);
	/* First along every control row, then down the columns of those results. */
	for (y = 0; y < height; ++y) {
		for (x = 0; x < outWidth; ++x) {
			int section;
			float t = XboxWorldPatchParam(x, steps, sectionsX, &section);

			XboxWorldBezier(&control[y * width + section * 2], 1, t, &rows[y * outWidth + x]);
		}
	}
	surface->numVerts = outWidth * outHeight;
	surface->numIndexes = (outWidth - 1) * (outHeight - 1) * 6;
	surface->verts = (xboxNV2AWorldVert_t *)ri.Hunk_Alloc(surface->numVerts *
		sizeof(*surface->verts), h_low);
	surface->indexes = (unsigned short *)ri.Hunk_Alloc(surface->numIndexes *
		sizeof(*surface->indexes), h_low);
	for (y = 0; y < outHeight; ++y) {
		int section;
		float t = XboxWorldPatchParam(y, steps, sectionsY, &section);

		for (x = 0; x < outWidth; ++x) {
			xboxNV2AWorldVert_t *out = &surface->verts[y * outWidth + x];

			XboxWorldBezier(&rows[section * 2 * outWidth + x], outWidth, t, out);
			VectorNormalize(out->normal);
		}
	}
	free(control);
	free(rows);

	/* ioq3 RB_SurfaceGrid triangle order. */
	n = 0;
	for (y = 0; y < outHeight - 1; ++y) {
		for (x = 0; x < outWidth - 1; ++x) {
			int v1 = y * outWidth + x + 1;
			int v2 = v1 - 1;
			int v3 = v2 + outWidth;
			int v4 = v3 + 1;

			surface->indexes[n++] = (unsigned short)v2;
			surface->indexes[n++] = (unsigned short)v3;
			surface->indexes[n++] = (unsigned short)v1;
			surface->indexes[n++] = (unsigned short)v1;
			surface->indexes[n++] = (unsigned short)v3;
			surface->indexes[n++] = (unsigned short)v4;
		}
	}
	XboxWorldSurfaceBounds(surface);
	return qtrue;
}

/* ioq3 ShaderForShaderNum; surfaceparm nodraw surfaces get no shader and are never drawn. */
static int XboxWorldShader(const dshader_t *shaders, int numShaders, int shaderNum,
	int flavor)
{
	char name[MAX_QPATH];

	if (shaderNum < 0 || shaderNum >= numShaders)
		ri.Error(ERR_DROP, "ShaderForShaderNum: bad num %i", shaderNum);
	if (shaders[shaderNum].surfaceFlags & SURF_NODRAW)
		return 0;
	Q_strncpyz(name, shaders[shaderNum].shader, sizeof(name));
	return XboxNV2AShader_RegisterWorld(name, flavor);
}

/* ioq3 R_LoadSurfaces geometry; shaders, lightmaps and fogs are resolved once the lumps are freed. */
static void XboxWorldLoadSurfaces(const drawVert_t *verts, int numVerts, const dsurface_t *in,
	int count)
{
	int numIndexes = XboxWorldLumpCount(LUMP_DRAWINDEXES, sizeof(int));
	int i;

	xboxWorld.numSurfaces = count;
	xboxWorld.surfaces = (xboxNV2AWorldSurface_t *)ri.Hunk_Alloc(count *
		sizeof(*xboxWorld.surfaces), h_low);
	xboxWorldPending = (xboxWorldPending_t *)calloc((size_t)(count ? count : 1),
		sizeof(*xboxWorldPending));
	if (!xboxWorldPending)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: no memory for %d surfaces", count);

	for (i = 0; i < count; ++i) {
		const dsurface_t *ds = &in[i];
		xboxNV2AWorldSurface_t *surface = &xboxWorld.surfaces[i];
		xboxWorldPending_t *pending = &xboxWorldPending[i];

		switch (ds->surfaceType) {
		case MST_PLANAR:
			surface->type = XBOX_NV2A_SURFACE_FACE;
			pending->parsed = XboxWorldParseTriangles(ds, verts, numVerts, numIndexes, surface);
			if (pending->parsed) {
				VectorCopy(ds->lightmapVecs[2], surface->plane.normal);
				surface->plane.dist = DotProduct(surface->verts[0].xyz, surface->plane.normal);
				surface->plane.type = PlaneTypeForNormal(surface->plane.normal);
				SetPlaneSignbits(&surface->plane);
			}
			break;
		case MST_PATCH:
			surface->type = XBOX_NV2A_SURFACE_GRID;
			pending->parsed = XboxWorldParsePatch(ds, verts, numVerts, surface);
			break;
		case MST_TRIANGLE_SOUP:
			surface->type = XBOX_NV2A_SURFACE_TRIANGLES;
			pending->parsed = XboxWorldParseTriangles(ds, verts, numVerts, numIndexes, surface);
			break;
		default:
			break;
		}
		pending->shaderNum = ds->shaderNum;
		pending->lightmapNum = ds->lightmapNum;
		pending->fogNum = ds->fogNum;
		pending->firstIndex = ds->firstIndex;
	}
}

/* Face and soup indexes; one outside its surface's vertices drops the surface, as ioq3 would crash. */
static void XboxWorldLoadIndexes(const int *indexes)
{
	int i, j;

	for (i = 0; i < xboxWorld.numSurfaces; ++i) {
		xboxNV2AWorldSurface_t *surface = &xboxWorld.surfaces[i];
		xboxWorldPending_t *pending = &xboxWorldPending[i];
		const int *in;

		if (!pending->parsed || surface->type == XBOX_NV2A_SURFACE_GRID)
			continue;
		in = indexes + pending->firstIndex;
		for (j = 0; j < surface->numIndexes; ++j) {
			if ((unsigned int)in[j] >= (unsigned int)surface->numVerts) {
				pending->parsed = qfalse;
				surface->numVerts = 0;
				surface->numIndexes = 0;
				break;
			}
			surface->indexes[j] = (unsigned short)in[j];
		}
	}
}

/* ioq3 R_LoadSurfaces shader step; a triangle soup is always vertex lit. */
static void XboxWorldRegisterSurfaces(const dshader_t *shaders, int numShaders)
{
	int counts[4] = {0, 0, 0, 0};
	int i;

	for (i = 0; i < xboxWorld.numSurfaces; ++i) {
		xboxNV2AWorldSurface_t *surface = &xboxWorld.surfaces[i];
		const xboxWorldPending_t *pending = &xboxWorldPending[i];
		/* ioq3 R_FindShader: a missing lightmap falls back to vertex lighting. */
		qboolean lit = surface->type != XBOX_NV2A_SURFACE_TRIANGLES &&
			pending->lightmapNum >= 0 && pending->lightmapNum < xboxWorld.numLightmaps;

		if (!pending->parsed)
			continue;
		counts[surface->type]++;
		counts[3] += surface->numVerts;
		surface->shader = XboxWorldShader(shaders, numShaders, pending->shaderNum,
			lit ? XBOX_NV2A_SHADER_LIGHTMAP : XBOX_NV2A_SHADER_VERTEX);
		surface->lightmap = lit ? xboxWorld.lightmaps[pending->lightmapNum] : 0;
		surface->fogIndex = pending->fogNum >= 0 && pending->fogNum + 1 < xboxWorld.numFogs ?
			pending->fogNum + 1 : 0;
		/* ioq3 reads the shader's surfaceparms; q3map stores the same flags in the BSP. */
		surface->noMarks = (shaders[pending->shaderNum].surfaceFlags &
			(SURF_NOIMPACT | SURF_NOMARKS | SURF_NODRAW)) ||
			(shaders[pending->shaderNum].contentFlags & CONTENTS_FOG);
	}
	Sys_XboxLog("Xbox world: %d surfaces: faces=%d patches=%d soups=%d verts=%d\n",
		xboxWorld.numSurfaces, counts[XBOX_NV2A_SURFACE_FACE],
		counts[XBOX_NV2A_SURFACE_GRID], counts[XBOX_NV2A_SURFACE_TRIANGLES], counts[3]);
}

static void XboxWorldLoadMarks(const int *in, int count)
{
	int i;

	xboxWorld.numMarks = count;
	xboxWorld.marks = (int *)ri.Hunk_Alloc(count * sizeof(int), h_low);
	for (i = 0; i < count; ++i) {
		if (in[i] < 0 || in[i] >= xboxWorld.numSurfaces)
			ri.Error(ERR_DROP, "RE_LoadWorldMap: bad leaf surface %d", in[i]);
		xboxWorld.marks[i] = in[i];
	}
}

static void XboxWorldSetParent(xboxWorldNode_t *node, xboxWorldNode_t *parent)
{
	node->parent = parent;
	if (node->contents != -1)
		return;
	XboxWorldSetParent(node->children[0], node);
	XboxWorldSetParent(node->children[1], node);
}

/* ioq3 R_LoadNodesAndLeafs: leafs follow the decision nodes in one array; numMarks is preset. */
static void XboxWorldLoadNodes(const dnode_t *inNodes, int numDecisionNodes,
	const dleaf_t *inLeafs, int numLeafs)
{
	int i, j;

	if (!numDecisionNodes || !numLeafs)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: %s has no nodes", xboxWorld.name);
	xboxWorld.numDecisionNodes = numDecisionNodes;
	xboxWorld.numNodes = numDecisionNodes + numLeafs;
	xboxWorld.nodes = (xboxWorldNode_t *)ri.Hunk_Alloc(xboxWorld.numNodes *
		sizeof(*xboxWorld.nodes), h_low);

	for (i = 0; i < numDecisionNodes; ++i) {
		const dnode_t *in = &inNodes[i];
		xboxWorldNode_t *out = &xboxWorld.nodes[i];

		for (j = 0; j < 3; ++j) {
			out->mins[j] = (float)in->mins[j];
			out->maxs[j] = (float)in->maxs[j];
		}
		if (in->planeNum < 0 || in->planeNum >= xboxWorld.numPlanes)
			ri.Error(ERR_DROP, "RE_LoadWorldMap: bad node plane %d", in->planeNum);
		out->plane = &xboxWorld.planes[in->planeNum];
		out->contents = -1;
		for (j = 0; j < 2; ++j) {
			int child = in->children[j];

			if (child >= 0 ? child >= numDecisionNodes : -1 - child >= numLeafs)
				ri.Error(ERR_DROP, "RE_LoadWorldMap: bad node child %d", child);
			out->children[j] = child >= 0 ? &xboxWorld.nodes[child] :
				&xboxWorld.nodes[numDecisionNodes + (-1 - child)];
		}
	}
	for (i = 0; i < numLeafs; ++i) {
		const dleaf_t *in = &inLeafs[i];
		xboxWorldNode_t *out = &xboxWorld.nodes[numDecisionNodes + i];

		for (j = 0; j < 3; ++j) {
			out->mins[j] = (float)in->mins[j];
			out->maxs[j] = (float)in->maxs[j];
		}
		out->cluster = in->cluster;
		out->area = in->area;
		if (out->cluster >= xboxWorld.numClusters)
			xboxWorld.numClusters = out->cluster + 1;
		if (in->firstLeafSurface < 0 || in->numLeafSurfaces < 0 ||
			in->firstLeafSurface > xboxWorld.numMarks ||
			in->numLeafSurfaces > xboxWorld.numMarks - in->firstLeafSurface)
			ri.Error(ERR_DROP, "RE_LoadWorldMap: bad leaf surfaces in leaf %d", i);
		out->firstMark = in->firstLeafSurface;
		out->numMarks = in->numLeafSurfaces;
	}
	XboxWorldSetParent(xboxWorld.nodes, NULL);
}

/* ioq3 R_LoadSubmodels; model 0 is the world itself, and numSurfaces is preset. */
static void XboxWorldLoadSubmodels(const dmodel_t *in, int count)
{
	int i;

	if (!count)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: %s has no models", xboxWorld.name);
	xboxWorld.numModels = count;
	xboxWorld.models = (xboxWorldModel_t *)ri.Hunk_Alloc(count * sizeof(*xboxWorld.models),
		h_low);
	for (i = 0; i < count; ++i) {
		xboxWorldModel_t *out = &xboxWorld.models[i];

		VectorCopy(in[i].mins, out->bounds[0]);
		VectorCopy(in[i].maxs, out->bounds[1]);
		if (in[i].firstSurface < 0 || in[i].numSurfaces < 0 ||
			in[i].firstSurface > xboxWorld.numSurfaces ||
			in[i].numSurfaces > xboxWorld.numSurfaces - in[i].firstSurface)
			ri.Error(ERR_DROP, "RE_LoadWorldMap: bad surfaces in model %d", i);
		out->firstSurface = in[i].firstSurface;
		out->numSurfaces = in[i].numSurfaces;
		if (i > 0) {
			char name[MAX_QPATH];

			Com_sprintf(name, sizeof(name), "*%d", i);
			XboxNV2AModel_RegisterBrush(name, i);
		}
	}
}

/* The collision map owns the PVS rows (ri.CM_ClusterPVS), so only the cluster count is read. */
static int XboxWorldReadVisibility(void)
{
	const lump_t *l = &xboxWorldStream.header.lumps[LUMP_VISIBILITY];
	int vis[2];

	if (XboxWorldLumpCount(LUMP_VISIBILITY, 1) < (int)sizeof(vis))
		return -1;
	XboxWorldRead(l->fileofs, vis, sizeof(vis));
	return vis[0];
}

/* ioq3 R_LoadEntities keeps the string for GetEntityToken and reads worldspawn's gridsize. */
static void XboxWorldLoadEntities(const char *in, int bytes)
{
	char *p;
	char *token;

	xboxWorld.entityString = (char *)ri.Hunk_Alloc(bytes + 1, h_low);
	memcpy(xboxWorld.entityString, in, (size_t)bytes);
	xboxWorld.entityString[bytes] = '\0';
	xboxWorld.entityParse = xboxWorld.entityString;

	p = xboxWorld.entityString;
	token = COM_ParseExt(&p, qtrue);
	if (token[0] != '{')
		return;
	for (;;) {
		char key[MAX_TOKEN_CHARS];
		char value[MAX_TOKEN_CHARS];
		char *v;
		int i;

		token = COM_ParseExt(&p, qtrue);
		if (!token[0] || token[0] == '}')
			break;
		Q_strncpyz(key, token, sizeof(key));
		token = COM_ParseExt(&p, qtrue);
		if (!token[0] || token[0] == '}')
			break;
		Q_strncpyz(value, token, sizeof(value));
		if (Q_stricmp(key, "gridsize"))
			continue;
		/* atof, not sscanf: nxdk's strtod path asserts. */
		v = value;
		for (i = 0; i < 3; ++i)
			xboxWorld.gridSize[i] = (float)atof(COM_Parse(&v));
	}
}

/* ioq3 R_LoadLightGrid: the grid spans the world model bounds in gridSize steps. */
static void XboxWorldLoadLightGrid(const byte *data, int bytes)
{
	const float *mins = xboxWorld.models[0].bounds[0];
	const float *maxs = xboxWorld.models[0].bounds[1];
	int points, i;

	for (i = 0; i < 3; ++i) {
		float top;

		if (xboxWorld.gridSize[i] <= 0.0f)
			return;
		xboxWorld.gridInverseSize[i] = 1.0f / xboxWorld.gridSize[i];
		xboxWorld.gridOrigin[i] = xboxWorld.gridSize[i] *
			ceilf(mins[i] / xboxWorld.gridSize[i]);
		top = xboxWorld.gridSize[i] * floorf(maxs[i] / xboxWorld.gridSize[i]);
		xboxWorld.gridBounds[i] = (int)((top - xboxWorld.gridOrigin[i]) /
			xboxWorld.gridSize[i]) + 1;
		if (xboxWorld.gridBounds[i] <= 0)
			return;
	}
	points = xboxWorld.gridBounds[0] * xboxWorld.gridBounds[1] * xboxWorld.gridBounds[2];
	if (bytes != points * 8) {
		Sys_XboxLog("Xbox world: light grid mismatch (%d bytes for %d points)\n", bytes,
			points);
		return;
	}
	xboxWorld.lightGrid = (byte *)ri.Hunk_Alloc(bytes, h_low);
	memcpy(xboxWorld.lightGrid, data, (size_t)bytes);
	for (i = 0; i < points; ++i) {
		XboxWorldShiftColor(&xboxWorld.lightGrid[i * 8], &xboxWorld.lightGrid[i * 8]);
		XboxWorldShiftColor(&xboxWorld.lightGrid[i * 8 + 3], &xboxWorld.lightGrid[i * 8 + 3]);
	}
}

/* ioq3 RE_LoadWorldMap in BSP file order, then one rewind for the early nodes, brushes and marks. */
void XboxNV2AWorld_Load(const char *name)
{
	xboxWorldStream_t *stream = &xboxWorldStream;
	void *shaders, *fogs, *first, *second;
	int numShaders, numFogs, count, secondCount;
	int visClusters;

	if (xboxWorld.loaded) {
		Sys_XboxLog("Xbox world: %s is already loaded\n", xboxWorld.name);
		return;
	}
	XboxWorldEndLoad();
	memset(&xboxWorld, 0, sizeof(xboxWorld));
	memset(stream, 0, sizeof(*stream));
	Q_strncpyz(xboxWorld.name, name, sizeof(xboxWorld.name));
	/* A private pk3 handle, as ioq3's streamed sounds use: the shared one moves with other reads. */
	stream->length = (int)FS_FOpenFileRead(name, &stream->file, qtrue);
	if (!stream->file || stream->length <= 0)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: %s not found", name);
	stream->lowestFree = Hunk_MemoryRemaining();
	if (stream->length < (int)sizeof(stream->header))
		ri.Error(ERR_DROP, "RE_LoadWorldMap: %s is not a version %d BSP", name, BSP_VERSION);
	XboxWorldRead(0, &stream->header, sizeof(stream->header));
	if (stream->header.ident != BSP_IDENT || stream->header.version != BSP_VERSION)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: %s is not a version %d BSP", name, BSP_VERSION);
	VectorSet(xboxWorld.gridSize, 64.0f, 64.0f, 128.0f);
	xboxWorld.numSurfaces = XboxWorldLumpCount(LUMP_SURFACES, sizeof(dsurface_t));
	xboxWorld.numMarks = XboxWorldLumpCount(LUMP_LEAFSURFACES, sizeof(int));

	shaders = XboxWorldReadLump(LUMP_SHADERS, sizeof(dshader_t), &numShaders);
	first = XboxWorldReadLump(LUMP_PLANES, sizeof(dplane_t), &count);
	XboxWorldLoadPlanes((const dplane_t *)first, count);
	XboxWorldFreeLump(first);
	first = XboxWorldReadLump(LUMP_MODELS, sizeof(dmodel_t), &count);
	XboxWorldLoadSubmodels((const dmodel_t *)first, count);
	XboxWorldFreeLump(first);
	/* The load's peak: both lumps plus the surfaces built from them. */
	first = XboxWorldReadLump(LUMP_DRAWVERTS, sizeof(drawVert_t), &count);
	second = XboxWorldReadLump(LUMP_SURFACES, sizeof(dsurface_t), &secondCount);
	XboxWorldLoadSurfaces((const drawVert_t *)first, count, (const dsurface_t *)second,
		secondCount);
	XboxWorldFreeLump(second);
	XboxWorldFreeLump(first);
	visClusters = XboxWorldReadVisibility();
	XboxWorldLoadLightmaps();
	first = XboxWorldReadLump(LUMP_LIGHTGRID, 1, &count);
	second = XboxWorldReadLump(LUMP_ENTITIES, 1, &secondCount);
	XboxWorldLoadEntities((const char *)second, secondCount);
	XboxWorldFreeLump(second);
	XboxWorldLoadLightGrid((const byte *)first, count);
	XboxWorldFreeLump(first);
	fogs = XboxWorldReadLump(LUMP_FOGS, sizeof(dfog_t), &numFogs);
	first = XboxWorldReadLump(LUMP_DRAWINDEXES, sizeof(int), &count);
	XboxWorldLoadIndexes((const int *)first);
	XboxWorldFreeLump(first);

	first = XboxWorldReadLump(LUMP_LEAFS, sizeof(dleaf_t), &count);
	second = XboxWorldReadLump(LUMP_NODES, sizeof(dnode_t), &secondCount);
	XboxWorldLoadNodes((const dnode_t *)second, secondCount, (const dleaf_t *)first, count);
	XboxWorldFreeLump(second);
	XboxWorldFreeLump(first);
	/* As ioq3's R_LoadVisibility after the leafs: the vis header's count wins. */
	if (visClusters >= 0) {
		xboxWorld.numClusters = visClusters;
		xboxWorld.vised = qtrue;
	}
	if (numFogs) {
		first = XboxWorldReadLump(LUMP_BRUSHES, sizeof(dbrush_t), &count);
		second = XboxWorldReadLump(LUMP_BRUSHSIDES, sizeof(dbrushside_t), &secondCount);
		XboxWorldLoadFogVolumes((const dfog_t *)fogs, numFogs, (const dbrush_t *)first, count,
			(const dbrushside_t *)second, secondCount);
		XboxWorldFreeLump(second);
		XboxWorldFreeLump(first);
	} else {
		XboxWorldLoadFogVolumes(NULL, 0, NULL, 0, NULL, 0);
	}
	first = XboxWorldReadLump(LUMP_LEAFSURFACES, sizeof(int), &count);
	XboxWorldLoadMarks((const int *)first, count);
	XboxWorldFreeLump(first);
	FS_FCloseFile(stream->file);
	stream->file = 0;

	/* Image loads come last; their FS_FreeFile clears all hunk temp memory, so the lists move out. */
	xboxWorldLists = (byte *)malloc((size_t)numShaders * sizeof(dshader_t) +
		(size_t)numFogs * sizeof(dfog_t) + 1);
	if (!xboxWorldLists)
		ri.Error(ERR_DROP, "RE_LoadWorldMap: no memory for the shader list");
	memcpy(xboxWorldLists, shaders, (size_t)numShaders * sizeof(dshader_t));
	memcpy(xboxWorldLists + (size_t)numShaders * sizeof(dshader_t), fogs,
		(size_t)numFogs * sizeof(dfog_t));
	XboxWorldFreeLump(fogs);
	XboxWorldFreeLump(shaders);
	XboxWorldRegisterFogs((const dfog_t *)(xboxWorldLists + (size_t)numShaders *
		sizeof(dshader_t)), numFogs);
	XboxWorldRegisterSurfaces((const dshader_t *)xboxWorldLists, numShaders);
	XboxWorldEndLoad();
	/* ioq3 default sun direction, used for entity light without a light grid. */
	VectorSet(xboxWorld.sunDirection, 0.45f, 0.3f, 0.9f);
	VectorNormalize(xboxWorld.sunDirection);

	xboxWorld.loaded = qtrue;
	Sys_XboxLog("Xbox world: %s lightmaps=%d leafs=%d clusters=%d vis=%d grid=%dx%dx%d "
		"models=%d hunk_free=%d KiB lowest=%d KiB\n", name, xboxWorld.numLightmaps,
		xboxWorld.numNodes - xboxWorld.numDecisionNodes, xboxWorld.numClusters,
		xboxWorld.vised, xboxWorld.lightGrid ? xboxWorld.gridBounds[0] : 0,
		xboxWorld.gridBounds[1], xboxWorld.gridBounds[2], xboxWorld.numModels,
		Hunk_MemoryRemaining() / 1024, stream->lowestFree / 1024);
	Sys_XboxMemoryReport("after world load");
}

/* The hunk owns the world data; the client clears it after the renderer shuts down. */
void XboxNV2AWorld_Free(void)
{
	XboxWorldEndLoad();
	memset(&xboxWorld, 0, sizeof(xboxWorld));
}

qboolean XboxNV2AWorld_Loaded(void)
{
	return xboxWorld.loaded;
}

static xboxWorldNode_t *XboxWorldPointInLeaf(const vec3_t point)
{
	xboxWorldNode_t *node = xboxWorld.nodes;

	while (node->contents == -1) {
		float d = DotProduct(point, node->plane->normal) - node->plane->dist;

		node = d > 0.0f ? node->children[0] : node->children[1];
	}
	return node;
}

/* ioq3 R_SetupFrustum: four side planes through the view origin, facing inwards. */
static void XboxWorldSetupFrustum(const refdef_t *fd)
{
	float angle = DEG2RAD(fd->fov_x) * 0.5f;
	float s = sinf(angle);
	float c = cosf(angle);
	int i;

	VectorScale(fd->viewaxis[0], s, xboxWorld.frustum[0].normal);
	VectorMA(xboxWorld.frustum[0].normal, c, fd->viewaxis[1], xboxWorld.frustum[0].normal);
	VectorScale(fd->viewaxis[0], s, xboxWorld.frustum[1].normal);
	VectorMA(xboxWorld.frustum[1].normal, -c, fd->viewaxis[1], xboxWorld.frustum[1].normal);
	angle = DEG2RAD(fd->fov_y) * 0.5f;
	s = sinf(angle);
	c = cosf(angle);
	VectorScale(fd->viewaxis[0], s, xboxWorld.frustum[2].normal);
	VectorMA(xboxWorld.frustum[2].normal, c, fd->viewaxis[2], xboxWorld.frustum[2].normal);
	VectorScale(fd->viewaxis[0], s, xboxWorld.frustum[3].normal);
	VectorMA(xboxWorld.frustum[3].normal, -c, fd->viewaxis[2], xboxWorld.frustum[3].normal);
	for (i = 0; i < 4; ++i) {
		xboxWorld.frustum[i].type = PLANE_NON_AXIAL;
		xboxWorld.frustum[i].dist = DotProduct(fd->vieworg, xboxWorld.frustum[i].normal);
		SetPlaneSignbits(&xboxWorld.frustum[i]);
	}
}

/* ioq3 R_MarkLeaves: mark every PVS leaf in an open area, and its parents. */
static void XboxWorldMarkLeaves(const refdef_t *fd, const vec3_t pvsOrigin)
{
	int cluster = XboxWorldPointInLeaf(pvsOrigin)->cluster;
	const byte *vis;
	int i;

	xboxWorld.visCount++;
	if (!xboxWorld.vised || cluster < 0 || cluster >= xboxWorld.numClusters) {
		for (i = 0; i < xboxWorld.numNodes; ++i)
			xboxWorld.nodes[i].visFrame = xboxWorld.visCount;
		return;
	}
	vis = ri.CM_ClusterPVS(cluster);
	for (i = xboxWorld.numDecisionNodes; i < xboxWorld.numNodes; ++i) {
		xboxWorldNode_t *node = &xboxWorld.nodes[i];
		int leafCluster = node->cluster;

		if (leafCluster < 0 || leafCluster >= xboxWorld.numClusters ||
			!(vis[leafCluster >> 3] & (1 << (leafCluster & 7))))
			continue;
		/* areamask bits mark areas closed off by doors. */
		if (node->area >= 0 && node->area < MAX_MAP_AREA_BYTES * 8 &&
			(fd->areamask[node->area >> 3] & (1 << (node->area & 7))))
			continue;
		for (; node && node->visFrame != xboxWorld.visCount; node = node->parent)
			node->visFrame = xboxWorld.visCount;
	}
}

/* ioq3 R_TransformDlights: loads the scene dlights in ref's space, or world space without ref. */
static unsigned int XboxWorldLoadDlights(const refEntity_t *ref)
{
	int i, k;

	xboxWorld.numDlights = XboxNV2A_SceneDlights(&xboxWorld.dlights);
	for (i = 0; i < xboxWorld.numDlights; ++i) {
		vec3_t delta;

		if (!ref) {
			VectorCopy(xboxWorld.dlights[i].origin, xboxWorld.dlightOrigins[i]);
			continue;
		}
		VectorSubtract(xboxWorld.dlights[i].origin, ref->origin, delta);
		for (k = 0; k < 3; ++k)
			xboxWorld.dlightOrigins[i][k] = DotProduct(delta, ref->axis[k]);
	}
	return xboxWorld.numDlights >= XBOX_NV2A_MAX_DLIGHTS ? ~0u :
		(1u << xboxWorld.numDlights) - 1;
}

/* ioq3 R_DlightSurface: faces test their plane, grids their bounds, soups keep every dlight. */
static unsigned int XboxWorldDlightSurface(const xboxNV2AWorldSurface_t *surface,
	unsigned int dlightBits)
{
	int i, k;

	for (i = 0; i < xboxWorld.numDlights; ++i) {
		const float *origin = xboxWorld.dlightOrigins[i];
		float radius = xboxWorld.dlights[i].radius;

		if (!(dlightBits & (1u << i)))
			continue;
		if (surface->type == XBOX_NV2A_SURFACE_FACE) {
			float d = DotProduct(origin, surface->plane.normal) - surface->plane.dist;

			if (d < -radius || d > radius)
				dlightBits &= ~(1u << i);
		} else if (surface->type == XBOX_NV2A_SURFACE_GRID) {
			for (k = 0; k < 3; ++k) {
				if (origin[k] - radius > surface->bounds[1][k] ||
					origin[k] + radius < surface->bounds[0][k])
					break;
			}
			if (k < 3)
				dlightBits &= ~(1u << i);
		}
	}
	return dlightBits;
}

/* ioq3 R_AddWorldSurface and R_CullSurface; sky surfaces go to the sky box instead. */
static void XboxWorldAddSurface(xboxNV2AWorldSurface_t *surface, const vec3_t viewOrigin,
	unsigned int dlightBits)
{
	qboolean sky;
	int cull;
	int i;

	if (surface->viewCount == xboxWorld.viewCount)
		return;
	surface->viewCount = xboxWorld.viewCount;
	sky = XboxNV2A_ShaderIsSky(surface->shader);
	if (!sky && !XboxNV2A_ShaderIsDrawable(surface->shader))
		return;
	if (surface->type == XBOX_NV2A_SURFACE_FACE) {
		float d = DotProduct(viewOrigin, surface->plane.normal);

		cull = XboxNV2A_ShaderCull(surface->shader);
		if (cull == XBOX_NV2A_CULL_FRONT &&
			d < surface->plane.dist - XBOX_WORLD_FACE_CULL_EPSILON)
			return;
		if (cull == XBOX_NV2A_CULL_BACK &&
			d > surface->plane.dist + XBOX_WORLD_FACE_CULL_EPSILON)
			return;
	} else {
		for (i = 0; i < 4; ++i) {
			if (BoxOnPlaneSide(surface->bounds[0], surface->bounds[1],
				&xboxWorld.frustum[i]) == 2)
				return;
		}
	}
	if (sky) {
		XboxNV2A_AddSkySurface(surface);
		return;
	}
	surface->dlightBits = dlightBits ? XboxWorldDlightSurface(surface, dlightBits) : 0;
	XboxNV2A_AddWorldSurface(surface, XBOX_NV2A_WORLD_ENTITY);
}

/* ioq3 R_RecursiveWorldNode: planeBits drop frustum planes, dlightBits the dlights out of reach. */
static void XboxWorldRecursiveNode(xboxWorldNode_t *node, int planeBits,
	unsigned int dlightBits, const vec3_t viewOrigin)
{
	int i;

	for (;;) {
		unsigned int newDlights[2] = {0, 0};

		if (node->visFrame != xboxWorld.visCount)
			return;
		for (i = 0; i < 4; ++i) {
			if (planeBits & (1 << i)) {
				int side = BoxOnPlaneSide(node->mins, node->maxs, &xboxWorld.frustum[i]);

				if (side == 2)
					return;
				if (side == 1)
					planeBits &= ~(1 << i);
			}
		}
		if (node->contents != -1)
			break;
		for (i = 0; i < xboxWorld.numDlights; ++i) {
			float dist;

			if (!(dlightBits & (1u << i)))
				continue;
			dist = DotProduct(xboxWorld.dlightOrigins[i], node->plane->normal) -
				node->plane->dist;
			if (dist > -xboxWorld.dlights[i].radius)
				newDlights[0] |= 1u << i;
			if (dist < xboxWorld.dlights[i].radius)
				newDlights[1] |= 1u << i;
		}
		XboxWorldRecursiveNode(node->children[0], planeBits, newDlights[0], viewOrigin);
		node = node->children[1];
		dlightBits = newDlights[1];
	}
	AddPointToBounds(node->mins, xboxWorld.visBounds[0], xboxWorld.visBounds[1]);
	AddPointToBounds(node->maxs, xboxWorld.visBounds[0], xboxWorld.visBounds[1]);
	for (i = 0; i < node->numMarks; ++i)
		XboxWorldAddSurface(&xboxWorld.surfaces[xboxWorld.marks[node->firstMark + i]],
			viewOrigin, dlightBits);
}

/* Adds the visible world surfaces and returns ioq3 R_SetFarClip's far plane distance. */
float XboxNV2AWorld_AddSurfaces(const refdef_t *fd, const vec3_t pvsOrigin)
{
	float farthest = 0.0f;
	int i;

	if (!xboxWorld.loaded)
		return XBOX_WORLD_DEFAULT_ZFAR;
	xboxWorld.viewCount++;
	XboxWorldSetupFrustum(fd);
	XboxWorldMarkLeaves(fd, pvsOrigin);
	ClearBounds(xboxWorld.visBounds[0], xboxWorld.visBounds[1]);
	XboxWorldRecursiveNode(xboxWorld.nodes, 15, XboxWorldLoadDlights(NULL), fd->vieworg);
	if (xboxWorld.visBounds[0][0] > xboxWorld.visBounds[1][0])
		return XBOX_WORLD_DEFAULT_ZFAR;
	for (i = 0; i < 8; ++i) {
		vec3_t delta;
		float d;

		delta[0] = xboxWorld.visBounds[i & 1][0] - fd->vieworg[0];
		delta[1] = xboxWorld.visBounds[(i >> 1) & 1][1] - fd->vieworg[1];
		delta[2] = xboxWorld.visBounds[(i >> 2) & 1][2] - fd->vieworg[2];
		d = DotProduct(delta, delta);
		if (d > farthest)
			farthest = d;
	}
	farthest = sqrtf(farthest);
	return farthest > XBOX_WORLD_MIN_ZFAR ? farthest : XBOX_WORLD_MIN_ZFAR;
}

/* ioq3 R_AddBrushModelSurfaces; the GPU applies the entity transform. */
void XboxNV2AWorld_AddBrushModel(int submodel, int entity, const refEntity_t *ref)
{
	const xboxWorldModel_t *model;
	unsigned int dlightBits, mask = 0;
	int i, k;

	if (!xboxWorld.loaded || submodel <= 0 || submodel >= xboxWorld.numModels)
		return;
	model = &xboxWorld.models[submodel];
	/* ioq3 R_DlightBmodel: the dlights whose sphere touches the model's local bounds. */
	dlightBits = XboxWorldLoadDlights(ref);
	for (i = 0; i < xboxWorld.numDlights; ++i) {
		const float *origin = xboxWorld.dlightOrigins[i];
		float radius = xboxWorld.dlights[i].radius;

		for (k = 0; k < 3; ++k) {
			if (origin[k] - model->bounds[1][k] > radius ||
				model->bounds[0][k] - origin[k] > radius)
				break;
		}
		if (k == 3)
			mask |= 1u << i;
	}
	mask &= dlightBits;
	for (i = 0; i < model->numSurfaces; ++i) {
		xboxNV2AWorldSurface_t *surface = &xboxWorld.surfaces[model->firstSurface + i];

		if (!XboxNV2A_ShaderIsDrawable(surface->shader))
			continue;
		/* ioq3 passes needDlights (0 or 1) here, so only dlight 0 reached bmodels. */
		surface->dlightBits = mask ? XboxWorldDlightSurface(surface, mask) : 0;
		XboxNV2A_AddWorldSurface(surface, entity);
	}
}

void XboxNV2AWorld_SubmodelBounds(int submodel, vec3_t mins, vec3_t maxs)
{
	if (!xboxWorld.loaded || submodel < 0 || submodel >= xboxWorld.numModels) {
		VectorClear(mins);
		VectorClear(maxs);
		return;
	}
	VectorCopy(xboxWorld.models[submodel].bounds[0], mins);
	VectorCopy(xboxWorld.models[submodel].bounds[1], maxs);
}

/* ioq3 tr.sunDirection: zero until a world is loaded. */
void XboxNV2AWorld_SunDirection(vec3_t direction)
{
	VectorCopy(xboxWorld.sunDirection, direction);
}

/* ioq3 R_SetupEntityLightingGrid: trilinear blend that skips samples inside walls. */
qboolean XboxNV2AWorld_LightGrid(const vec3_t origin, vec3_t ambient,
	vec3_t directed, vec3_t direction)
{
	vec3_t lightOrigin;
	const byte *gridData;
	float frac[3];
	float totalFactor = 0.0f;
	int pos[3];
	int gridStep[3];
	int i, j;

	if (!xboxWorld.loaded || !xboxWorld.lightGrid)
		return qfalse;
	VectorSubtract(origin, xboxWorld.gridOrigin, lightOrigin);
	for (i = 0; i < 3; ++i) {
		float v = lightOrigin[i] * xboxWorld.gridInverseSize[i];

		pos[i] = (int)floorf(v);
		frac[i] = v - pos[i];
		if (pos[i] < 0)
			pos[i] = 0;
		else if (pos[i] > xboxWorld.gridBounds[i] - 1)
			pos[i] = xboxWorld.gridBounds[i] - 1;
	}
	VectorClear(ambient);
	VectorClear(directed);
	VectorClear(direction);
	gridStep[0] = 8;
	gridStep[1] = 8 * xboxWorld.gridBounds[0];
	gridStep[2] = 8 * xboxWorld.gridBounds[0] * xboxWorld.gridBounds[1];
	gridData = xboxWorld.lightGrid + pos[0] * gridStep[0] + pos[1] * gridStep[1] +
		pos[2] * gridStep[2];

	for (i = 0; i < 8; ++i) {
		const byte *data = gridData;
		float factor = 1.0f;
		qboolean outside = qfalse;
		float lat, lng;

		for (j = 0; j < 3; ++j) {
			if (i & (1 << j)) {
				if (pos[j] + 1 > xboxWorld.gridBounds[j] - 1) {
					outside = qtrue;
					break;
				}
				factor *= frac[j];
				data += gridStep[j];
			} else {
				factor *= 1.0f - frac[j];
			}
		}
		if (outside || !(data[0] + data[1] + data[2]))
			continue;
		totalFactor += factor;
		for (j = 0; j < 3; ++j) {
			ambient[j] += factor * data[j];
			directed[j] += factor * data[3 + j];
		}
		/* Byte angles: data[7] is latitude and data[6] longitude, 256 steps per turn. */
		lat = data[7] * (2.0f * (float)M_PI / 256.0f);
		lng = data[6] * (2.0f * (float)M_PI / 256.0f);
		direction[0] += factor * cosf(lat) * sinf(lng);
		direction[1] += factor * sinf(lat) * sinf(lng);
		direction[2] += factor * cosf(lng);
	}
	if (totalFactor > 0.0f && totalFactor < 0.99f) {
		VectorScale(ambient, 1.0f / totalFactor, ambient);
		VectorScale(directed, 1.0f / totalFactor, directed);
	}
	VectorScale(ambient, XBOX_WORLD_AMBIENT_SCALE, ambient);
	VectorScale(directed, XBOX_WORLD_DIRECTED_SCALE, directed);
	VectorNormalize(direction);
	return qtrue;
}

int XboxNV2AWorld_NumFogs(void)
{
	return xboxWorld.loaded ? xboxWorld.numFogs : 0;
}

const xboxNV2AFog_t *XboxNV2AWorld_Fog(int index)
{
	if (!xboxWorld.loaded || index <= 0 || index >= xboxWorld.numFogs)
		return NULL;
	return &xboxWorld.fogs[index];
}

/* ioq3 R_GetEntityToken: the parse restarts once the string is used up. */
qboolean XboxNV2AWorld_GetEntityToken(char *buffer, int size)
{
	const char *token;

	if (!xboxWorld.entityString) {
		if (buffer && size > 0)
			buffer[0] = '\0';
		return qfalse;
	}
	token = COM_Parse(&xboxWorld.entityParse);
	Q_strncpyz(buffer, token, size);
	if (!xboxWorld.entityParse && !token[0]) {
		xboxWorld.entityParse = xboxWorld.entityString;
		return qfalse;
	}
	return qtrue;
}

/* ioq3 R_inPVS, with the PVS rows taken from the collision map. */
qboolean XboxNV2AWorld_InPVS(const vec3_t p1, const vec3_t p2)
{
	const byte *vis;
	int c1, c2;

	if (!xboxWorld.loaded)
		return qfalse;
	c1 = XboxWorldPointInLeaf(p1)->cluster;
	c2 = XboxWorldPointInLeaf(p2)->cluster;
	if (c1 < 0 || c2 < 0)
		return qfalse;
	vis = ri.CM_ClusterPVS(c1);
	return (vis[c2 >> 3] & (1 << (c2 & 7))) != 0;
}

/* ioq3 R_ChopPolyBehindPlane: keeps the part in front; out needs two more slots than in. */
static void XboxWorldChopPoly(int numIn, vec3_t in[XBOX_WORLD_MARK_VERTS], int *numOut,
	vec3_t out[XBOX_WORLD_MARK_VERTS], const vec3_t normal, float dist, float epsilon)
{
	float dists[XBOX_WORLD_MARK_VERTS + 4];
	int sides[XBOX_WORLD_MARK_VERTS + 4];
	int counts[3] = {0, 0, 0};
	int i, j;

	*numOut = 0;
	if (numIn >= XBOX_WORLD_MARK_VERTS - 2)
		return;
	for (i = 0; i < numIn; ++i) {
		dists[i] = DotProduct(in[i], normal) - dist;
		if (dists[i] > epsilon)
			sides[i] = XBOX_WORLD_FRONT;
		else if (dists[i] < -epsilon)
			sides[i] = XBOX_WORLD_BACK;
		else
			sides[i] = XBOX_WORLD_ON;
		counts[sides[i]]++;
	}
	sides[i] = sides[0];
	dists[i] = dists[0];
	if (!counts[XBOX_WORLD_FRONT])
		return;
	if (!counts[XBOX_WORLD_BACK]) {
		*numOut = numIn;
		memcpy(out, in, (size_t)numIn * sizeof(vec3_t));
		return;
	}
	for (i = 0; i < numIn; ++i) {
		const float *p1 = in[i];
		const float *p2;
		float d, dot;

		if (sides[i] != XBOX_WORLD_BACK) {
			VectorCopy(p1, out[*numOut]);
			(*numOut)++;
		}
		if (sides[i] == XBOX_WORLD_ON || sides[i + 1] == XBOX_WORLD_ON ||
			sides[i + 1] == sides[i])
			continue;
		p2 = in[(i + 1) % numIn];
		d = dists[i] - dists[i + 1];
		dot = d == 0.0f ? 0.0f : dists[i] / d;
		for (j = 0; j < 3; ++j)
			out[*numOut][j] = p1[j] + dot * (p2[j] - p1[j]);
		(*numOut)++;
	}
}

/* ioq3 R_BoxSurfaces_r: faces and patches in the mark box, each listed once per call. */
static void XboxWorldBoxSurfaces(xboxWorldNode_t *node, vec3_t mins, vec3_t maxs,
	xboxNV2AWorldSurface_t **list, int *listLength, const vec3_t dir)
{
	int i;

	while (node->contents == -1) {
		int side = BoxOnPlaneSide(mins, maxs, node->plane);

		if (side == 1) {
			node = node->children[0];
		} else if (side == 2) {
			node = node->children[1];
		} else {
			XboxWorldBoxSurfaces(node->children[0], mins, maxs, list, listLength, dir);
			node = node->children[1];
		}
	}
	for (i = 0; i < node->numMarks && *listLength < XBOX_WORLD_MARK_SURFACES; ++i) {
		xboxNV2AWorldSurface_t *surface = &xboxWorld.surfaces[xboxWorld.marks[node->firstMark + i]];

		if (surface->noMarks || !surface->numVerts) {
			surface->viewCount = xboxWorld.viewCount;
		} else if (surface->type == XBOX_NV2A_SURFACE_FACE) {
			int side = BoxOnPlaneSide(mins, maxs, &surface->plane);

			/* The face plane must cross the box and not meet the projection at a sharp angle. */
			if (side == 1 || side == 2 || DotProduct(surface->plane.normal, dir) > -0.5f)
				surface->viewCount = xboxWorld.viewCount;
		}
		if (surface->viewCount != xboxWorld.viewCount) {
			surface->viewCount = xboxWorld.viewCount;
			list[(*listLength)++] = surface;
		}
	}
}

/* ioq3 R_AddMarkFragments: chops a triangle by every bounding plane and keeps the rest. */
static void XboxWorldAddMarkFragment(vec3_t clipPoints[2][XBOX_WORLD_MARK_VERTS],
	int numPlanes, vec3_t *normals, const float *dists, int maxPoints, vec3_t pointBuffer,
	markFragment_t *fragmentBuffer, int *returnedPoints, int *returnedFragments)
{
	int numClipPoints = 3;
	int pingPong = 0;
	markFragment_t *fragment;
	int i;

	for (i = 0; i < numPlanes && numClipPoints; ++i) {
		XboxWorldChopPoly(numClipPoints, clipPoints[pingPong], &numClipPoints,
			clipPoints[!pingPong], normals[i], dists[i], 0.5f);
		pingPong ^= 1;
	}
	if (!numClipPoints || numClipPoints + *returnedPoints > maxPoints)
		return;
	fragment = &fragmentBuffer[*returnedFragments];
	fragment->firstPoint = *returnedPoints;
	fragment->numPoints = numClipPoints;
	memcpy(pointBuffer + *returnedPoints * 3, clipPoints[pingPong],
		(size_t)numClipPoints * sizeof(vec3_t));
	*returnedPoints += numClipPoints;
	(*returnedFragments)++;
}

/* ioq3 R_MarkFragments; triangle soups are skipped, as r_marksOnTriangleMeshes is 0. */
int XboxNV2AWorld_MarkFragments(int numPoints, const vec3_t *points,
	const vec3_t projection, int maxPoints, vec3_t pointBuffer, int maxFragments,
	markFragment_t *fragmentBuffer)
{
	xboxNV2AWorldSurface_t *surfaces[XBOX_WORLD_MARK_SURFACES];
	vec3_t normals[XBOX_WORLD_MARK_VERTS + 2];
	float dists[XBOX_WORLD_MARK_VERTS + 2];
	vec3_t clipPoints[2][XBOX_WORLD_MARK_VERTS];
	vec3_t mins, maxs, projectionDir, v1, v2;
	int numSurfaces = 0;
	int returnedPoints = 0;
	int returnedFragments = 0;
	int numPlanes, i, j, k;

	if (numPoints <= 0 || maxFragments <= 0 || !xboxWorld.loaded)
		return 0;
	xboxWorld.viewCount++;
	VectorNormalize2(projection, projectionDir);
	ClearBounds(mins, maxs);
	for (i = 0; i < numPoints; ++i) {
		vec3_t temp;

		AddPointToBounds(points[i], mins, maxs);
		VectorAdd(points[i], projection, temp);
		AddPointToBounds(temp, mins, maxs);
		/* Also the leafs in front of the hit surface. */
		VectorMA(points[i], -20.0f, projectionDir, temp);
		AddPointToBounds(temp, mins, maxs);
	}
	if (numPoints > XBOX_WORLD_MARK_VERTS)
		numPoints = XBOX_WORLD_MARK_VERTS;
	for (i = 0; i < numPoints; ++i) {
		VectorSubtract(points[(i + 1) % numPoints], points[i], v1);
		VectorAdd(points[i], projection, v2);
		VectorSubtract(points[i], v2, v2);
		CrossProduct(v1, v2, normals[i]);
		VectorNormalizeFast(normals[i]);
		dists[i] = DotProduct(normals[i], points[i]);
	}
	/* Near and far planes: 20 units along the projection and 32 against it. */
	VectorCopy(projectionDir, normals[numPoints]);
	dists[numPoints] = DotProduct(normals[numPoints], points[0]) - 32.0f;
	VectorCopy(projectionDir, normals[numPoints + 1]);
	VectorInverse(normals[numPoints + 1]);
	dists[numPoints + 1] = DotProduct(normals[numPoints + 1], points[0]) - 20.0f;
	numPlanes = numPoints + 2;

	XboxWorldBoxSurfaces(xboxWorld.nodes, mins, maxs, surfaces, &numSurfaces, projectionDir);
	for (i = 0; i < numSurfaces; ++i) {
		const xboxNV2AWorldSurface_t *surface = surfaces[i];

		if (surface->type == XBOX_NV2A_SURFACE_TRIANGLES ||
			(surface->type == XBOX_NV2A_SURFACE_FACE &&
			DotProduct(surface->plane.normal, projectionDir) > -0.5f))
			continue;
		for (k = 0; k + 2 < surface->numIndexes; k += 3) {
			for (j = 0; j < 3; ++j)
				VectorCopy(surface->verts[surface->indexes[k + j]].xyz, clipPoints[0][j]);
			/* Grid cells hold two triangles in ioq3's order; each must face the projection. */
			if (surface->type == XBOX_NV2A_SURFACE_GRID) {
				vec3_t normal;

				VectorSubtract(clipPoints[0][0], clipPoints[0][1], v1);
				VectorSubtract(clipPoints[0][2], clipPoints[0][1], v2);
				CrossProduct(v1, v2, normal);
				VectorNormalizeFast(normal);
				if (DotProduct(normal, projectionDir) >= ((k / 3) & 1 ? -0.05f : -0.1f))
					continue;
			}
			XboxWorldAddMarkFragment(clipPoints, numPlanes, normals, dists, maxPoints,
				pointBuffer, fragmentBuffer, &returnedPoints, &returnedFragments);
			if (returnedFragments == maxFragments)
				return returnedFragments;
		}
	}
	return returnedFragments;
}
