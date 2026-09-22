/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"

#include <hal/debug.h>
#include <windows.h>

#include <stdarg.h>
#include <stdio.h>

static volatile int xboxExitRequested;

void Sys_XboxLog(const char *format, ...)
{
	char message[512];
	va_list args;

	va_start(args, format);
	vsnprintf(message, sizeof(message), format, args);
	va_end(args);

	debugPrint("%s", message);
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
