/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"
#include "xbox_input.h"
#include "xbox_snd.h"

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
	const xboxControllerState_t *controller;
	qboolean lastConnected = qfalse;
	uint16_t lastButtons = 0;
	int lastA = -1;
	int lastB = -1;

	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	Sys_XboxLogOpen();
	Sys_XboxPlatformInit();
	IN_Init(NULL);
	if (!Sys_XboxSoundInit(NULL, NULL))
		Sys_XboxLog("AC97 sound service failed to initialize\n");
	else
		Sys_XboxLog("AC97 sound service initialized (silent PCM test)\n");

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
		IN_Frame();
		controller = Sys_XboxInputState(0);
		if (controller->connected != lastConnected)
		{
			Sys_XboxLog("controller port 0: %s\n",
				controller->connected ? "connected" : "disconnected");
			lastConnected = controller->connected;
		}
		if (controller->connected &&
			(controller->buttons != lastButtons || controller->a != lastA ||
				controller->b != lastB))
		{
			Sys_XboxLog("controller port 0: buttons=%04x A=%d B=%d\n",
				controller->buttons, controller->a, controller->b);
			lastButtons = controller->buttons;
			lastA = controller->a;
			lastB = controller->b;
		}
		if (Sys_XboxExitRequested())
		{
			Sys_XboxSoundShutdown();
			IN_Shutdown();
			Sys_XboxPlatformShutdown();
			return 0;
		}

		Sys_XboxSleep(1000);
	}
}
