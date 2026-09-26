/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#ifndef IOQUAKE3_SYS_XBOX_H
#define IOQUAKE3_SYS_XBOX_H

#include <stdint.h>
#include <stdio.h>

/* Per-flavor names, as ps3_gamedir_defs.h does on the PS3 port. */
#ifdef STANDALONEOA
#define XBOX_TITLE "OpenArena"
#define XBOX_LOG_PATH "D:\\ioquake3_oa.log"
#else
#define XBOX_TITLE "ioQuake3"
#define XBOX_LOG_PATH "D:\\ioquake3.log"
#endif

void Sys_XboxLogOpen(void);
void Sys_XboxLog(const char *format, ...);
void Sys_XboxMemoryReport(const char *stage);
unsigned int Sys_XboxPhysicalMegs(void);
/* Heap tuning at boot, and a trim after big frees (map loads, renderer restarts). */
void Sys_XboxHeapInit(void);
void Sys_XboxHeapTrim(void);
/* Samples free physical memory at the image decode peak for the next memory report. */
void Sys_XboxNoteDecodeMemory(void);
/* Free bytes now and lowest since the last call, or -1 before the zone or sound memory exists. */
void Z_XboxFreeMemory(int *freeBytes, int *lowestFree);
void SND_XboxFreeMemory(int *freeBytes, int *lowestFree);
void Sys_XboxServerTrace(char *buffer, int size);
void Sys_XboxDiagFrame(void);
void Sys_XboxStartWatchdog(void (*report)(const char *tag));
/* Call before Com_Init: CL_Init uses the name as the "name" cvar default, so cvar_restart keeps it. */
void Sys_XboxInitDefaultPlayerName(void);
const char *Sys_XboxDefaultPlayerName(void);
void Sys_XboxSleep(unsigned int milliseconds);
void Sys_XboxRequestExit(void);
int Sys_XboxExitRequested(void);

/* DIAGNOSTIC: SV_Frame sum and longest call in performance counter ticks, and S_memoryLoad calls
   (in game these are sounds reloaded after eviction); the renderer perf line reads and clears them. */
extern int64_t xboxServerFrameTicks;
extern int64_t xboxServerFrameMaxTicks;
extern unsigned int xboxSoundLoads;
int64_t Sys_XboxTicks(void);

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

/* Nonzero when vm.c should load the linked-in module instead of a QVM. */
int Sys_XboxUseBuiltinModule(const char *name);

#endif
