/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

cvar_t *com_developer;
cvar_t *com_cl_running;
cvar_t *com_sv_running;
qboolean com_errorEntered;

void Com_InitSmallZoneMemory(void)
{
	/* G1 uses the Xbox heap shim; the full zone allocator lands with G2. */
}

static void *Xbox_Alloc(int size, qboolean clear)
{
	void *memory;

	if (size < 1)
		size = 1;
	memory = clear ? calloc(1, (size_t)size) : malloc((size_t)size);
	if (!memory)
		Sys_Error("Xbox qcommon allocation failed: %d bytes", size);
	return memory;
}

void *Z_TagMallocDebug(int size, int tag, char *label, char *file, int line)
{
	(void)tag;
	(void)label;
	(void)file;
	(void)line;
	return Xbox_Alloc(size, qfalse);
}

void *Z_MallocDebug(int size, char *label, char *file, int line)
{
	(void)label;
	(void)file;
	(void)line;
	return Xbox_Alloc(size, qtrue);
}

void *S_MallocDebug(int size, char *label, char *file, int line)
{
	return Z_TagMallocDebug(size, 0, label, file, line);
}

void Z_Free(void *ptr)
{
	free(ptr);
}

void Z_FreeTags(int tag)
{
	(void)tag;
}

char *CopyString(const char *in)
{
	char *copy = (char *)Z_Malloc((int)strlen(in) + 1);
	strcpy(copy, in);
	return copy;
}

void QDECL Com_Printf(const char *format, ...)
{
	char message[MAXPRINTMSG];
	va_list args;

	va_start(args, format);
	Q_vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	Sys_Print(message);
}

void QDECL Com_DPrintf(const char *format, ...)
{
	char message[MAXPRINTMSG];
	va_list args;

	va_start(args, format);
	Q_vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	Sys_Print(message);
}

/* The shell has no client event loop yet; keep the real qcommon event API
 * visible and observable until the full client is linked. */
void Com_QueueEvent(int time, sysEventType_t type, int value, int value2,
	int ptrLength, void *ptr)
{
	(void)time;
	(void)ptrLength;
	(void)ptr;
	if (type == SE_KEY)
		Sys_XboxLog("input event: key=%d down=%d\n", value, value2);
	else if (type == SE_JOYSTICK_AXIS)
		Sys_XboxLog("input event: axis=%d value=%d\n", value, value2);
}

void QDECL Com_Error(int code, const char *format, ...)
{
	char message[MAXPRINTMSG];
	va_list args;

	(void)code;
	va_start(args, format);
	Q_vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	Sys_Error("%s", message);
}

int Com_Milliseconds(void)
{
	return Sys_Milliseconds();
}

int Com_Filter(char *filter, char *name, int casesensitive)
{
	(void)casesensitive;
	return filter && name && Q_stristr(name, filter) != NULL;
}

int Com_FilterPath(char *filter, char *name, int casesensitive)
{
	return Com_Filter(filter, name, casesensitive);
}

void Field_CompleteFilename(const char *dir, const char *ext, char *args,
	qboolean stripExt, qboolean allowNonPure)
{
	(void)dir;
	(void)ext;
	(void)args;
	(void)stripExt;
	(void)allowNonPure;
}

void Field_CompleteCommand(char *cmd, qboolean doCommands, qboolean doCvars)
{
	(void)cmd;
	(void)doCommands;
	(void)doCvars;
}

qboolean CL_GameCommand(void) { return qfalse; }
qboolean SV_GameCommand(void) { return qfalse; }
qboolean UI_GameCommand(void) { return qfalse; }
void CL_ForwardCommandToServer(const char *string) { (void)string; }

long FS_ReadFile(const char *qpath, void **buffer)
{
	FILE *file;
	long length;
	void *data;

	if (buffer)
		*buffer = NULL;
	file = Sys_XboxOpenGameFile(qpath, "rb");
	if (!file)
		return -1;
	if (fseek(file, 0, SEEK_END) != 0)
	{
		fclose(file);
		return -1;
	}
	length = ftell(file);
	if (length < 0 || fseek(file, 0, SEEK_SET) != 0)
	{
		fclose(file);
		return -1;
	}
	if (!buffer)
	{
		fclose(file);
		return length;
	}
	data = Z_Malloc((int)length + 1);
	if (fread(data, 1, (size_t)length, file) != (size_t)length)
	{
		fclose(file);
		Z_Free(data);
		return -1;
	}
	fclose(file);
	((byte *)data)[length] = '\0';
	*buffer = data;
	return length;
}

void FS_FreeFile(void *buffer)
{
	Z_Free(buffer);
}

int FS_Write(const void *buffer, int len, fileHandle_t f)
{
	(void)buffer;
	(void)f;
	return len;
}

double atof(const char *text)
{
	const char *p = text;
	double value = 0.0;
	double fraction = 0.1;
	int sign = 1;
	int exponent = 0;
	int exponentSign = 1;

	while (*p == ' ' || *p == '\t')
		p++;
	if (*p == '+' || *p == '-')
	{
		if (*p == '-')
			sign = -1;
		p++;
	}

	while (*p >= '0' && *p <= '9')
		value = value * 10.0 + (*p++ - '0');

	if (*p == '.')
	{
		p++;
		while (*p >= '0' && *p <= '9')
		{
			value += (*p++ - '0') * fraction;
			fraction *= 0.1;
		}
	}

	if (*p == 'e' || *p == 'E')
	{
		p++;
		if (*p == '+' || *p == '-')
		{
			if (*p == '-')
				exponentSign = -1;
			p++;
		}
		while (*p >= '0' && *p <= '9')
			exponent = exponent * 10 + (*p++ - '0');
		while (exponent-- > 0)
		{
			if (exponentSign < 0)
				value *= 0.1;
			else
				value *= 10.0;
		}
	}

	return sign * value;
}

void __chkstk(void) {}
void __xbox_assert(const char *expression, const char *file, int line)
{
	Sys_Error("assertion failed: %s (%s:%d)", expression, file, line);
}
