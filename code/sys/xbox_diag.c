/* DIAGNOSTIC: server end of the map-load connection trace; bg_public.h blocks client.h here. */
#include "../server/server.h"
#include "sys_xbox.h"

#include <windows.h>

static volatile unsigned int xboxDiagFrames;
static void (*xboxDiagReport)(const char *tag);

void Sys_XboxServerTrace(char *buffer, int size)
{
	const client_t *client;

	if (!com_sv_running || !com_sv_running->integer || !svs.clients) {
		Q_strncpyz(buffer, "sv=off", size);
		return;
	}
	client = &svs.clients[0];
	Com_sprintf(buffer, size, "sv state=%d out=%d unsent=%d frag=%d/%d queued=%d time=%d",
		client->state, client->netchan.outgoingSequence, client->netchan.unsentFragments,
		client->netchan.unsentFragmentStart, client->netchan.unsentLength,
		client->netchan_start_queue != NULL, sv.time);
}

void Sys_XboxDiagFrame(void)
{
	xboxDiagFrames++;
}

/* Reports only while Com_Frame is stuck; a CPU fault halts this thread too, so silence means a crash. */
static DWORD WINAPI XboxDiagWatchdog(LPVOID unused)
{
	unsigned int last = xboxDiagFrames;
	int reports = 0;

	(void)unused;
	while (reports < 5) {
		Sleep(3000);
		if (xboxDiagFrames == last) {
			xboxDiagReport("stalled");
			reports++;
		}
		last = xboxDiagFrames;
	}
	return 0;
}

void Sys_XboxStartWatchdog(void (*report)(const char *tag))
{
	xboxDiagReport = report;
	if (!CreateThread(NULL, 0, XboxDiagWatchdog, NULL, 0, NULL))
		Sys_XboxLog("Xbox diag: watchdog thread failed\n");
}
