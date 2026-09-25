/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#include "sys_xbox.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../renderernv2a/xbox_nv2a.h"

#include <hal/debug.h>
#include <xboxkrnl/xboxkrnl.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fileapi.h>

/* DIAGNOSTIC: 0 flushes every line to find the crash point; restore 250 after. */
#define XBOX_LOG_FLUSH_MSEC 0
#define XBOX_FATAL_DISPLAY_MSEC 10000

static volatile int xboxExitRequested;
static FILE *xboxLogFile;
static DWORD xboxLogLastFlush;
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
#ifdef XBOX_DEBUG_LOG
	if (xboxLogFile)
		return;

	xboxLogFile = fopen(XBOX_LOG_PATH, "wb");
	if (!xboxLogFile)
		debugPrint("Xbox log open failed: " XBOX_LOG_PATH "\n");
#endif
}

void Sys_XboxLog(const char *format, ...)
{
	char message[512];
	va_list args;

#ifndef XBOX_DEBUG_LOG
	/* No log file: only the debug screen (boot, fatal errors) still shows the text. */
	if (XboxNV2A_OwnsScreen())
		return;
#endif
	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	/* debugPrint targets the hidden kernel framebuffer while pbkit owns the screen. */
	if (!XboxNV2A_OwnsScreen())
		debugPrint("%s", message);
	if (xboxLogFile)
	{
		DWORD now = GetTickCount();

		fputs(message, xboxLogFile);
		if (!XboxNV2A_OwnsScreen() || now - xboxLogLastFlush >= XBOX_LOG_FLUSH_MSEC)
		{
			fflush(xboxLogFile);
			xboxLogLastFlush = now;
		}
	}
}

void Sys_XboxMemoryReport(const char *stage)
{
	MM_STATISTICS statistics;
	NTSTATUS status;

	memset(&statistics, 0, sizeof(statistics));
	statistics.Length = sizeof(statistics);
	status = MmQueryStatistics(&statistics);
	if (status != 0)
	{
		Sys_XboxLog("Xbox memory %s: MmQueryStatistics failed 0x%08x\n",
			stage ? stage : "", (unsigned int)status);
		return;
	}

	Sys_XboxLog(
		"Xbox memory %s: physical=%u MiB available=%u KiB "
		"committed=%u MiB reserved=%u MiB image=%u MiB stack=%u KiB\n",
		stage ? stage : "",
		(unsigned int)((statistics.TotalPhysicalPages * 4U) / 1024U),
		(unsigned int)(statistics.AvailablePages * 4U),
		(unsigned int)(statistics.VirtualMemoryBytesCommitted / (1024U * 1024U)),
		(unsigned int)(statistics.VirtualMemoryBytesReserved / (1024U * 1024U)),
		(unsigned int)((statistics.ImagePagesCommitted * 4U) / 1024U),
		(unsigned int)((statistics.StackPagesCommitted * 4U) / 1024U));
}

/* 0 if the query fails. A 128 MB unit also needs a 128 MB BIOS and the XBE 64 MB flag cleared. */
unsigned int Sys_XboxPhysicalMegs(void)
{
	MM_STATISTICS statistics;

	memset(&statistics, 0, sizeof(statistics));
	statistics.Length = sizeof(statistics);
	if (MmQueryStatistics(&statistics) != 0)
		return 0;
	return (unsigned int)((statistics.TotalPhysicalPages * 4U) / 1024U);
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

int64_t xboxServerFrameTicks;
int64_t xboxServerFrameMaxTicks;
unsigned int xboxSoundLoads;

int64_t Sys_XboxTicks(void)
{
	LARGE_INTEGER now;

	QueryPerformanceCounter(&now);
	return now.QuadPart;
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

char *Sys_SteamPath(void) { return ""; }
char *Sys_GogPath(void) { return ""; }
char *Sys_MicrosoftStorePath(void) { return ""; }

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
	DWORD attributes;
	DWORD error;

	if (!path || !*path)
		return qfalse;
	Q_strncpyz(normalized, path, sizeof(normalized));
	Sys_XboxNormalizePath(normalized);

	if (CreateDirectoryA(normalized, NULL))
		return qtrue;

	/* FATX/nxdk can report a stale last-error when the directory exists; */
	/* query it directly, since FS_CreatePath calls us for every parent. */
	error = GetLastError();
	attributes = GetFileAttributesA(normalized);
	if (attributes != INVALID_FILE_ATTRIBUTES &&
		(attributes & FILE_ATTRIBUTE_DIRECTORY))
		return qtrue;

	Sys_XboxLog("Xbox mkdir failed: %s error=0x%08x attrs=0x%08x\n",
		normalized, (unsigned int)error, (unsigned int)attributes);
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

qboolean Sys_DllExtension(const char *name) { (void)name; return qfalse; }
const char *Sys_LibraryError(void) { return "dynamic libraries are not supported on Xbox"; }

void Sys_SetEnv(const char *name, const char *value)
{
	(void)name;
	(void)value;
}

char *Sys_ConsoleInput(void)
{
	return NULL;
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
	{
		DWORD attributes;

		/* Mutable data belongs on E:, but some launchers do not mount it; */
		/* fall back to D:\BASEGAME, which already holds the shipped game data. */
		if (CreateDirectoryA("E:\\" BASEGAME, NULL))
		{
			Q_strncpyz(xboxHomePath, "E:", sizeof(xboxHomePath));
		}
		else
		{
			attributes = GetFileAttributesA("E:\\" BASEGAME);
			if (attributes != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				Q_strncpyz(xboxHomePath, "E:", sizeof(xboxHomePath));
			}
			else if ((attributes = GetFileAttributesA("D:\\" BASEGAME)) != INVALID_FILE_ATTRIBUTES &&
				(attributes & FILE_ATTRIBUTE_DIRECTORY))
			{
				Sys_XboxLog("Xbox home path: E:\\" BASEGAME " unavailable, using D:\\" BASEGAME "\n");
				Q_strncpyz(xboxHomePath, "D:", sizeof(xboxHomePath));
			}
			else
			{
				/* Keep the normal path in the diagnostic case. */
				Q_strncpyz(xboxHomePath, "E:", sizeof(xboxHomePath));
			}
		}
	}
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

	XboxNV2A_ShowDebugScreen();
	Sys_XboxLog("FATAL: %s\n", message);
	Sys_XboxLog("Returning to the dashboard in %d seconds.\n",
		XBOX_FATAL_DISPLAY_MSEC / 1000);
	Sleep(XBOX_FATAL_DISPLAY_MSEC);
	XboxNV2A_Kill();
	Sys_XboxPlatformShutdown();
	exit(1);
}

void Sys_Quit(void)
{
	NET_Shutdown();
	XboxNV2A_Kill();
	Sys_XboxPlatformShutdown();
	exit(0);
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
