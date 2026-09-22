/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <hal/debug.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fileapi.h>

static volatile int xboxExitRequested;
static FILE *xboxLogFile;
static int xboxTimeBase;
static char xboxBinaryPath[MAX_OSPATH];
static char xboxHomePath[MAX_OSPATH];

static void Sys_XboxNormalizePath(char *path)
{
	char *p;

	for (p = path; *p; ++p)
	{
		if (*p == '/')
			*p = '\\';
	}
}

void Sys_XboxLogOpen(void)
{
	if (xboxLogFile)
		return;

	xboxLogFile = fopen("D:\\ioquake3.log", "wb");
	if (!xboxLogFile)
		debugPrint("Xbox log open failed: D:\\ioquake3.log\n");
}

void Sys_XboxLog(const char *format, ...)
{
	char message[512];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	debugPrint("%s", message);
	if (xboxLogFile)
	{
		fputs(message, xboxLogFile);
		fflush(xboxLogFile);
	}
}

void Sys_Print(const char *message)
{
	Sys_XboxLog("%s", message);
}

int Sys_Milliseconds(void)
{
	int now = (int)GetTickCount();

	if (!xboxTimeBase)
		xboxTimeBase = now;

	return now - xboxTimeBase;
}

qboolean Sys_RandomBytes(byte *string, int len)
{
	(void)string;
	(void)len;
	return qfalse;
}

cpuFeatures_t Sys_GetProcessorFeatures(void)
{
	/* The retail CPU has SSE1 but no SSE2. */
	return CF_SSE;
}

void Sys_DisplaySystemConsole(qboolean show)
{
	(void)show;
}

char *Sys_GetCurrentUser(void)
{
	static char user[] = "xbox-player";
	return user;
}

char *Sys_GetClipboardData(void)
{
	return NULL;
}

qboolean Sys_LowPhysicalMemory(void)
{
	/* Retail Xbox hardware has 64 MiB total; reserve the conservative path. */
	return qtrue;
}

void Sys_SetErrorText(const char *text)
{
	if (text)
		Sys_XboxLog("%s\n", text);
}

void Sys_Init(void)
{
	Cvar_Set("arch", "xbox x86");
	Cvar_Set("username", Sys_GetCurrentUser());
	Sys_XboxPlatformInit();
}

void Sys_InitPIDFile(const char *gamedir)
{
	(void)gamedir;
}

void Sys_RemovePIDFile(const char *gamedir)
{
	(void)gamedir;
}

void Sys_Sleep(int milliseconds)
{
	if (milliseconds < 0)
		milliseconds = 1000;
	Sleep((DWORD)milliseconds);
}

FILE *Sys_FOpen(const char *ospath, const char *mode)
{
	char path[MAX_OSPATH];

	if (!ospath || !*ospath || !mode)
		return NULL;

	Q_strncpyz(path, ospath, sizeof(path));
	Sys_XboxNormalizePath(path);
	if (path[strlen(path) - 1] == ' ' || path[strlen(path) - 1] == '.')
		return NULL;

	return fopen(path, mode);
}

qboolean Sys_Mkdir(const char *path)
{
	char normalized[MAX_OSPATH];

	if (!path || !*path)
		return qfalse;
	Q_strncpyz(normalized, path, sizeof(normalized));
	Sys_XboxNormalizePath(normalized);
	if (CreateDirectoryA(normalized, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
		return qtrue;
	return qfalse;
}

FILE *Sys_Mkfifo(const char *ospath)
{
	(void)ospath;
	return NULL;
}

char *Sys_Cwd(void)
{
	static char cwd[MAX_OSPATH];

	Q_strncpyz(cwd, Sys_XboxBasePath(), sizeof(cwd));
	return cwd;
}

char *Sys_DefaultInstallPath(void)
{
	return (char *)Sys_XboxBasePath();
}

char *Sys_DefaultHomeConfigPath(void)
{
	return (char *)Sys_XboxHomePath();
}

char *Sys_DefaultHomeDataPath(void)
{
	return (char *)Sys_XboxHomePath();
}

char *Sys_DefaultHomeStatePath(void)
{
	return (char *)Sys_XboxHomePath();
}

char *Sys_BinaryPath(void)
{
	return (char *)Sys_XboxBasePath();
}

char *Sys_BinaryPathRelative(const char *relative)
{
	static char path[MAX_OSPATH];

	Com_sprintf(path, sizeof(path), "%s\\%s", Sys_XboxBasePath(), relative);
	return path;
}

const char *Sys_Basename(char *path)
{
	char *slash;

	Sys_XboxNormalizePath(path);
	slash = strrchr(path, '\\');
	return slash ? slash + 1 : path;
}

const char *Sys_Dirname(char *path)
{
	static char dirname[MAX_OSPATH];
	char *slash;

	Q_strncpyz(dirname, path, sizeof(dirname));
	Sys_XboxNormalizePath(dirname);
	slash = strrchr(dirname, '\\');
	if (slash)
		*slash = '\0';
	return dirname;
}

char **Sys_ListFiles(const char *directory, const char *extension, char *filter,
	int *numfiles, qboolean wantsubs)
{
	char search[MAX_OSPATH];
	char **list;
	char *names[4096];
	WIN32_FIND_DATAA info;
	HANDLE handle;
	int count = 0;
	int i;

	(void)filter;
	if (!numfiles || !directory || !*directory)
		return NULL;
	if (!extension)
		extension = "";
	Com_sprintf(search, sizeof(search), "%s\\*%s", directory,
		extension[0] == '/' ? "" : extension);
	handle = FindFirstFileA(search, &info);
	if (handle == INVALID_HANDLE_VALUE)
	{
		*numfiles = 0;
		return NULL;
	}

	do
	{
		qboolean isDir = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? qtrue : qfalse;
		if (!Q_stricmp(info.cFileName, ".") || !Q_stricmp(info.cFileName, ".."))
			continue;
		if ((wantsubs && !isDir) || (!wantsubs && isDir))
			continue;
		if (extension[0] && extension[0] != '/' &&
			strlen(info.cFileName) < strlen(extension))
			continue;
		if (extension[0] && extension[0] != '/' &&
			Q_stricmp(info.cFileName + strlen(info.cFileName) - strlen(extension), extension))
			continue;
		if (count == (int)ARRAY_LEN(names) - 1)
			break;
		names[count++] = CopyString(info.cFileName);
	} while (FindNextFileA(handle, &info));
	FindClose(handle);

	if (!count)
	{
		*numfiles = 0;
		return NULL;
	}
	list = (char **)Z_Malloc((count + 1) * sizeof(*list));
	for (i = 0; i < count; ++i)
		list[i] = names[i];
	list[count] = NULL;
	*numfiles = count;
	return list;
}

void Sys_FreeFileList(char **list)
{
	int i;
	if (!list)
		return;
	for (i = 0; list[i]; ++i)
		Z_Free(list[i]);
	Z_Free(list);
}

qboolean Sys_OpenFolderInFileManager(const char *path, qboolean create)
{
	(void)path;
	(void)create;
	return qfalse;
}

void Sys_GLimpSafeInit(void) {}
void Sys_GLimpInit(void) {}
void Sys_PlatformInit(void) { Sys_XboxPlatformInit(); }
void Sys_PlatformExit(void) { Sys_XboxPlatformShutdown(); }
void Sys_ErrorDialog(const char *error) { Sys_XboxLog("FATAL: %s\n", error); }
void Sys_AnsiColorPrint(const char *message) { Sys_XboxLog("%s", message); }

void *Sys_LoadDll(const char *name, qboolean useSystemLib)
{
	(void)name;
	(void)useSystemLib;
	return NULL;
}

void Sys_UnloadDll(void *handle) { (void)handle; }
qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
const char *Sys_LibraryError(void) { return "dynamic libraries are not supported on Xbox"; }

void *Sys_LoadGameDll(const char *name, vmMainProc *entryPoint,
	intptr_t (QDECL *systemcalls)(intptr_t, ...))
{
	(void)name;
	(void)entryPoint;
	(void)systemcalls;
	return NULL;
}

void Sys_SetEnv(const char *name, const char *value)
{
	(void)name;
	(void)value;
}

char *Sys_ConsoleInput(void)
{
	return NULL;
}

void Sys_ShowIP(void) {}
void Sys_SendPacket(int length, const void *data, netadr_t to)
{
	(void)length;
	(void)data;
	(void)to;
}

dialogResult_t Sys_Dialog(dialogType_t type, const char *message, const char *title)
{
	(void)type;
	(void)title;
	Sys_XboxLog("%s\n", message ? message : "");
	return DR_OK;
}

const char *Sys_XboxBasePath(void)
{
	if (!xboxBinaryPath[0])
		Q_strncpyz(xboxBinaryPath, "D:", sizeof(xboxBinaryPath));
	return xboxBinaryPath;
}

const char *Sys_XboxHomePath(void)
{
	if (!xboxHomePath[0])
		Q_strncpyz(xboxHomePath, "E:\\Apps\\ioquake3", sizeof(xboxHomePath));
	return xboxHomePath;
}

FILE *Sys_XboxOpenGameFile(const char *qpath, const char *mode)
{
	char path[MAX_OSPATH];

	if (!qpath || !*qpath || strstr(qpath, "..") || strchr(qpath, ':') ||
		strchr(qpath, '\\'))
		return NULL;
	Com_sprintf(path, sizeof(path), "%s\\%s\\%s", Sys_XboxBasePath(),
		BASEGAME, qpath);
	return Sys_FOpen(path, mode);
}

void Sys_XboxPlatformInit(void)
{
	Sys_XboxLog("Xbox platform services: base=%s home=%s\n",
		Sys_XboxBasePath(), Sys_XboxHomePath());
}

void Sys_XboxPlatformShutdown(void)
{
	if (xboxLogFile)
	{
		fclose(xboxLogFile);
		xboxLogFile = NULL;
	}
}

void Sys_Error(const char *format, ...)
{
	char message[512];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	Sys_XboxLog("FATAL: %s\n", message);
	Sys_XboxRequestExit();
	for (;;)
		Sleep(1000);
}

void Sys_Quit(void)
{
	Sys_XboxRequestExit();
	for (;;)
		Sleep(1000);
}

void Sys_XboxSleep(unsigned int milliseconds)
{
	Sleep(milliseconds);
}

void Sys_XboxRequestExit(void)
{
	xboxExitRequested = 1;
}

int Sys_XboxExitRequested(void)
{
	return xboxExitRequested;
}
