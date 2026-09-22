/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"
#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"

#include <hal/video.h>

#define XBOX_COM_HUNK_MEGS 16
#define XBOX_COM_ZONE_MEGS 8
#define XBOX_COM_SOUND_MEGS 1

#define XBOX_STRINGIFY_VALUE(value) #value
#define XBOX_STRINGIFY(value) XBOX_STRINGIFY_VALUE(value)

int main(void)
{
	char commandLine[] =
		"+set com_hunkMegs " XBOX_STRINGIFY(XBOX_COM_HUNK_MEGS)
		" +set com_zoneMegs " XBOX_STRINGIFY(XBOX_COM_ZONE_MEGS)
		" +set com_soundMegs " XBOX_STRINGIFY(XBOX_COM_SOUND_MEGS);

	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	Sys_XboxLogOpen();
	Sys_XboxLog("ioQuake3 Xbox native engine shell\n");
	Sys_XboxLog("Xbox startup budget: hunk=%d MiB zone=%d MiB sound=%d units\n",
		XBOX_COM_HUNK_MEGS, XBOX_COM_ZONE_MEGS, XBOX_COM_SOUND_MEGS);
	Sys_XboxMemoryReport("before Com_Init");
	Com_Init(commandLine);
	Sys_XboxMemoryReport("after Com_Init");
	Sys_XboxLog(
		"Xbox active budget: hunk=%d MiB zone=%d MiB sound=%d units "
		"hunk_remaining=%d KiB\n",
		Cvar_VariableIntegerValue("com_hunkMegs"),
		Cvar_VariableIntegerValue("com_zoneMegs"),
		Cvar_VariableIntegerValue("com_soundMegs"),
		Hunk_MemoryRemaining() / 1024);
	Cmd_ExecuteString("meminfo");

	for (;;)
	{
		Com_Frame();
		if (Sys_XboxExitRequested())
		{
			Com_Shutdown();
			return 0;
		}
	}
}
