/* Q3 shader scripts and TGA/JPG images for the NV2A 2D path. */
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
static int XboxShaderLoadImage(const char *name)
{
	char base[MAX_QPATH];
	const char *extension = COM_GetExtension(name);
	byte *pic = NULL;
	byte *scaled = NULL;
	int width = 0, height = 0;
	int scaledWidth, scaledHeight;
	int image;
	int x, y;

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
	image = XboxNV2A_CreateImage(base, scaledWidth, scaledHeight,
		scaled ? scaled : pic);
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

/* Reads one stage up to its closing brace; qtrue if it names a loadable image. */
static qboolean XboxShaderParseStage(char **text, xboxNV2AShaderState_t *state)
{
	char mapName[MAX_QPATH] = "";
	xboxNV2AShaderState_t stage;
	char *token;

	memset(&stage, 0, sizeof(stage));
	stage.srcBlend = GL_ONE;
	stage.dstBlend = GL_ZERO;
	stage.alphaFunc = XBOX_NV2A_ALPHA_NONE;
	for (;;) {
		token = COM_ParseExt(text, qtrue);
		if (!token[0])
			return qfalse;
		if (!strcmp(token, "}"))
			break;
		if (!Q_stricmp(token, "map") || !Q_stricmp(token, "clampmap")) {
			stage.clamp = !Q_stricmp(token, "clampmap");
			token = COM_ParseExt(text, qfalse);
			/* $lightmap needs world data; the 2D path cannot use it. */
			if (Q_stricmp(token, "$lightmap"))
				Q_strncpyz(mapName, token, sizeof(mapName));
		} else if (!Q_stricmp(token, "animMap")) {
			COM_ParseExt(text, qfalse);
			token = COM_ParseExt(text, qfalse);
			Q_strncpyz(mapName, token, sizeof(mapName));
			SkipRestOfLine(text);
		} else if (!Q_stricmp(token, "blendFunc")) {
			token = COM_ParseExt(text, qfalse);
			if (!Q_stricmp(token, "add")) {
				stage.srcBlend = GL_ONE;
				stage.dstBlend = GL_ONE;
			} else if (!Q_stricmp(token, "filter")) {
				stage.srcBlend = GL_DST_COLOR;
				stage.dstBlend = GL_ZERO;
			} else if (!Q_stricmp(token, "blend")) {
				stage.srcBlend = GL_SRC_ALPHA;
				stage.dstBlend = GL_ONE_MINUS_SRC_ALPHA;
			} else {
				stage.srcBlend = XboxShaderBlendFactor(token);
				token = COM_ParseExt(text, qfalse);
				stage.dstBlend = XboxShaderBlendFactor(token);
			}
		} else if (!Q_stricmp(token, "alphaFunc")) {
			token = COM_ParseExt(text, qfalse);
			if (!Q_stricmp(token, "GT0"))
				stage.alphaFunc = XBOX_NV2A_ALPHA_GT0;
			else if (!Q_stricmp(token, "LT128"))
				stage.alphaFunc = XBOX_NV2A_ALPHA_LT128;
			else if (!Q_stricmp(token, "GE128"))
				stage.alphaFunc = XBOX_NV2A_ALPHA_GE128;
		} else if (!Q_stricmp(token, "rgbGen")) {
			token = COM_ParseExt(text, qfalse);
			stage.vertexColor = !Q_stricmp(token, "vertex") ||
				!Q_stricmp(token, "exactVertex");
			SkipRestOfLine(text);
		} else if (!Q_stricmp(token, "alphaGen")) {
			token = COM_ParseExt(text, qfalse);
			stage.vertexAlpha = !Q_stricmp(token, "vertex");
			SkipRestOfLine(text);
		} else {
			SkipRestOfLine(text);
		}
	}
	if (!mapName[0])
		return qfalse;
	stage.image = XboxShaderLoadImage(mapName);
	if (!stage.image)
		return qfalse;
	*state = stage;
	return qtrue;
}

/* The 2D path draws one layer: the first stage whose image loads. */
static void XboxShaderParse(char *body, xboxNV2AShaderState_t *state)
{
	char *p = body;
	qboolean haveStage = qfalse;
	char *token;

	for (;;) {
		token = COM_ParseExt(&p, qtrue);
		if (!token[0] || !strcmp(token, "}"))
			break;
		if (!strcmp(token, "{")) {
			if (haveStage) {
				if (!SkipBracedSection(&p, 1))
					break;
			} else {
				haveStage = XboxShaderParseStage(&p, state);
			}
			continue;
		}
		SkipRestOfLine(&p);
	}
}

qhandle_t XboxNV2AShader_Register(const char *name, qboolean mipRawImage)
{
	char stripped[MAX_QPATH];
	xboxNV2AShaderState_t state;
	xboxShaderText_t *script;
	qhandle_t handle;

	if (!name || !*name)
		return 0;
	if (!Q_stricmp(name, "white"))
		return XBOX_NV2A_WHITE_SHADER;
	COM_StripExtension(name, stripped, sizeof(stripped));
	if (XboxNV2A_FindShader(stripped, &handle))
		return handle;
	if (!xboxShaderScriptsLoaded)
		XboxShaderLoadScripts();

	memset(&state, 0, sizeof(state));
	script = XboxShaderFindText(stripped);
	if (script) {
		XboxShaderParse(script->body, &state);
	} else {
		/* Implicit 2D shader, as in ioq3 R_FindShader for LIGHTMAP_2D. */
		state.image = XboxShaderLoadImage(name);
		state.srcBlend = GL_SRC_ALPHA;
		state.dstBlend = GL_ONE_MINUS_SRC_ALPHA;
		state.alphaFunc = XBOX_NV2A_ALPHA_NONE;
		state.clamp = !mipRawImage;
		state.vertexColor = qtrue;
		state.vertexAlpha = qtrue;
	}
	if (!state.image)
		Sys_XboxLog("Xbox shaders: %s unresolved%s\n", stripped,
			script ? " (script has no loadable 2D stage)" : "");
	return XboxNV2A_CreateShader(stripped, &state);
}
