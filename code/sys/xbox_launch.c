/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

/* Q3 and Team Arena XBEs hand a game switch to each other through launch data. */

#include "sys_xbox.h"

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"
#include "../renderernv2a/xbox_nv2a.h"

#include <hal/xbox.h>
#include <xboxkrnl/xboxkrnl.h>

#include <stdlib.h>
#include <string.h>

#ifndef STANDALONEOA

/* Marks our launch data, so data from a dashboard or another title is ignored. */
#define XBOX_LAUNCH_MAGIC "ioq3-xbox-launch"
#define XBOX_LAUNCH_COMMANDS 512

/* Relative, so nxdk resolves it next to this XBE; "D:\\" would give \??\D:, gone after the reboot. */
#ifdef MISSIONPACK
#define XBOX_OTHER_XBE "default.xbe"
#else
#define XBOX_OTHER_XBE "ta.xbe"
#endif

typedef struct
{
	char magic[sizeof(XBOX_LAUNCH_MAGIC)];
	char commands[XBOX_LAUNCH_COMMANDS];
} xboxLaunchData_t;

/* XLaunchXBEEx copies the whole LaunchData field, so the buffer has its size. */
static union
{
	xboxLaunchData_t data;
	UCHAR page[sizeof(((LAUNCH_DATA_PAGE *)0)->LaunchData)];
} xboxLaunch;

#endif

void Sys_XboxAppendLaunchCommands(char *commandLine, int size)
{
#ifndef STANDALONEOA
	unsigned long type;
	const unsigned char *data;
	char commands[XBOX_LAUNCH_COMMANDS];
	int i;

	/* With no page, this allocates one at boot; a hand-over page allocated after shutdown froze TA. */
	if (XGetLaunchInfo(&type, &data) != 0)
		return;

	if (type == LDT_TITLE && !memcmp(data, XBOX_LAUNCH_MAGIC, sizeof(XBOX_LAUNCH_MAGIC)))
	{
		Q_strncpyz(commands, (const char *)data + sizeof(XBOX_LAUNCH_MAGIC), sizeof(commands));
		for (i = 0; commands[i]; i++)
		{
			if (commands[i] < ' ' || commands[i] > '~')
			{
				commands[0] = '\0';
				break;
			}
		}
		Q_strcat(commandLine, size, commands);
		Sys_XboxLog("Xbox launch data:%s\n", commands);
	}

	/* Quit's quick reboot reads this page; an unpersisted empty one crashed Q3, so point it at the dashboard. */
	MmPersistContiguousMemory(LaunchDataPage, sizeof(*LaunchDataPage), TRUE);
	LaunchDataPage->Header.dwLaunchDataType = LDT_LAUNCH_DASHBOARD;
	LaunchDataPage->Header.szLaunchPath[0] = '\0';
	memset(LaunchDataPage->LaunchData, 0, sizeof(LaunchDataPage->LaunchData));
#endif
}

void Sys_XboxHandOverGame(const char *game)
{
#ifndef STANDALONEOA
	char *commands = xboxLaunch.data.commands;
	int size = sizeof(xboxLaunch.data.commands);
	FILE *xbe;

#ifdef MISSIONPACK
	if (!Q_stricmp(game, BASETA))
		return;
#else
	if (Q_stricmp(game, BASETA))
		return;
#endif

	/* Without the other XBE the switch stays here, and that game runs as QVMs. */
	xbe = Sys_FOpen("D:\\" XBOX_OTHER_XBE, "rb");
	if (!xbe)
	{
		Com_Printf("Xbox: no D:\\" XBOX_OTHER_XBE ", %s stays in this XBE\n",
			game[0] ? game : BASEGAME);
		return;
	}
	fclose(xbe);

	memset(&xboxLaunch, 0, sizeof(xboxLaunch));
	memcpy(xboxLaunch.data.magic, XBOX_LAUNCH_MAGIC, sizeof(XBOX_LAUNCH_MAGIC));
#ifdef MISSIONPACK
	/* The Q3 XBE boots in baseq3 and runs any other mod as QVMs. */
	if (game[0])
		Q_strcat(commands, size, va(" +set fs_game %s", game));
#endif
	if (clc.demoplaying)
		Q_strcat(commands, size, va(" +demo \"%s\"", clc.demoName));
	else if (clc.state >= CA_CONNECTED && clc.serverAddress.type == NA_IP)
		Q_strcat(commands, size, va(" +connect %s", NET_AdrToStringwPort(clc.serverAddress)));
	else
		/* Any startup command skips the intro cinematic. */
		Q_strcat(commands, size, " +wait");

	Com_Printf("Xbox: %s runs in " XBOX_OTHER_XBE ", relaunching with:%s\n",
		game[0] ? game : BASEGAME, commands);

	/* Com_Quit_f and Sys_Quit's shutdown plus main's USB stop; then launch instead of exit. */
	VM_Forced_Unload_Start();
	SV_Shutdown("Game directory changed");
	CL_Shutdown("Game directory changed", qtrue, qtrue);
	VM_Forced_Unload_Done();
	IN_Shutdown();
	Com_Shutdown();
	FS_Shutdown(qtrue);
	NET_Shutdown();
	XboxNV2A_Kill();
	Sys_XboxPlatformShutdown();
	XLaunchXBEEx(XBOX_OTHER_XBE, &xboxLaunch);
	/* Returns only if the path conversion failed; leave like quit does. */
	exit(0);
#endif
}
