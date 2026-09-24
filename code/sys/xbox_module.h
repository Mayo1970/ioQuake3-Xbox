/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

/* Force-included into every native game-module file by Makefile.xbox. */

#ifndef IOQUAKE3_XBOX_MODULE_H
#define IOQUAKE3_XBOX_MODULE_H

/* pdclib drops %f; xbox_module_libc.c supplies float-capable versions. */
#define vsnprintf XboxModule_vsnprintf
#define sscanf XboxModule_sscanf

/* Unique entry names; the build localizes every other module symbol. */
#if defined(QAGAME)
#define vmMain vmMainQAG
#define dllEntry dllEntryQAG
#elif defined(CGAME)
#define vmMain vmMainCG
#define dllEntry dllEntryCG
#elif defined(UI)
#define vmMain vmMainUI
#define dllEntry dllEntryUI
#else
#error "Native Xbox module without QAGAME, CGAME or UI"
#endif

/* Own sections per module, so xbox_modules.c can reset its state on each load. */
#if defined(QAGAME)
#pragma clang section bss=".bss$qag1" data=".data$qag1"
#elif defined(CGAME)
#pragma clang section bss=".bss$cg1" data=".data$cg1"
#else
#pragma clang section bss=".bss$ui1" data=".data$ui1"
#endif

#include "../qcommon/q_shared.h"

/* The entry points are linked into the XBE, not exported from a DLL. */
#undef Q_EXPORT
#define Q_EXPORT

#endif
