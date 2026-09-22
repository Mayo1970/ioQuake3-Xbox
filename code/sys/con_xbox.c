/*
===========================================================================
Copyright (C) 1999-2005 Id Software, Inc.

This file is part of Quake III Arena source code.
===========================================================================
*/

#include "sys_xbox.h"

void CON_XboxPrint(const char *message)
{
	Sys_XboxLog("%s", message);
}
