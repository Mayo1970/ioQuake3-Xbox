/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#include "sys_xbox.h"

#include <nxdk/net.h>
#include <lwip/autoip.h>
#include <lwip/dhcp.h>
#include <lwip/dns.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/netdb.h>
#include <windows.h>

#include <string.h>

/* RFC 3927 probes plus the announce delay take up to about 9 s. */
#define XBOX_NET_AUTOIP_WAIT_MSEC 12000
#define XBOX_NET_POLL_MSEC 250
/* One slot per master name; a result stays cached for later refreshes. */
#define XBOX_NET_MAX_LOOKUPS 8
/* A failed lookup answers from the cache this long, so the rerun command does not start it again. */
#define XBOX_NET_LOOKUP_RETRY_MSEC 30000
/* Stack size 0 would take the 1 MiB XBE stack; getaddrinfo needs a few KiB. */
#define XBOX_NET_LOOKUP_STACK (32 * 1024)

enum { XBOX_LOOKUP_FREE, XBOX_LOOKUP_RUNNING, XBOX_LOOKUP_DONE, XBOX_LOOKUP_FAILED };

typedef struct
{
	char host[128];
	volatile LONG state;
	volatile uint32_t ip;
	DWORD startedAt;
	DWORD failedAt;
} xboxNetLookup_t;

extern struct netif *g_pnetif;

static int xboxNetReady;
static xboxNetLookup_t xboxNetLookups[XBOX_NET_MAX_LOOKUPS];

static void Sys_XboxNetLogAddress(void)
{
	char ip[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];
	char gateway[IP4ADDR_STRLEN_MAX];
	char dns1[IPADDR_STRLEN_MAX];
	char dns2[IPADDR_STRLEN_MAX];

	ip4addr_ntoa_r(netif_ip4_addr(g_pnetif), ip, sizeof(ip));
	ip4addr_ntoa_r(netif_ip4_netmask(g_pnetif), netmask, sizeof(netmask));
	ip4addr_ntoa_r(netif_ip4_gw(g_pnetif), gateway, sizeof(gateway));
	/* A dashboard manual DNS overrides DHCP; lwIP asks server 2 only after ~7 s on server 1. */
	ipaddr_ntoa_r(dns_getserver(0), dns1, sizeof(dns1));
	ipaddr_ntoa_r(dns_getserver(1), dns2, sizeof(dns2));
	Sys_XboxLog("Xbox net: ip=%s netmask=%s gateway=%s dns=%s,%s link=%s\n", ip, netmask,
		gateway, dns1, dns2, netif_is_link_up(g_pnetif) ? "up" : "down");
}

void Sys_XboxNetInit(void)
{
	int result;
	int waited;

	Sys_XboxLog("Xbox net: starting with the dashboard network settings\n");
	result = nxNetInit(NULL);
	if (result == -2 && netif_is_link_up(g_pnetif))
	{
		/* A System Link cable or a bare switch has no DHCP server, so use 169.254/16. */
		Sys_XboxLog("Xbox net: no DHCP reply, using link-local addressing\n");
		netifapi_dhcp_release_and_stop(g_pnetif);
		netifapi_autoip_start(g_pnetif);
		for (waited = 0; !autoip_supplied_address(g_pnetif) &&
			waited < XBOX_NET_AUTOIP_WAIT_MSEC; waited += XBOX_NET_POLL_MSEC)
			Sys_XboxSleep(XBOX_NET_POLL_MSEC);
	}
	else if (result == -2)
	{
		Sys_XboxLog("Xbox net: no link, DHCP waits for a cable\n");
	}
	else if (result != 0)
	{
		Sys_XboxLog("Xbox net: nxNetInit failed (%d), networking disabled\n", result);
		return;
	}

	xboxNetReady = 1;
	Sys_XboxNetLogAddress();
}

int Sys_XboxNetReady(void)
{
	return xboxNetReady;
}

/* lwIP getaddrinfo blocks its caller until DNS answers or times out (4 tries, ~7 s per server). */
static DWORD WINAPI Sys_XboxLookupThread(LPVOID param)
{
	xboxNetLookup_t *lookup = param;
	struct addrinfo hints;
	struct addrinfo *result = NULL;
	int error;

	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_INET;
	hints.ai_socktype = SOCK_DGRAM;
	error = getaddrinfo(lookup->host, NULL, &hints, &result);
	if (error == 0 && result) {
		lookup->ip = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
		Sys_XboxLog("Xbox net: DNS lookup of %s took %u ms\n", lookup->host,
			(unsigned int)(GetTickCount() - lookup->startedAt));
		lookup->state = XBOX_LOOKUP_DONE;
	} else {
		Sys_XboxLog("Xbox net: DNS lookup of %s failed (%d) after %u ms\n", lookup->host, error,
			(unsigned int)(GetTickCount() - lookup->startedAt));
		lookup->failedAt = GetTickCount();
		lookup->state = XBOX_LOOKUP_FAILED;
	}
	if (result)
		freeaddrinfo(result);
	return 0;
}

int Sys_XboxResolveAsync(const char *host, uint32_t *ip)
{
	xboxNetLookup_t *lookup = NULL;
	xboxNetLookup_t *freeSlot = NULL;
	HANDLE thread;
	int i;

	if (!xboxNetReady || strlen(host) >= sizeof(lookup->host))
		return 0;
	for (i = 0; i < XBOX_NET_MAX_LOOKUPS; i++) {
		if (xboxNetLookups[i].state == XBOX_LOOKUP_FREE) {
			if (!freeSlot)
				freeSlot = &xboxNetLookups[i];
		} else if (!strcmp(xboxNetLookups[i].host, host)) {
			lookup = &xboxNetLookups[i];
			break;
		}
	}

	if (lookup && lookup->state == XBOX_LOOKUP_FAILED &&
		GetTickCount() - lookup->failedAt >= XBOX_NET_LOOKUP_RETRY_MSEC) {
		freeSlot = lookup;
		lookup = NULL;
	}
	if (!lookup) {
		if (!freeSlot)
			return 0;
		strcpy(freeSlot->host, host);
		freeSlot->startedAt = GetTickCount();
		freeSlot->state = XBOX_LOOKUP_RUNNING;
		thread = CreateThread(NULL, XBOX_NET_LOOKUP_STACK, Sys_XboxLookupThread, freeSlot, 0, NULL);
		if (!thread) {
			Sys_XboxLog("Xbox net: no thread for the DNS lookup of %s\n", host);
			freeSlot->state = XBOX_LOOKUP_FREE;
			return 0;
		}
		CloseHandle(thread);
		return -1;
	}

	if (lookup->state == XBOX_LOOKUP_RUNNING)
		return -1;
	if (lookup->state == XBOX_LOOKUP_FAILED)
		return 0;
	*ip = lookup->ip;
	return 1;
}

int Sys_XboxNetAddress(uint32_t *ip, uint32_t *netmask)
{
	if (!xboxNetReady || ip4_addr_isany_val(*netif_ip4_addr(g_pnetif)))
		return 0;

	*ip = ip4_addr_get_u32(netif_ip4_addr(g_pnetif));
	*netmask = ip4_addr_get_u32(netif_ip4_netmask(g_pnetif));
	return 1;
}
