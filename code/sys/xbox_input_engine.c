/* ioQuake3 input-system boundary for the native Xbox controller service. */
#include "xbox_input.h"

void IN_Init(void *windowData)
{
	(void)windowData;
	Sys_XboxInputInit();
}

void IN_Frame(void)
{
	Sys_XboxInputFrame();
}

void IN_Shutdown(void)
{
	Sys_XboxInputShutdown();
}

void IN_Restart(void)
{
	IN_Shutdown();
	IN_Init(NULL);
}
