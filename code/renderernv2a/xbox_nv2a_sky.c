/* Sky box and cloud layers for the NV2A renderer, following ioq3 tr_sky.c. */
#include "xbox_nv2a.h"
#include "../renderercommon/tr_common.h"

#include <math.h>

#define XBOX_SKY_HALF (XBOX_NV2A_SKY_SUBDIVISIONS / 2)
#define XBOX_SKY_ON_EPSILON 0.1f
#define XBOX_SKY_MAX_CLIP_VERTS 64
/* ioq3 R_InitSkyTexCoords: a 4096-unit world sphere, built while zFar is 1024. */
#define XBOX_SKY_RADIUS_WORLD 4096.0
#define XBOX_SKY_TABLE_BOX_SIZE (1024.0f / 1.75f)

enum { XBOX_SKY_FRONT, XBOX_SKY_BACK, XBOX_SKY_ON };

/* ioq3 sky_clip: the planes through the box edges that split a polygon between sides. */
static const vec3_t xboxSkyClip[6] = {
	{1, 1, 0}, {1, -1, 0}, {0, -1, 1}, {0, 1, 1}, {1, 0, 1}, {-1, 0, 1}
};

static float xboxSkyCloudHeight;
static float xboxSkyCloudTexCoords[XBOX_NV2A_SKY_SIDES][XBOX_NV2A_SKY_SUBDIVISIONS + 1]
	[XBOX_NV2A_SKY_SUBDIVISIONS + 1][2];

void XboxNV2ASky_Clear(xboxNV2ASkyBounds_t *bounds)
{
	int i;

	for (i = 0; i < XBOX_NV2A_SKY_SIDES; ++i) {
		bounds->mins[0][i] = bounds->mins[1][i] = 9999.0f;
		bounds->maxs[0][i] = bounds->maxs[1][i] = -9999.0f;
	}
}

/* ioq3 AddSkyPolygon: grows the (s, t) range of the side the polygon faces. */
static void XboxSkyAddPolygon(xboxNV2ASkyBounds_t *bounds, int count, vec3_t *points)
{
	/* s = [0]/[2], t = [1]/[2]; a negative entry negates that component. */
	static const int vecToSt[6][3] = {
		{-2, 3, 1}, {2, 3, -1}, {1, 3, 2}, {-1, 3, -2}, {-2, -1, 3}, {-2, 1, -3}
	};
	vec3_t v, av;
	int axis, i, j;

	VectorClear(v);
	for (i = 0; i < count; ++i)
		VectorAdd(points[i], v, v);
	av[0] = fabsf(v[0]);
	av[1] = fabsf(v[1]);
	av[2] = fabsf(v[2]);
	if (av[0] > av[1] && av[0] > av[2])
		axis = v[0] < 0 ? 1 : 0;
	else if (av[1] > av[2] && av[1] > av[0])
		axis = v[1] < 0 ? 3 : 2;
	else
		axis = v[2] < 0 ? 5 : 4;

	for (i = 0; i < count; ++i) {
		float dv, s, t;

		j = vecToSt[axis][2];
		dv = j > 0 ? points[i][j - 1] : -points[i][-j - 1];
		if (dv < 0.001f)
			continue;
		j = vecToSt[axis][0];
		s = (j < 0 ? -points[i][-j - 1] : points[i][j - 1]) / dv;
		j = vecToSt[axis][1];
		t = (j < 0 ? -points[i][-j - 1] : points[i][j - 1]) / dv;
		if (s < bounds->mins[0][axis])
			bounds->mins[0][axis] = s;
		if (t < bounds->mins[1][axis])
			bounds->mins[1][axis] = t;
		if (s > bounds->maxs[0][axis])
			bounds->maxs[0][axis] = s;
		if (t > bounds->maxs[1][axis])
			bounds->maxs[1][axis] = t;
	}
}

/* ioq3 ClipSkyPolygon; points needs one spare slot, as the first point is copied past the last. */
static void XboxSkyClipPolygon(xboxNV2ASkyBounds_t *bounds, int count, vec3_t *points,
	int stage)
{
	float dists[XBOX_SKY_MAX_CLIP_VERTS];
	int sides[XBOX_SKY_MAX_CLIP_VERTS];
	vec3_t newPoints[2][XBOX_SKY_MAX_CLIP_VERTS];
	int newCount[2];
	qboolean front = qfalse, back = qfalse;
	const float *normal;
	int i, j;

	if (count > XBOX_SKY_MAX_CLIP_VERTS - 2)
		ri.Error(ERR_DROP, "ClipSkyPolygon: MAX_CLIP_VERTS");
	if (stage == 6) {
		XboxSkyAddPolygon(bounds, count, points);
		return;
	}
	normal = xboxSkyClip[stage];
	for (i = 0; i < count; ++i) {
		float d = DotProduct(points[i], normal);

		if (d > XBOX_SKY_ON_EPSILON) {
			front = qtrue;
			sides[i] = XBOX_SKY_FRONT;
		} else if (d < -XBOX_SKY_ON_EPSILON) {
			back = qtrue;
			sides[i] = XBOX_SKY_BACK;
		} else {
			sides[i] = XBOX_SKY_ON;
		}
		dists[i] = d;
	}
	if (!front || !back) {
		XboxSkyClipPolygon(bounds, count, points, stage + 1);
		return;
	}

	sides[i] = sides[0];
	dists[i] = dists[0];
	VectorCopy(points[0], points[i]);
	newCount[0] = newCount[1] = 0;
	for (i = 0; i < count; ++i) {
		float d;

		/* VectorCopy is a macro that evaluates its destination three times. */
		if (sides[i] != XBOX_SKY_BACK) {
			VectorCopy(points[i], newPoints[0][newCount[0]]);
			newCount[0]++;
		}
		if (sides[i] != XBOX_SKY_FRONT) {
			VectorCopy(points[i], newPoints[1][newCount[1]]);
			newCount[1]++;
		}
		if (sides[i] == XBOX_SKY_ON || sides[i + 1] == XBOX_SKY_ON || sides[i + 1] == sides[i])
			continue;
		d = dists[i] / (dists[i] - dists[i + 1]);
		for (j = 0; j < 3; ++j) {
			float e = points[i][j] + d * (points[i + 1][j] - points[i][j]);

			newPoints[0][newCount[0]][j] = e;
			newPoints[1][newCount[1]][j] = e;
		}
		newCount[0]++;
		newCount[1]++;
	}
	XboxSkyClipPolygon(bounds, newCount[0], newPoints[0], stage + 1);
	XboxSkyClipPolygon(bounds, newCount[1], newPoints[1], stage + 1);
}

/* ioq3 RB_ClipSkyPolygons for one sky surface. */
void XboxNV2ASky_AddSurface(xboxNV2ASkyBounds_t *bounds,
	const xboxNV2AWorldSurface_t *surface, const vec3_t origin)
{
	vec3_t points[5];
	int i, j;

	for (i = 0; i + 2 < surface->numIndexes; i += 3) {
		for (j = 0; j < 3; ++j)
			VectorSubtract(surface->verts[surface->indexes[i + j]].xyz, origin, points[j]);
		XboxSkyClipPolygon(bounds, 3, points, 0);
	}
}

/* ioq3 MakeSkyVec: s and t run from -1 to 1 across the side; st is clamped to [0, 1]. */
static void XboxSkyMakeVec(float s, float t, int axis, float boxSize, float *st, vec3_t xyz)
{
	/* 1 = s, 2 = t, 3 = boxSize; a negative entry negates that component. */
	static const int stToVec[6][3] = {
		{3, -1, 2}, {-3, 1, 2}, {1, 3, 2}, {-1, -3, 2}, {-2, -1, 3}, {2, -1, -3}
	};
	vec3_t b;
	int j;

	b[0] = s * boxSize;
	b[1] = t * boxSize;
	b[2] = boxSize;
	for (j = 0; j < 3; ++j) {
		int k = stToVec[axis][j];

		xyz[j] = k < 0 ? -b[-k - 1] : b[k - 1];
	}
	if (st) {
		st[0] = Com_Clamp(0.0f, 1.0f, (s + 1.0f) * 0.5f);
		st[1] = 1.0f - Com_Clamp(0.0f, 1.0f, (t + 1.0f) * 0.5f);
	}
}

/* ioq3 R_InitSkyTexCoords, with p's squared terms collected and evaluated in double. */
static void XboxSkyInitCloudTexCoords(float cloudHeight)
{
	const double radius = XBOX_SKY_RADIUS_WORLD;
	const double height = cloudHeight;
	int i, s, t;

	for (i = 0; i < XBOX_NV2A_SKY_SIDES; ++i) {
		for (t = 0; t <= XBOX_NV2A_SKY_SUBDIVISIONS; ++t) {
			for (s = 0; s <= XBOX_NV2A_SKY_SUBDIVISIONS; ++s) {
				vec3_t skyVec, v;
				double d, p;

				XboxSkyMakeVec((s - XBOX_SKY_HALF) / (float)XBOX_SKY_HALF,
					(t - XBOX_SKY_HALF) / (float)XBOX_SKY_HALF, i, XBOX_SKY_TABLE_BOX_SIZE,
					NULL, skyVec);
				/* p puts p * skyVec on the cloud sphere, radius + height from its centre. */
				d = DotProduct(skyVec, skyVec);
				p = (-skyVec[2] * radius + sqrt(skyVec[2] * skyVec[2] * radius * radius +
					d * height * (2.0 * radius + height))) / d;
				VectorScale(skyVec, (float)p, v);
				v[2] += (float)radius;
				VectorNormalize(v);
				xboxSkyCloudTexCoords[i][t][s][0] = acosf(Com_Clamp(-1.0f, 1.0f, v[0]));
				xboxSkyCloudTexCoords[i][t][s][1] = acosf(Com_Clamp(-1.0f, 1.0f, v[1]));
			}
		}
	}
	xboxSkyCloudHeight = cloudHeight;
}

/* ioq3 DrawSkyBox/FillCloudBox: the covered part of a side, in whole subdivisions. */
static qboolean XboxSkySideRange(const xboxNV2ASkyBounds_t *bounds, int side, int mins[2],
	int maxs[2])
{
	int k;

	for (k = 0; k < 2; ++k) {
		float low = floorf(bounds->mins[k][side] * XBOX_SKY_HALF) / XBOX_SKY_HALF;
		float high = ceilf(bounds->maxs[k][side] * XBOX_SKY_HALF) / XBOX_SKY_HALF;

		if (low >= high)
			return qfalse;
		mins[k] = (int)(low * XBOX_SKY_HALF);
		maxs[k] = (int)(high * XBOX_SKY_HALF);
		if (mins[k] < -XBOX_SKY_HALF)
			mins[k] = -XBOX_SKY_HALF;
		else if (mins[k] > XBOX_SKY_HALF)
			mins[k] = XBOX_SKY_HALF;
		if (maxs[k] < -XBOX_SKY_HALF)
			maxs[k] = -XBOX_SKY_HALF;
		else if (maxs[k] > XBOX_SKY_HALF)
			maxs[k] = XBOX_SKY_HALF;
	}
	return qtrue;
}

/* Appends one side's grid, two triangles per cell in ioq3 FillCloudySkySide order. */
static int XboxSkyAddGrid(const int mins[2], const int maxs[2], int side, float boxSize,
	qboolean clouds, int firstVert, float (*xyz)[3], float (*st)[2],
	unsigned short *indexes, int *numIndexes)
{
	int width = maxs[0] - mins[0] + 1;
	int height = maxs[1] - mins[1] + 1;
	int v = firstVert;
	int s, t;

	if (width < 2 || height < 2)
		return 0;
	for (t = mins[1]; t <= maxs[1]; ++t) {
		for (s = mins[0]; s <= maxs[0]; ++s, ++v) {
			XboxSkyMakeVec(s / (float)XBOX_SKY_HALF, t / (float)XBOX_SKY_HALF, side, boxSize,
				clouds ? NULL : st[v], xyz[v]);
			if (clouds) {
				st[v][0] = xboxSkyCloudTexCoords[side][t + XBOX_SKY_HALF][s + XBOX_SKY_HALF][0];
				st[v][1] = xboxSkyCloudTexCoords[side][t + XBOX_SKY_HALF][s + XBOX_SKY_HALF][1];
			}
		}
	}
	for (t = 0; t < height - 1; ++t) {
		for (s = 0; s < width - 1; ++s) {
			int corner = firstVert + t * width + s;

			indexes[(*numIndexes)++] = (unsigned short)corner;
			indexes[(*numIndexes)++] = (unsigned short)(corner + width);
			indexes[(*numIndexes)++] = (unsigned short)(corner + 1);
			indexes[(*numIndexes)++] = (unsigned short)(corner + width);
			indexes[(*numIndexes)++] = (unsigned short)(corner + width + 1);
			indexes[(*numIndexes)++] = (unsigned short)(corner + 1);
		}
	}
	return width * height;
}

int XboxNV2ASky_BoxSide(const xboxNV2ASkyBounds_t *bounds, int side, float boxSize,
	float (*xyz)[3], float (*st)[2], unsigned short *indexes, int *numIndexes)
{
	int mins[2], maxs[2];

	*numIndexes = 0;
	if (!XboxSkySideRange(bounds, side, mins, maxs))
		return 0;
	return XboxSkyAddGrid(mins, maxs, side, boxSize, qfalse, 0, xyz, st, indexes,
		numIndexes);
}

/* ioq3 R_BuildCloudData: every side except the bottom, one vertex set for all stages. */
int XboxNV2ASky_Clouds(const xboxNV2ASkyBounds_t *bounds, float cloudHeight,
	float boxSize, float (*xyz)[3], float (*st)[2], unsigned short *indexes,
	int *numIndexes)
{
	int mins[2], maxs[2];
	int numVerts = 0;
	int side;

	*numIndexes = 0;
	if (cloudHeight <= 0.0f)
		return 0;
	if (cloudHeight != xboxSkyCloudHeight)
		XboxSkyInitCloudTexCoords(cloudHeight);
	for (side = 0; side < XBOX_NV2A_SKY_SIDES - 1; ++side) {
		if (XboxSkySideRange(bounds, side, mins, maxs))
			numVerts += XboxSkyAddGrid(mins, maxs, side, boxSize, qtrue, numVerts, xyz, st,
				indexes, numIndexes);
	}
	return numVerts;
}
