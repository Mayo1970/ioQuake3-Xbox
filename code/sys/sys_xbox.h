/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#ifndef IOQUAKE3_SYS_XBOX_H
#define IOQUAKE3_SYS_XBOX_H

#include <stdint.h>
#include <stdio.h>

void Sys_XboxLogOpen(void);
void Sys_XboxLog(const char *format, ...);
void Sys_XboxMemoryReport(const char *stage);
void Sys_XboxServerTrace(char *buffer, int size);
void Sys_XboxDiagFrame(void);
void Sys_XboxStartWatchdog(void (*report)(const char *tag));
void Sys_XboxSleep(unsigned int milliseconds);
void Sys_XboxRequestExit(void);
int Sys_XboxExitRequested(void);

/* Platform paths deliberately keep content and mutable state separate. */
const char *Sys_XboxBasePath(void);
const char *Sys_XboxHomePath(void);
FILE *Sys_XboxOpenGameFile(const char *qpath, const char *mode);

/* lwIP socket calls block forever until Sys_XboxNetInit has started its core. */
void Sys_XboxNetInit(void);
int Sys_XboxNetReady(void);
int Sys_XboxNetAddress(uint32_t *ip, uint32_t *netmask);

void Sys_XboxPlatformInit(void);
void Sys_XboxPlatformShutdown(void);

#endif
