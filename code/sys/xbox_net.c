/* Copyright (C) 1999-2005 Id Software, Inc. Part of Quake III Arena source code. */

#include "sys_xbox.h"

#include <nxdk/net.h>
#include <lwip/autoip.h>
#include <lwip/dhcp.h>
#include <lwip/ip4_addr.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>

/* RFC 3927 probes plus the announce delay take up to about 9 s. */
#define XBOX_NET_AUTOIP_WAIT_MSEC 12000
#define XBOX_NET_POLL_MSEC 250

extern struct netif *g_pnetif;

static int xboxNetReady;

static void Sys_XboxNetLogAddress(void)
{
	char ip[IP4ADDR_STRLEN_MAX];
	char netmask[IP4ADDR_STRLEN_MAX];
	char gateway[IP4ADDR_STRLEN_MAX];

	ip4addr_ntoa_r(netif_ip4_addr(g_pnetif), ip, sizeof(ip));
	ip4addr_ntoa_r(netif_ip4_netmask(g_pnetif), netmask, sizeof(netmask));
	ip4addr_ntoa_r(netif_ip4_gw(g_pnetif), gateway, sizeof(gateway));
	Sys_XboxLog("Xbox net: ip=%s netmask=%s gateway=%s link=%s\n", ip, netmask, gateway,
		netif_is_link_up(g_pnetif) ? "up" : "down");
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

int Sys_XboxNetAddress(uint32_t *ip, uint32_t *netmask)
{
	if (!xboxNetReady || ip4_addr_isany_val(*netif_ip4_addr(g_pnetif)))
		return 0;

	*ip = ip4_addr_get_u32(netif_ip4_addr(g_pnetif));
	*netmask = ip4_addr_get_u32(netif_ip4_netmask(g_pnetif));
	return 1;
}
