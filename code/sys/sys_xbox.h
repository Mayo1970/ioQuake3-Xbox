/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#ifndef IOQUAKE3_SYS_XBOX_H
#define IOQUAKE3_SYS_XBOX_H

#include <stdio.h>

void Sys_XboxLogOpen(void);
void Sys_XboxLog(const char *format, ...);
void Sys_XboxSleep(unsigned int milliseconds);
void Sys_XboxRequestExit(void);
int Sys_XboxExitRequested(void);

/* Platform paths deliberately keep content and mutable state separate. */
const char *Sys_XboxBasePath(void);
const char *Sys_XboxHomePath(void);
FILE *Sys_XboxOpenGameFile(const char *qpath, const char *mode);

void Sys_XboxPlatformInit(void);
void Sys_XboxPlatformShutdown(void);

#endif
