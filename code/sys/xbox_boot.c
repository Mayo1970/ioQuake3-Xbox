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

void CON_XboxPrint(const char *message);
void Com_InitSmallZoneMemory(void);

static void Xbox_Quit_f(void)
{
	Sys_XboxRequestExit();
}

int main(void)
{
	cvar_t *shellCvar;

	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	Sys_XboxLogOpen();

	CON_XboxPrint("ioQuake3 Xbox nxdk skeleton\n");
	CON_XboxPrint("G1 qcommon shell boundary reached\n");

	Com_InitSmallZoneMemory();
	Cvar_Init();
	Cmd_Init();
	Cmd_AddCommand("quit", Xbox_Quit_f);
	shellCvar = Cvar_Get("xbox_shell", "qcommon", CVAR_ROM);

	Com_Printf("qcommon initialized: %s\n", shellCvar->string);
	Cbuf_Init();
	Cbuf_AddText("echo Xbox command buffer online\n");
	Cbuf_Execute();
	Com_Printf("controlled exit command: quit\n");

	for (;;)
	{
		if (Sys_XboxExitRequested())
			return 0;

		Sys_XboxSleep(1000);
	}
}
