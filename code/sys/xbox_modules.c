/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

/* Native qagame, cgame and ui, linked into the XBE by Makefile.xbox. */

#include "sys_xbox.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <stdlib.h>
#include <string.h>

typedef void (*xboxDllEntry_t)(intptr_t (QDECL *syscallptr)(intptr_t, ...));

#define XBOX_MODULE_DECLARE(suffix) \
	intptr_t vmMain##suffix(int command, int arg0, int arg1, int arg2, int arg3, int arg4, \
		int arg5, int arg6, int arg7, int arg8, int arg9, int arg10, int arg11); \
	void dllEntry##suffix(intptr_t (QDECL *syscallptr)(intptr_t, ...)); \
	extern char xbox##suffix##DataStart[], xbox##suffix##DataEnd[]; \
	extern char xbox##suffix##BssStart[], xbox##suffix##BssEnd[]

XBOX_MODULE_DECLARE(QAG);
XBOX_MODULE_DECLARE(CG);
XBOX_MODULE_DECLARE(UI);

/* Empty markers around each module's .data$<tag>1/.bss$<tag>1 (see xbox_module.h).
   lld orders same-named sections by their $ suffix, so 0 and 2 bracket 1. */
#define XBOX_MODULE_MARKERS(tag, suffix) \
	__asm__( \
		".section .data$" tag "0,\"dw\"\n.globl _xbox" suffix "DataStart\n_xbox" suffix "DataStart:\n" \
		".section .data$" tag "2,\"dw\"\n.globl _xbox" suffix "DataEnd\n_xbox" suffix "DataEnd:\n" \
		".section .bss$" tag "0,\"bw\"\n.globl _xbox" suffix "BssStart\n_xbox" suffix "BssStart:\n" \
		".section .bss$" tag "2,\"bw\"\n.globl _xbox" suffix "BssEnd\n_xbox" suffix "BssEnd:\n" \
		".text\n")

XBOX_MODULE_MARKERS("qag", "QAG");
XBOX_MODULE_MARKERS("cg", "CG");
XBOX_MODULE_MARKERS("ui", "UI");

typedef struct
{
	const char *name;
	vmMainProc vmMain;
	xboxDllEntry_t dllEntry;
	char *dataStart;
	char *dataEnd;
	char *bssStart;
	char *bssEnd;
	void *pristineData;
	qboolean active;
} xboxModule_t;

#define XBOX_MODULE(name, suffix) \
	{ name, vmMain##suffix, dllEntry##suffix, \
		xbox##suffix##DataStart, xbox##suffix##DataEnd, \
		xbox##suffix##BssStart, xbox##suffix##BssEnd, NULL, qfalse }

static xboxModule_t xboxModules[] =
{
	XBOX_MODULE("qagame", QAG),
	XBOX_MODULE("cgame", CG),
	XBOX_MODULE("ui", UI),
};

static xboxModule_t *Sys_XboxFindModule(const char *name)
{
	int i;

	for (i = 0; i < (int)ARRAY_LEN(xboxModules); i++)
	{
		if (!Q_stricmp(name, xboxModules[i].name))
			return &xboxModules[i];
	}
	return NULL;
}

int Sys_XboxUseBuiltinModule(const char *name)
{
	/* These are the baseq3 modules; mods keep loading their own QVMs. */
	return Sys_XboxFindModule(name) && !Q_stricmp(FS_GetCurrentGameDir(), BASEGAME);
}

/* A QVM or DLL starts from fresh globals; restore .data and zero .bss to match. */
static qboolean Sys_XboxResetModule(xboxModule_t *module)
{
	size_t dataSize = (size_t)(module->dataEnd - module->dataStart);
	size_t bssSize = (size_t)(module->bssEnd - module->bssStart);

	if (!module->pristineData)
	{
		/* First load: the module has never run, so .data is still the linked image. */
		module->pristineData = malloc(dataSize ? dataSize : 1);
		if (!module->pristineData)
		{
			Com_Printf("Xbox module %s: no memory for the %d byte data snapshot\n",
				module->name, (int)dataSize);
			return qfalse;
		}
		memcpy(module->pristineData, module->dataStart, dataSize);
		Com_Printf("Xbox module %s: data %d KiB, bss %d KiB\n", module->name,
			(int)(dataSize / 1024), (int)(bssSize / 1024));
		return qtrue;
	}

	memcpy(module->dataStart, module->pristineData, dataSize);
	memset(module->bssStart, 0, bssSize);
	return qtrue;
}

void *Sys_LoadGameDll(const char *name, vmMainProc *entryPoint,
	intptr_t (QDECL *systemcalls)(intptr_t, ...))
{
	xboxModule_t *module = Sys_XboxFindModule(name);

	if (!module)
	{
		Com_Printf("Xbox module %s: not built in\n", name);
		return NULL;
	}
	if (module->active)
	{
		Com_Printf("Xbox module %s: already loaded\n", name);
		return NULL;
	}

	/* *entryPoint stays untouched on failure so vm.c can fall back to the QVM. */
	if (!Sys_XboxResetModule(module))
		return NULL;

	module->active = qtrue;
	*entryPoint = module->vmMain;
	Com_Printf("Xbox module %s: native\n", module->name);
	module->dllEntry(systemcalls);
	return module;
}

void Sys_UnloadDll(void *handle)
{
	xboxModule_t *module = handle;

	if (module >= xboxModules && module < xboxModules + ARRAY_LEN(xboxModules))
		module->active = qfalse;
}
