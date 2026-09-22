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

static volatile int xboxExitRequested;
static FILE *xboxLogFile;

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
	return (int)GetTickCount();
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

void Sys_SetEnv(const char *name, const char *value)
{
	(void)name;
	(void)value;
}

void Sys_DisplaySystemConsole(qboolean show)
{
	(void)show;
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
