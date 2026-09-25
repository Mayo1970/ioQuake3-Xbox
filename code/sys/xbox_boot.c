/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#include "sys_xbox.h"
#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "../client/client.h"

#include <hal/video.h>

/* 20, not 24: the native modules add ~4 MiB to the XBE image but no longer use the hunk. */
#define XBOX_COM_HUNK_MEGS 20
#define XBOX_COM_ZONE_MEGS 8
/* 1536 sound chunks (~3 MiB); 2 ran out of memory on Q3DM11, so snd_dma.c stores mono sounds as ADPCM. */
#define XBOX_COM_SOUND_MEGS 1

#define XBOX_STRINGIFY_VALUE(value) #value
#define XBOX_STRINGIFY(value) XBOX_STRINGIFY_VALUE(value)

/* DIAGNOSTIC: both netchan ends of the map-load connection; remove after. */
static void XboxLogConnect(const char *tag)
{
	char server[160];

	Sys_XboxServerTrace(server, sizeof(server));
	Sys_XboxLog("Xbox %s: t=%d cl state=%d in=%d out=%d frag=%d/%d msg=%d %s\n", tag,
		Sys_Milliseconds(), (int)clc.state, clc.netchan.incomingSequence,
		clc.netchan.outgoingSequence, clc.netchan.fragmentSequence,
		clc.netchan.fragmentLength, clc.serverMessageSequence, server);
}

int main(void)
{
	char commandLine[] =
		"+set com_hunkMegs " XBOX_STRINGIFY(XBOX_COM_HUNK_MEGS)
		" +set com_zoneMegs " XBOX_STRINGIFY(XBOX_COM_ZONE_MEGS)
		" +set com_soundMegs " XBOX_STRINGIFY(XBOX_COM_SOUND_MEGS)
		/* IPv4 only, as on the PS3, PS4 and Wii U ports. */
		" +set net_enabled 1"
		/* Linked-in native modules; overrides archived vm_* values. A pure server
		   drops clients without cgame/ui QVM pak refs, as on the PSP port. */
		" +set vm_game 0 +set vm_cgame 0 +set vm_ui 0 +set sv_pure 0"
#ifdef XBOX_DIAG_NO_CINEMATIC
		/* DIAGNOSTIC bisection switch: any non-set command skips idlogo.RoQ. */
		" +wait"
#endif
		;

	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
	Sys_XboxLogOpen();
	Sys_XboxLog(XBOX_TITLE "\n");
	{
		unsigned int physicalMegs = Sys_XboxPhysicalMegs();

		Sys_XboxLog("Xbox RAM: %u MiB physical, %s unit detected\n", physicalMegs,
			physicalMegs > 64 ? "128 MB" : "64 MB");
	}
	Sys_XboxLog("Xbox startup budget: hunk=%d MiB zone=%d MiB sound=%d units\n",
		XBOX_COM_HUNK_MEGS, XBOX_COM_ZONE_MEGS, XBOX_COM_SOUND_MEGS);
	/* Before Com_Init, so the DHCP/link-local wait still shows on the debug screen. */
	Sys_XboxNetInit();
	Sys_XboxMemoryReport("before Com_Init");
	Com_Init(commandLine);
	NET_Init();
	Sys_XboxMemoryReport("after Com_Init");
	/* The Xbox renderer never calls ri.IN_Init, and bindings need Com_Init done. */
	IN_Init(NULL);
	Sys_XboxLog(
		"Xbox active budget: hunk=%d MiB zone=%d MiB sound=%d units "
		"hunk_remaining=%d KiB\n",
		Cvar_VariableIntegerValue("com_hunkMegs"),
		Cvar_VariableIntegerValue("com_zoneMegs"),
		Cvar_VariableIntegerValue("com_soundMegs"),
		Hunk_MemoryRemaining() / 1024);
	Cmd_ExecuteString("meminfo");
	Sys_XboxStartWatchdog(XboxLogConnect);

	for (;;)
	{
		Com_Frame();
		Sys_XboxDiagFrame();
		if (Sys_XboxExitRequested())
		{
			IN_Shutdown();
			Com_Shutdown();
			NET_Shutdown();
			return 0;
		}
	}
}
