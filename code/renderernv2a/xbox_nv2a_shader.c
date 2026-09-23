/* Q3 shader scripts and TGA/JPG images for the NV2A renderer. */
#include "xbox_nv2a.h"
#include "../renderercommon/tr_common.h"
#include "../sys/sys_xbox.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define XBOX_SHADER_HASH_SIZE 1024

typedef struct xboxShaderText_s {
	char *name;
	char *body;
	struct xboxShaderText_s *next;
} xboxShaderText_t;

static char *xboxShaderText;
static char *xboxShaderNames;
static xboxShaderText_t *xboxShaderEntries;
static xboxShaderText_t *xboxShaderHash[XBOX_SHADER_HASH_SIZE];
static qboolean xboxShaderScriptsLoaded;

static unsigned int XboxShaderHash(const char *name)
{
	unsigned int hash = 0;
	int i;

	for (i = 0; name[i]; ++i) {
		char letter = (char)tolower((unsigned char)name[i]);

		if (letter == '\\')
			letter = '/';
		hash += (unsigned int)(unsigned char)letter * (unsigned int)(i + 119);
	}
	return (hash ^ (hash >> 10) ^ (hash >> 20)) & (XBOX_SHADER_HASH_SIZE - 1);
}

static xboxShaderText_t *XboxShaderFindText(const char *name)
{
	xboxShaderText_t *entry;

	for (entry = xboxShaderHash[XboxShaderHash(name)]; entry; entry = entry->next) {
		if (!Q_stricmp(entry->name, name))
			return entry;
	}
	return NULL;
}

void XboxNV2AShader_FreeScripts(void)
{
	free(xboxShaderText);
	free(xboxShaderNames);
	free(xboxShaderEntries);
	xboxShaderText = NULL;
	xboxShaderNames = NULL;
	xboxShaderEntries = NULL;
	memset(xboxShaderHash, 0, sizeof(xboxShaderHash));
	xboxShaderScriptsLoaded = qfalse;
}

/* Returns the entry count; with entries set it also fills names and the hash. */
static int XboxShaderIndex(char *text, char *names, xboxShaderText_t *entries,
	size_t *nameBytes)
{
	char *p = text;
	char name[MAX_QPATH];
	int count = 0;
	char *token;

	*nameBytes = 0;
	for (;;) {
		token = COM_ParseExt(&p, qtrue);
		if (!token[0])
			break;
		COM_StripExtension(token, name, sizeof(name));
		token = COM_ParseExt(&p, qtrue);
		if (token[0] != '{' || token[1]) {
			ri.Printf(PRINT_WARNING, "Xbox shaders: expected '{' after %s\n", name);
			break;
		}
		if (entries && !XboxShaderFindText(name)) {
			unsigned int hash = XboxShaderHash(name);

			entries[count].name = names + *nameBytes;
			strcpy(entries[count].name, name);
			entries[count].body = p;
			entries[count].next = xboxShaderHash[hash];
			xboxShaderHash[hash] = &entries[count];
		}
		*nameBytes += strlen(name) + 1;
		count++;
		if (!SkipBracedSection(&p, 1)) {
			ri.Printf(PRINT_WARNING, "Xbox shaders: unbalanced braces in %s\n", name);
			break;
		}
	}
	return count;
}

/* ioq3 order: later script files are scanned first, so their definitions win. */
static void XboxShaderLoadScripts(void)
{
	char **files;
	int numFiles;
	int i;
	long total = 0;
	size_t offset = 0;
	size_t nameBytes;
	int count;

	xboxShaderScriptsLoaded = qtrue;
#ifdef XBOX_DIAG_NO_SCRIPTS
	/* DIAGNOSTIC bisection switch: implicit shaders only. */
	return;
#endif
	files = ri.FS_ListFiles("scripts", ".shader", &numFiles);
	if (!files || numFiles <= 0) {
		if (files)
			ri.FS_FreeFileList(files);
		return;
	}
	for (i = 0; i < numFiles; ++i) {
		char path[MAX_QPATH];
		long length;

		Com_sprintf(path, sizeof(path), "scripts/%s", files[i]);
		length = ri.FS_ReadFile(path, NULL);
		if (length > 0)
			total += length + 1;
	}
	xboxShaderText = (char *)malloc((size_t)total + 1);
	if (!xboxShaderText) {
		ri.Printf(PRINT_WARNING, "Xbox shaders: no memory for %ld bytes\n", total);
		ri.FS_FreeFileList(files);
		return;
	}
	for (i = numFiles - 1; i >= 0; --i) {
		char path[MAX_QPATH];
		void *buffer;
		long length;

		Com_sprintf(path, sizeof(path), "scripts/%s", files[i]);
		length = ri.FS_ReadFile(path, &buffer);
		if (length <= 0 || !buffer || offset + (size_t)length + 1 > (size_t)total)
			continue;
		memcpy(xboxShaderText + offset, buffer, (size_t)length);
		xboxShaderText[offset + length] = '\0';
		ri.FS_FreeFile(buffer);
		offset += (size_t)COM_Compress(xboxShaderText + offset);
		xboxShaderText[offset++] = '\n';
	}
	xboxShaderText[offset] = '\0';
	ri.FS_FreeFileList(files);

	/* COM_ParseExt only reads the text, so a counting pass can size the index. */
	count = XboxShaderIndex(xboxShaderText, NULL, NULL, &nameBytes);
	xboxShaderNames = (char *)malloc(nameBytes ? nameBytes : 1);
	xboxShaderEntries = (xboxShaderText_t *)calloc((size_t)(count ? count : 1),
		sizeof(*xboxShaderEntries));
	if (!xboxShaderNames || !xboxShaderEntries) {
		ri.Printf(PRINT_WARNING, "Xbox shaders: no memory for the index\n");
		XboxNV2AShader_FreeScripts();
		xboxShaderScriptsLoaded = qtrue;
		return;
	}
	XboxShaderIndex(xboxShaderText, xboxShaderNames, xboxShaderEntries, &nameBytes);
	Sys_XboxLog("Xbox shaders: %d definitions from %d files, %u KiB\n",
		count, numFiles, (unsigned int)((offset + nameBytes +
		(size_t)count * sizeof(*xboxShaderEntries)) / 1024));
}

static int XboxShaderRoundDownPowerOfTwo(int value)
{
	int result = 1;

	while (result * 2 <= value)
		result *= 2;
	if (result > XBOX_NV2A_MAX_TEXTURE_SIZE)
		result = XBOX_NV2A_MAX_TEXTURE_SIZE;
	return result;
}

/* ioq3 R_MipMap box filter, used for picmip; a 1-texel side stays 1 texel. */
static void XboxShaderHalveImage(byte *pic, int *width, int *height)
{
	int outWidth = *width > 1 ? *width / 2 : 1;
	int outHeight = *height > 1 ? *height / 2 : 1;
	int stepX = *width > 1 ? 1 : 0;
	int stepY = *height > 1 ? 1 : 0;
	int x, y, k;

	for (y = 0; y < outHeight; ++y) {
		const byte *row0 = pic + (size_t)(y * (stepY + 1)) * *width * 4;
		const byte *row1 = row0 + (size_t)stepY * *width * 4;

		for (x = 0; x < outWidth; ++x) {
			int x0 = x * (stepX + 1) * 4;
			int x1 = x0 + stepX * 4;
			byte *out = pic + ((size_t)y * outWidth + x) * 4;

			for (k = 0; k < 4; ++k)
				out[k] = (byte)((row0[x0 + k] + row0[x1 + k] + row1[x0 + k] +
					row1[x1 + k] + 2) >> 2);
		}
	}
	*width = outWidth;
	*height = outHeight;
}

static void XboxShaderReadImage(const char *base, const char *extension,
	byte **pic, int *width, int *height)
{
	char path[MAX_QPATH];

	Com_sprintf(path, sizeof(path), "%s.%s", base, extension);
	/* DIAGNOSTIC trace for the post-ui.qvm crash; remove once it is found. */
	Sys_XboxLog("Xbox image: decode %s\n", path);
#ifdef XBOX_DIAG_NO_JPG
	/* DIAGNOSTIC bisection switch: never run libjpeg. */
	if (!Q_stricmp(extension, "jpg")) {
		*pic = NULL;
		return;
	}
#endif
	if (!Q_stricmp(extension, "jpg"))
		R_LoadJPG(path, pic, width, height);
	else
		R_LoadTGA(path, pic, width, height);
	Sys_XboxLog("Xbox image: %s -> %s %dx%d\n", path, *pic ? "ok" : "missing",
		*width, *height);
}

/* ioq3 rounds non-power-of-two images down (r_roundImagesDown); NV2A swizzle needs it. */
static int XboxShaderLoadImage(const char *name, qboolean picmip)
{
	char base[MAX_QPATH];
	const char *extension = COM_GetExtension(name);
	byte *pic = NULL;
	byte *scaled = NULL;
	int width = 0, height = 0;
	int scaledWidth, scaledHeight;
	int image;
	int x, y;
	int i;

	if (!Q_stricmp(name, "*white") || !Q_stricmp(name, "$whiteimage"))
		return XBOX_NV2A_WHITE_IMAGE;
	COM_StripExtension(name, base, sizeof(base));
	image = XboxNV2A_FindImage(base);
	if (image)
		return image;

	if (!Q_stricmp(extension, "jpg")) {
		XboxShaderReadImage(base, "jpg", &pic, &width, &height);
		if (!pic)
			XboxShaderReadImage(base, "tga", &pic, &width, &height);
	} else {
		XboxShaderReadImage(base, "tga", &pic, &width, &height);
		if (!pic)
			XboxShaderReadImage(base, "jpg", &pic, &width, &height);
	}
	if (!pic || width <= 0 || height <= 0) {
		if (pic)
			ri.Free(pic);
		return 0;
	}

	scaledWidth = XboxShaderRoundDownPowerOfTwo(width);
	scaledHeight = XboxShaderRoundDownPowerOfTwo(height);
	if (scaledWidth != width || scaledHeight != height) {
		scaled = (byte *)malloc((size_t)scaledWidth * scaledHeight * 4);
		if (!scaled) {
			ri.Free(pic);
			return 0;
		}
		for (y = 0; y < scaledHeight; ++y) {
			int sourceY = y * height / scaledHeight;

			for (x = 0; x < scaledWidth; ++x) {
				int sourceX = x * width / scaledWidth;

				memcpy(scaled + ((size_t)y * scaledWidth + x) * 4,
					pic + ((size_t)sourceY * width + sourceX) * 4, 4);
			}
		}
	}
	for (i = 0; picmip && i < XBOX_NV2A_PICMIP; ++i)
		XboxShaderHalveImage(scaled ? scaled : pic, &scaledWidth, &scaledHeight);
	image = XboxNV2A_CreateImage(base, scaledWidth, scaledHeight,
		scaled ? scaled : pic, qfalse);
	free(scaled);
	ri.Free(pic);
	return image;
}

static unsigned int XboxShaderBlendFactor(const char *name)
{
	static const struct { const char *name; unsigned int value; } factors[] = {
		{ "GL_ZERO", GL_ZERO }, { "GL_ONE", GL_ONE },
		{ "GL_SRC_COLOR", GL_SRC_COLOR }, { "GL_ONE_MINUS_SRC_COLOR", GL_ONE_MINUS_SRC_COLOR },
		{ "GL_SRC_ALPHA", GL_SRC_ALPHA }, { "GL_ONE_MINUS_SRC_ALPHA", GL_ONE_MINUS_SRC_ALPHA },
		{ "GL_DST_ALPHA", GL_DST_ALPHA }, { "GL_ONE_MINUS_DST_ALPHA", GL_ONE_MINUS_DST_ALPHA },
		{ "GL_DST_COLOR", GL_DST_COLOR }, { "GL_ONE_MINUS_DST_COLOR", GL_ONE_MINUS_DST_COLOR },
		{ "GL_SRC_ALPHA_SATURATE", GL_SRC_ALPHA_SATURATE },
	};
	size_t i;

	for (i = 0; i < ARRAY_LEN(factors); ++i) {
		if (!Q_stricmp(name, factors[i].name))
			return factors[i].value;
	}
	return GL_ONE;
}

static int XboxShaderWaveFunc(const char *name)
{
	if (!Q_stricmp(name, "square"))
		return XBOX_NV2A_WAVE_SQUARE;
	if (!Q_stricmp(name, "triangle"))
		return XBOX_NV2A_WAVE_TRIANGLE;
	if (!Q_stricmp(name, "sawtooth"))
		return XBOX_NV2A_WAVE_SAWTOOTH;
	if (!Q_stricmp(name, "inversesawtooth"))
		return XBOX_NV2A_WAVE_INVERSE_SAWTOOTH;
	if (!Q_stricmp(name, "noise"))
		return XBOX_NV2A_WAVE_NOISE;
	return XBOX_NV2A_WAVE_SIN;
}

/* Reads "func base amplitude phase frequency" from the current line. */
static void XboxShaderParseWave(char **text, xboxNV2AWave_t *wave)
{
	wave->func = XboxShaderWaveFunc(COM_ParseExt(text, qfalse));
	wave->base = atof(COM_ParseExt(text, qfalse));
	wave->amplitude = atof(COM_ParseExt(text, qfalse));
	wave->phase = atof(COM_ParseExt(text, qfalse));
	wave->frequency = atof(COM_ParseExt(text, qfalse));
}

/* Reads "( a b c )" as in ioq3 Parse1DMatrix. */
static void XboxShaderParseVector(char **text, int count, float *out)
{
	int i;

	if (strcmp(COM_ParseExt(text, qfalse), "("))
		return;
	for (i = 0; i < count; ++i)
		out[i] = atof(COM_ParseExt(text, qfalse));
	COM_ParseExt(text, qfalse);
}

static void XboxShaderParseTexMod(char **text, xboxNV2AStage_t *stage)
{
	xboxNV2ATexMod_t *mod;
	char *token;

	if (stage->numTexMods == XBOX_NV2A_MAX_TEXMODS)
		return;
	mod = &stage->texMods[stage->numTexMods];
	memset(mod, 0, sizeof(*mod));
	token = COM_ParseExt(text, qfalse);
	if (!Q_stricmp(token, "turb")) {
		mod->type = XBOX_NV2A_TCMOD_TURB;
		mod->wave.base = atof(COM_ParseExt(text, qfalse));
		mod->wave.amplitude = atof(COM_ParseExt(text, qfalse));
		mod->wave.phase = atof(COM_ParseExt(text, qfalse));
		mod->wave.frequency = atof(COM_ParseExt(text, qfalse));
	} else if (!Q_stricmp(token, "scale")) {
		mod->type = XBOX_NV2A_TCMOD_SCALE;
		mod->scale[0] = atof(COM_ParseExt(text, qfalse));
		mod->scale[1] = atof(COM_ParseExt(text, qfalse));
	} else if (!Q_stricmp(token, "scroll")) {
		mod->type = XBOX_NV2A_TCMOD_SCROLL;
		mod->scroll[0] = atof(COM_ParseExt(text, qfalse));
		mod->scroll[1] = atof(COM_ParseExt(text, qfalse));
	} else if (!Q_stricmp(token, "stretch")) {
		mod->type = XBOX_NV2A_TCMOD_STRETCH;
		XboxShaderParseWave(text, &mod->wave);
	} else if (!Q_stricmp(token, "transform")) {
		mod->type = XBOX_NV2A_TCMOD_TRANSFORM;
		mod->matrix[0][0] = atof(COM_ParseExt(text, qfalse));
		mod->matrix[0][1] = atof(COM_ParseExt(text, qfalse));
		mod->matrix[1][0] = atof(COM_ParseExt(text, qfalse));
		mod->matrix[1][1] = atof(COM_ParseExt(text, qfalse));
		mod->translate[0] = atof(COM_ParseExt(text, qfalse));
		mod->translate[1] = atof(COM_ParseExt(text, qfalse));
	} else if (!Q_stricmp(token, "rotate")) {
		mod->type = XBOX_NV2A_TCMOD_ROTATE;
		mod->rotateSpeed = atof(COM_ParseExt(text, qfalse));
	} else if (!Q_stricmp(token, "entityTranslate")) {
		mod->type = XBOX_NV2A_TCMOD_ENTITY_TRANSLATE;
	} else {
		if (token[0])
			SkipRestOfLine(text);
		return;
	}
	stage->numTexMods++;
}

static void XboxShaderParseRgbGen(char **text, xboxNV2AStage_t *stage)
{
	char *token = COM_ParseExt(text, qfalse);

	if (!Q_stricmp(token, "wave")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_WAVE;
		XboxShaderParseWave(text, &stage->rgbWave);
	} else if (!Q_stricmp(token, "const")) {
		float color[3] = {0.0f, 0.0f, 0.0f};

		stage->rgbGen = XBOX_NV2A_RGBGEN_CONST;
		XboxShaderParseVector(text, 3, color);
		stage->constant[0] = (byte)(255 * Com_Clamp(0.0f, 1.0f, color[0]));
		stage->constant[1] = (byte)(255 * Com_Clamp(0.0f, 1.0f, color[1]));
		stage->constant[2] = (byte)(255 * Com_Clamp(0.0f, 1.0f, color[2]));
	} else if (!Q_stricmp(token, "identity")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_IDENTITY;
	} else if (!Q_stricmp(token, "identityLighting")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_IDENTITY_LIGHTING;
	} else if (!Q_stricmp(token, "entity")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_ENTITY;
	} else if (!Q_stricmp(token, "oneMinusEntity")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_ONE_MINUS_ENTITY;
	} else if (!Q_stricmp(token, "vertex")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_VERTEX;
	} else if (!Q_stricmp(token, "exactVertex")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_EXACT_VERTEX;
	} else if (!Q_stricmp(token, "lightingDiffuse")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_LIGHTING_DIFFUSE;
	} else if (!Q_stricmp(token, "oneMinusVertex")) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_ONE_MINUS_VERTEX;
	}
}

/* lightingSpecular and portal need world data, so they fall back to identity. */
static void XboxShaderParseAlphaGen(char **text, xboxNV2AStage_t *stage)
{
	char *token = COM_ParseExt(text, qfalse);

	if (!Q_stricmp(token, "wave")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_WAVE;
		XboxShaderParseWave(text, &stage->alphaWave);
	} else if (!Q_stricmp(token, "const")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_CONST;
		stage->constant[3] = (byte)(255 * Com_Clamp(0.0f, 1.0f,
			atof(COM_ParseExt(text, qfalse))));
	} else if (!Q_stricmp(token, "entity")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_ENTITY;
	} else if (!Q_stricmp(token, "oneMinusEntity")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_ONE_MINUS_ENTITY;
	} else if (!Q_stricmp(token, "vertex")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_VERTEX;
	} else if (!Q_stricmp(token, "oneMinusVertex")) {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_ONE_MINUS_VERTEX;
	} else {
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_IDENTITY;
	}
}

/* Reads one stage up to its closing brace; qfalse if the stage cannot be drawn. */
static qboolean XboxShaderParseStage(char **text, xboxNV2AStage_t *stage,
	qboolean picmip)
{
	char maps[XBOX_NV2A_MAX_ANIM_IMAGES][MAX_QPATH];
	int numMaps = 0;
	qboolean drawable = qtrue;
	qboolean depthWriteSet = qfalse;
	char *token;
	int i;

	memset(stage, 0, sizeof(*stage));
	stage->srcBlend = GL_ONE;
	stage->dstBlend = GL_ZERO;
	stage->depthWrite = qtrue;
	stage->alphaFunc = XBOX_NV2A_ALPHA_NONE;
	for (;;) {
		token = COM_ParseExt(text, qtrue);
		if (!token[0])
			return qfalse;
		if (!strcmp(token, "}"))
			break;
		if (!Q_stricmp(token, "map") || !Q_stricmp(token, "clampmap")) {
			stage->clamp = !Q_stricmp(token, "clampmap");
			token = COM_ParseExt(text, qfalse);
			/* ioq3 ParseStage: $lightmap samples the surface lightmap with tcGen lightmap. */
			if (!Q_stricmp(token, "$lightmap")) {
				stage->isLightmap = qtrue;
				stage->tcGen = XBOX_NV2A_TCGEN_LIGHTMAP;
				numMaps = 0;
			} else {
				Q_strncpyz(maps[0], token, sizeof(maps[0]));
				numMaps = 1;
			}
		} else if (!Q_stricmp(token, "animMap")) {
			stage->animFrequency = atof(COM_ParseExt(text, qfalse));
			numMaps = 0;
			for (token = COM_ParseExt(text, qfalse); token[0];
				token = COM_ParseExt(text, qfalse)) {
				if (numMaps < XBOX_NV2A_MAX_ANIM_IMAGES)
					Q_strncpyz(maps[numMaps++], token, sizeof(maps[0]));
			}
		} else if (!Q_stricmp(token, "videoMap")) {
			drawable = qfalse;
			COM_ParseExt(text, qfalse);
		} else if (!Q_stricmp(token, "blendFunc")) {
			token = COM_ParseExt(text, qfalse);
			if (!Q_stricmp(token, "add")) {
				stage->srcBlend = GL_ONE;
				stage->dstBlend = GL_ONE;
			} else if (!Q_stricmp(token, "filter")) {
				stage->srcBlend = GL_DST_COLOR;
				stage->dstBlend = GL_ZERO;
			} else if (!Q_stricmp(token, "blend")) {
				stage->srcBlend = GL_SRC_ALPHA;
				stage->dstBlend = GL_ONE_MINUS_SRC_ALPHA;
			} else {
				stage->srcBlend = XboxShaderBlendFactor(token);
				stage->dstBlend = XboxShaderBlendFactor(COM_ParseExt(text, qfalse));
			}
			/* ioq3 clears the depth mask for blended stages. */
			if (!depthWriteSet)
				stage->depthWrite = qfalse;
		} else if (!Q_stricmp(token, "alphaFunc")) {
			token = COM_ParseExt(text, qfalse);
			if (!Q_stricmp(token, "GT0"))
				stage->alphaFunc = XBOX_NV2A_ALPHA_GT0;
			else if (!Q_stricmp(token, "LT128"))
				stage->alphaFunc = XBOX_NV2A_ALPHA_LT128;
			else if (!Q_stricmp(token, "GE128"))
				stage->alphaFunc = XBOX_NV2A_ALPHA_GE128;
		} else if (!Q_stricmp(token, "depthFunc")) {
			stage->depthEqual = !Q_stricmp(COM_ParseExt(text, qfalse), "equal");
		} else if (!Q_stricmp(token, "depthWrite")) {
			stage->depthWrite = qtrue;
			depthWriteSet = qtrue;
		} else if (!Q_stricmp(token, "rgbGen")) {
			XboxShaderParseRgbGen(text, stage);
		} else if (!Q_stricmp(token, "alphaGen")) {
			XboxShaderParseAlphaGen(text, stage);
		} else if (!Q_stricmp(token, "tcGen") || !Q_stricmp(token, "texGen")) {
			token = COM_ParseExt(text, qfalse);
			if (!Q_stricmp(token, "environment")) {
				stage->tcGen = XBOX_NV2A_TCGEN_ENVIRONMENT;
			} else if (!Q_stricmp(token, "lightmap")) {
				stage->tcGen = XBOX_NV2A_TCGEN_LIGHTMAP;
			} else if (!Q_stricmp(token, "texture") || !Q_stricmp(token, "base")) {
				stage->tcGen = XBOX_NV2A_TCGEN_TEXTURE;
			} else if (!Q_stricmp(token, "vector")) {
				stage->tcGen = XBOX_NV2A_TCGEN_VECTOR;
				XboxShaderParseVector(text, 3, stage->tcGenVectors[0]);
				XboxShaderParseVector(text, 3, stage->tcGenVectors[1]);
			}
		} else if (!Q_stricmp(token, "tcMod")) {
			XboxShaderParseTexMod(text, stage);
		} else {
			/* Known keywords consume exact tokens; a skip after an empty token would eat a line. */
			SkipRestOfLine(text);
		}
	}
	/* ioq3: GL_ONE GL_ZERO disables blending and writes depth. */
	if (stage->srcBlend == GL_ONE && stage->dstBlend == GL_ZERO)
		stage->depthWrite = qtrue;
	if (!drawable)
		return qfalse;
	if (stage->isLightmap) {
		stage->images[0] = XBOX_NV2A_WHITE_IMAGE;
		stage->numImages = 1;
		return qtrue;
	}
	for (i = 0; i < numMaps; ++i) {
		int image = XboxShaderLoadImage(maps[i], picmip);

		if (image)
			stage->images[stage->numImages++] = image;
	}
	return stage->numImages > 0;
}

static float XboxShaderParseSort(const char *token)
{
	if (!Q_stricmp(token, "portal"))
		return XBOX_NV2A_SORT_PORTAL;
	if (!Q_stricmp(token, "sky"))
		return XBOX_NV2A_SORT_ENVIRONMENT;
	if (!Q_stricmp(token, "opaque"))
		return XBOX_NV2A_SORT_OPAQUE;
	if (!Q_stricmp(token, "decal"))
		return XBOX_NV2A_SORT_DECAL;
	if (!Q_stricmp(token, "seeThrough"))
		return XBOX_NV2A_SORT_SEE_THROUGH;
	if (!Q_stricmp(token, "banner"))
		return XBOX_NV2A_SORT_BANNER;
	if (!Q_stricmp(token, "additive"))
		return XBOX_NV2A_SORT_BLEND1;
	if (!Q_stricmp(token, "nearest"))
		return XBOX_NV2A_SORT_NEAREST;
	if (!Q_stricmp(token, "underwater"))
		return XBOX_NV2A_SORT_UNDERWATER;
	return atof(token);
}

static void XboxShaderParse(char *body, xboxNV2AShaderDef_t *def)
{
	char *p = body;
	qboolean sortSet = qfalse;
	qboolean polygonOffset = qfalse;
	qboolean picmip = qtrue;
	char *token;

	for (;;) {
		token = COM_ParseExt(&p, qtrue);
		if (!token[0] || !strcmp(token, "}"))
			break;
		if (!strcmp(token, "{")) {
			if (def->numStages < XBOX_NV2A_MAX_STAGES) {
				if (XboxShaderParseStage(&p, &def->stages[def->numStages], picmip))
					def->numStages++;
			} else if (!SkipBracedSection(&p, 1)) {
				break;
			}
			continue;
		}
		/* ioq3 ParseShader: nomipmaps also implies nopicmip. */
		if (!Q_stricmp(token, "nopicmip") || !Q_stricmp(token, "nomipmaps")) {
			picmip = qfalse;
			continue;
		}
		if (!Q_stricmp(token, "skyParms")) {
			def->isSky = qtrue;
			SkipRestOfLine(&p);
			continue;
		}
		if (!Q_stricmp(token, "cull")) {
			token = COM_ParseExt(&p, qfalse);
			if (!Q_stricmp(token, "none") || !Q_stricmp(token, "twosided") ||
				!Q_stricmp(token, "disable"))
				def->cull = XBOX_NV2A_CULL_NONE;
			else if (!Q_stricmp(token, "back") || !Q_stricmp(token, "backside") ||
				!Q_stricmp(token, "backsided"))
				def->cull = XBOX_NV2A_CULL_BACK;
			else
				def->cull = XBOX_NV2A_CULL_FRONT;
		} else if (!Q_stricmp(token, "sort")) {
			def->sort = XboxShaderParseSort(COM_ParseExt(&p, qfalse));
			sortSet = qtrue;
		} else if (!Q_stricmp(token, "portal")) {
			def->sort = XBOX_NV2A_SORT_PORTAL;
			sortSet = qtrue;
		} else if (!Q_stricmp(token, "polygonOffset")) {
			polygonOffset = qtrue;
		} else {
			SkipRestOfLine(&p);
		}
	}
	/* ioq3 FinishShader: a blended first stage sorts after opaque surfaces. */
	if (!sortSet) {
		const xboxNV2AStage_t *first = &def->stages[0];

		if (def->numStages && (first->srcBlend != GL_ONE || first->dstBlend != GL_ZERO))
			def->sort = first->depthWrite ? XBOX_NV2A_SORT_SEE_THROUGH :
				XBOX_NV2A_SORT_BLEND0;
		else if (polygonOffset)
			def->sort = XBOX_NV2A_SORT_DECAL;
		else
			def->sort = XBOX_NV2A_SORT_OPAQUE;
	}
}

/* ioq3 R_FindShader defaults for LIGHTMAP_2D, LIGHTMAP_NONE, lightmapped and vertex-lit. */
static void XboxShaderImplicit(const char *name, int flavor,
	qboolean mipRawImage, xboxNV2AShaderDef_t *def)
{
	xboxNV2AStage_t *stage = &def->stages[0];
	int image = XboxShaderLoadImage(name, mipRawImage);

	def->sort = XBOX_NV2A_SORT_OPAQUE;
	if (!image)
		return;
	stage->images[0] = image;
	stage->numImages = 1;
	stage->alphaFunc = XBOX_NV2A_ALPHA_NONE;
	stage->srcBlend = GL_ONE;
	stage->dstBlend = GL_ZERO;
	stage->depthWrite = qtrue;
	def->numStages = 1;
	if (flavor == XBOX_NV2A_SHADER_2D) {
		stage->srcBlend = GL_SRC_ALPHA;
		stage->dstBlend = GL_ONE_MINUS_SRC_ALPHA;
		stage->depthWrite = qfalse;
		stage->clamp = !mipRawImage;
		stage->rgbGen = XBOX_NV2A_RGBGEN_VERTEX;
		stage->alphaGen = XBOX_NV2A_ALPHAGEN_VERTEX;
	} else if (flavor == XBOX_NV2A_SHADER_MODEL) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_LIGHTING_DIFFUSE;
	} else if (flavor == XBOX_NV2A_SHADER_VERTEX) {
		stage->rgbGen = XBOX_NV2A_RGBGEN_EXACT_VERTEX;
	} else {
		/* Two passes: the lightmap, then the texture with a GL_DST_COLOR GL_ZERO filter. */
		stage->isLightmap = qtrue;
		stage->tcGen = XBOX_NV2A_TCGEN_LIGHTMAP;
		stage->images[0] = XBOX_NV2A_WHITE_IMAGE;
		stage->rgbGen = XBOX_NV2A_RGBGEN_IDENTITY;
		stage = &def->stages[1];
		stage->images[0] = image;
		stage->numImages = 1;
		stage->srcBlend = GL_DST_COLOR;
		stage->dstBlend = GL_ZERO;
		stage->rgbGen = XBOX_NV2A_RGBGEN_IDENTITY;
		def->numStages = 2;
	}
}

static qhandle_t XboxShaderRegister(const char *name, int flavor,
	qboolean mipRawImage)
{
	char stripped[MAX_QPATH];
	xboxNV2AShaderDef_t def;
	xboxShaderText_t *script;
	qhandle_t handle;

	if (!name || !*name)
		return 0;
	if (!Q_stricmp(name, "white"))
		return XBOX_NV2A_WHITE_SHADER;
	COM_StripExtension(name, stripped, sizeof(stripped));
	if (XboxNV2A_FindShader(stripped, flavor, &handle))
		return handle;
	if (!xboxShaderScriptsLoaded)
		XboxShaderLoadScripts();

	memset(&def, 0, sizeof(def));
	def.cull = XBOX_NV2A_CULL_FRONT;
	script = XboxShaderFindText(stripped);
	if (script)
		XboxShaderParse(script->body, &def);
	else
		XboxShaderImplicit(name, flavor, mipRawImage, &def);
	/* nxdk's vsnprintf has no float conversions, so the sort prints as an integer. */
	if (!def.numStages)
		Sys_XboxLog("Xbox shaders: %s unresolved%s\n", stripped,
			script ? " (script has no drawable stage)" : "");
	else if (flavor == XBOX_NV2A_SHADER_MODEL)
		Sys_XboxLog("Xbox shaders: model %s stages=%d sort=%d cull=%d\n",
			stripped, def.numStages, (int)def.sort, def.cull);
	return XboxNV2A_CreateShader(stripped, flavor, &def);
}

qhandle_t XboxNV2AShader_Register(const char *name, qboolean mipRawImage)
{
	return XboxShaderRegister(name, XBOX_NV2A_SHADER_2D, mipRawImage);
}

qhandle_t XboxNV2AShader_RegisterModel(const char *name)
{
	return XboxShaderRegister(name, XBOX_NV2A_SHADER_MODEL, qtrue);
}

qhandle_t XboxNV2AShader_RegisterWorld(const char *name, int flavor)
{
	return XboxShaderRegister(name, flavor, qtrue);
}
