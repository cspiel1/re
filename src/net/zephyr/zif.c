/**
 * @file zephyr/zif.c  Zephyr network interface code
 *
 * Copyright (C) 2023 Christian Spielberger
 */
#include <string.h>
#include <errno.h>
#include <re_types.h>
#include <re_fmt.h>
#include <re_mbuf.h>
#include <re_sa.h>
#include <re_net.h>

#include <zephyr/net/net_if.h>
#include <zephyr/net/net_linkaddr.h>


#define DEBUG_MODULE "zephyrif"
#define DEBUG_LEVEL 5
#include <re_dbg.h>


/**
 * Get IP address for a given network interface
 *
 * @param ifname  Network interface name
 * @param af      Address Family
 * @param ip      Returned IP address
 *
 * @return 0 if success, otherwise errorcode
 *
 * @deprecated Works for IPv4 only
 */
int net_if_getaddr4(const char *ifname, int af, struct sa *ip)
{
	(void)ifname;
	(void)af;
	(void)ip;

	return EAFNOSUPPORT;
}


/**
 * Enumerate all network interfaces
 *
 * @param ifh Interface handler
 * @param arg Handler argument
 *
 * @return 0 if success, otherwise errorcode
 *
 * @deprecated Works for IPv4 only
 */
int net_if_list(net_ifaddr_h *ifh, void *arg)
{
	struct sa sa;

	if (!ifh)
		return EINVAL;

	struct net_if *iface = net_if_get_default();
	if (!iface)
		return EADDRNOTAVAIL;

	struct net_linkaddr *ll_addr = net_if_get_link_addr(iface);
	if (!ll_addr)
		return EADDRNOTAVAIL;

	struct sockaddr saddr;
	saddr.sa_family = AF_INET;
	memcpy(saddr.data, ll_addr->addr,
	       min(ll_addr->len, sizeof(saddr.data)));
	sa_set_sa(&sa, &saddr);

	(void)ifh(iface->if_dev->dev->name, &sa, arg);
	return 0;
}
