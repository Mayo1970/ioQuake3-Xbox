/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"

#include <hal/video.h>

void CON_XboxPrint(const char *message);

int main(void)
{
	XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);

	CON_XboxPrint("ioQuake3 Xbox nxdk skeleton\n");
	CON_XboxPrint("V1 build boundary reached\n");

	for (;;)
	{
		if (Sys_XboxExitRequested())
			return 0;

		Sys_XboxSleep(1000);
	}
}
