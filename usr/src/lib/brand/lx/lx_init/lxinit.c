/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2018 Joyent, Inc.
 * Copyright 2020 OmniOS Community Edition (OmniOSce) Association.
 * Copyright 2025 Edgecast Cloud LLC.
 */

/*
 * lxinit performs zone-specific initialization prior to handing control to the
 * guest Linux init.  This primarily consists of:
 *
 * - Starting ipmgmtd
 * - Configuring network interfaces
 * - Adding a default route
 * - Normalize netstack buffer sizes
 */

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <limits.h>
#include <net/if.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stropts.h>
#include <sys/ioccom.h>
#include <sys/stat.h>
#include <sys/systeminfo.h>
#include <sys/sockio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/varargs.h>
#include <sys/param.h>
#include <unistd.h>
#include <libintl.h>
#include <locale.h>
#include <libcustr.h>

#include <netinet/dhcp.h>
#include <inet/tunables.h>
#include <dhcpagent_util.h>
#include <dhcpagent_ipc.h>

#include <arpa/inet.h>
#include <net/route.h>
#include <libipadm.h>
#include <libzonecfg.h>
#include <libinetutil.h>
#include <sys/lx_brand.h>

#include "run_command.h"

static void lxi_err(char *msg, ...) __NORETURN;
static void lxi_err(char *msg, ...);

#define	IPMGMTD_PATH		"/lib/inet/ipmgmtd"
#define	IN_NDPD_PATH		"/usr/lib/inet/in.ndpd"
#define	HOOK_POSTNET_PATH	"/usr/lib/brand/lx/lx_hook_postnet"

#define	PREFIX_LOG_WARN	"lx_init warn: "
#define	PREFIX_LOG_ERR	"lx_init err: "

#define	RTMBUFSZ	(sizeof (struct rt_msghdr) + \
		(3 * sizeof (struct sockaddr_in)))

#define	NETSTACK_BUFSZ 524288

ipadm_handle_t iph;
boolean_t ipv6_enable = B_TRUE;

static void
lxi_err(char *msg, ...)
{
	char buf[1024];
	int len;
	va_list ap;

	va_start(ap, msg);
	/*LINTED*/
	len = vsnprintf(buf, sizeof (buf), msg, ap);
	va_end(ap);

	(void) write(1, PREFIX_LOG_ERR, strlen(PREFIX_LOG_ERR));
	(void) write(1, buf, len);
	(void) write(1, "\n", 1);

	/*
	 * Since a non-zero exit will cause the zone to reboot, a pause here
	 * will prevent a mis-configured zone from spinning in a reboot loop.
	 */
	(void) pause();
	exit(1);
	/*NOTREACHED*/
}

static void
lxi_warn(char *msg, ...)
{
	char buf[1024];
	int len;
	va_list ap;

	va_start(ap, msg);
	/*LINTED*/
	len = vsnprintf(buf, sizeof (buf), msg, ap);
	va_end(ap);

	(void) write(1, PREFIX_LOG_WARN, strlen(PREFIX_LOG_WARN));
	(void) write(1, buf, len);
	(void) write(1, "\n", 1);
}

static void
lxi_log_open()
{
	int fd = open("/dev/console", O_WRONLY);

	if (fd < 0) {
		/* hard to log at this point... */
		exit(1);
	} else if (fd != 1) {
		/*
		 * Use stdout as the log fd.  Init should start with no files
		 * open, so we should be required to perform this relocation
		 * every time.
		 */
		if (dup2(fd, 1) != 1) {
			exit(1);
		}
	}
}

static void
lxi_log_close()
{
	(void) close(0);
	(void) close(1);
}

static zone_dochandle_t
lxi_config_open()
{
	zoneid_t zoneid;
	char zonename[ZONENAME_MAX];
	zone_dochandle_t handle;
	zone_iptype_t iptype;
	int res;

	zoneid = getzoneid();
	if (getzonenamebyid(zoneid, zonename, sizeof (zonename)) < 0) {
		lxi_err("could not determine zone name");
	}

	if ((handle = zonecfg_init_handle()) == NULL)
		lxi_err("internal libzonecfg.so.1 error", 0);

	if ((res = zonecfg_get_handle(zonename, handle)) != Z_OK) {
		zonecfg_fini_handle(handle);
		lxi_err("could not locate zone config %d", res);
	}

	/*
	 * Only exclusive stack is supported.
	 */
	if (zonecfg_get_iptype(handle, &iptype) != Z_OK ||
	    iptype != ZS_EXCLUSIVE) {
		zonecfg_fini_handle(handle);
		lxi_err("lx zones do not support shared IP stacks");
	}

	return (handle);
}

static void
lxi_init(zone_dochandle_t handle)
{
	struct zone_attrtab a;
	char val[6];	/* Big enough for true/false */

	bzero(&a, sizeof (a));
	(void) strlcpy(a.zone_attr_name, "ipv6", sizeof (a.zone_attr_name));

	if (zonecfg_lookup_attr(handle, &a) == Z_OK &&
	    zonecfg_get_attr_string(&a, val, sizeof (val)) == Z_OK) {
		if (strcmp(val, "true") == 0)
			ipv6_enable = B_TRUE;
		else if (strcmp(val, "false") == 0)
			ipv6_enable = B_FALSE;
		else
			lxi_err("invalid value for 'ipv6' attribute");

		lxi_warn("IPv6 is %sabled by zone configuration",
		    ipv6_enable ? "en" : "dis");
	}
}

static int
zone_find_attr(struct zone_res_attrtab *attrs, const char *name,
    const char **result)
{
	while (attrs != NULL) {
		if (strncmp(attrs->zone_res_attr_name, name,
		    MAXNAMELEN) == 0) {
			*result = attrs->zone_res_attr_value;
			return (0);
		}
		attrs = attrs->zone_res_attr_next;
	}
	return (-1);
}

static void
lxi_svc_start(char *name, char *path, char *fmri)
{
	pid_t pid;
	int status;
	char *argv[] = {
		NULL,
		NULL
	};
	char *envp[] = {
		NULL,
		NULL
	};
	argv[0] = name;
	envp[0] = fmri;

	pid = fork();
	if (pid == -1) {
		lxi_err("fork() failed: %s", strerror(errno));
	}

	if (pid == 0) {
		/* child */
		const char *zroot = zone_get_nroot();
		char cmd[MAXPATHLEN];

		/*
		 * Construct the full path to the binary, including the native
		 * system root (e.g. "/native") if in use for this zone:
		 */
		(void) snprintf(cmd, sizeof (cmd), "%s%s", zroot != NULL ?
		    zroot : "", path);

		(void) execve(cmd, argv, envp);

		lxi_err("execve(%s) failed: %s", cmd, strerror(errno));
		/* NOTREACHED */
	}

	/* parent */
	while (wait(&status) != pid)
		;

	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) != 0) {
			lxi_err("%s[%d] exited: %d", name,
			    (int)pid, WEXITSTATUS(status));
		}
	} else if (WIFSIGNALED(status)) {
		lxi_err("%s[%d] died on signal: %d", name,
		    (int)pid, WTERMSIG(status));
	} else {
		lxi_err("%s[%d] failed in unknown way", name,
		    (int)pid);
	}
}

void
lxi_net_ipmgmtd_start()
{
	lxi_svc_start("ipmgmtd", IPMGMTD_PATH,
	    "SMF_FMRI=svc:/network/ip-interface-management:default");
}

void
lxi_net_ndpd_start()
{
	lxi_svc_start("in.ndpd", IN_NDPD_PATH,
	    "SMF_FMRI=svc:/network/routing/ndp:default");
}


static void
lxi_net_ipadm_open()
{
	ipadm_status_t status;

	if ((status = ipadm_open(&iph, IPH_LEGACY)) != IPADM_SUCCESS) {
		lxi_err("Error opening ipadm handle: %s",
		    ipadm_status2str(status));
	}
}

static void
lxi_net_ipadm_close()
{
	ipadm_close(iph);
}

/*
 * lxi_kern_release_cmp(zone_dochandle_t handle, const char *vers)
 *     Compare linux kernel version to the one set kernel-version attr in
 *     the zone document.
 *
 * Arguments:
 *     handle  zone document handle for xml properties parsing.
 *     *vers   kernel version in major.minor.patch format to compare with zone
 *             defined kernel-version attribute property.
 * Returns:
 *
 *    > 0 if zone kernel-version attribute > *vers
 *    < 0 if zone kernel-version attribute < *vers
 *    = 0 if zone kernel-version attribute = *vers
 *
 * Notes:
 *     In case of an error the lxi_err will exit the program.
 */
static int
lxi_kern_release_cmp(zone_dochandle_t handle, const char *vers)
{
	struct zone_attrtab attrtab;
	int zvers[3] = {0, 0, 0};
	int cvers[3] = {0, 0, 0};
	int res = 0;
	int i = 0;

	if (handle == NULL) {
		lxi_err("%s zone handle is NULL", __FUNCTION__);
	}

	if (vers == NULL) {
		lxi_err("%s kernel version is NULL", __FUNCTION__);
	}

	bzero(&attrtab, sizeof (attrtab));
	(void) strlcpy(attrtab.zone_attr_name, "kernel-version",
	    sizeof (attrtab.zone_attr_name));

	if ((res = zonecfg_lookup_attr(handle, &attrtab)) == Z_OK) {
		(void) sscanf(attrtab.zone_attr_value, "%d.%d.%d", &zvers[0],
		    &zvers[1], &zvers[2]);
		(void) sscanf(vers, "%d.%d.%d", &cvers[0], &cvers[1],
		    &cvers[2]);
		for (i = 0; i < 3; i++) {
			if (zvers[i] > cvers[i]) {
				return (1);
			} else if (zvers[i] < cvers[i]) {
				return (-1);
			}
		}
	} else {
		lxi_err("%s kernel-version zonecfg_lookup_attr: %s\n",
		    __FUNCTION__, zonecfg_strerror(res));
	}
	return (0);
}
/*
 * lxi_normalize_protocols(zone_dochandle_t handle)
 *     Sets all four netstack protocols recv/send buffers to
 *     the same value (currently 1MiB), and max_buf to values expected by Linux
 *     applications.
 *
 * Arguments:
 *     handle    zone document handler for xml properties parsing.
 *     iph       ipadm handle for updating netstack protocol's properties.
 *
 * Returns:
 *     No return value.
 *
 * Notes:
 *
 * As part of adding support for /proc/sys/net/core/{r|w}mem_{default|max}
 * kernel tunables, we need to normalize values for the four protocols in the
 * netstack in order to report more Linux-like uniform values for the netstack
 * of this zone.
 * More information in usr/src/uts/common/brand/lx/procfs/lx_prvnops.c
 */
static void
lxi_normalize_protocols(zone_dochandle_t handle, ipadm_handle_t iph)
{
	uint_t proto_entries[] = {
		MOD_PROTO_TCP,
		MOD_PROTO_UDP,
		MOD_PROTO_SCTP,
		MOD_PROTO_RAWIP
	};
	ipadm_status_t status;
	size_t proto_cnt, i;
	char val_max[16];
	uint32_t max_buf;
	char val[16];

	if (iph == NULL) {
		lxi_err("%s ipadm handle is NULL", __FUNCTION__);
	}
	/*
	 * Prior to kernel 3.4, Linux defaulted to a max of 4MB for both the
	 * tcp_rmem and tcp_wmem tunables. Kernels since then have increased the
	 * tcp_rmem default max to 6MB. Today kernels since version 6.9 this
	 * value is dynamically assigned more information in
	 * linux/net/ipv4/tcp.c.
	 * Prior to OS-6096, as the TCP buffer sizing in illumos is smaller
	 * than Linux LX Branded zones experience setsockopt() errors, this is
	 * replicated here.
	 *
	 * We are not emulating dynamic TCP buffer sizing because the computed
	 * value  would not match exactly and thus adds little value. If needed,
	 * buffer sizes can be adjusted with ipadm(8), or via the kernel
	 * tunables  /proc/sys/net/core/{r|w}mem_{default|max}.
	 * These tunables are not as fine-grained as ipadm.
	 */
	if (lxi_kern_release_cmp(handle, "3.4.0") < 0) {
		max_buf = 4 * 1024 * 1024;
	} else {
		max_buf = 6 * 1024 * 1024;
	}
	/*
	 * Normalize recv/send buffers to 1MiB and max_buf to Linux expected
	 * default values defined by kernel version.
	 */
	(void) snprintf(val, sizeof (val), "%u", NETSTACK_BUFSZ * 2);
	(void) snprintf(val_max, sizeof (val_max), "%u", max_buf);

	proto_cnt = sizeof (proto_entries)/ sizeof (proto_entries[0]);

	/*
	 *  To avoid ERANGE errors, max_buf is updated first then the
	 *  rest of the protocols.
	 *  In case of a failure, we log the error and let the lx zone continue
	 *  it's boot process. Administrators could still setup the protocols
	 *  buffers if needed later via ipadm(8).
	 */
	for (i = 0; i < proto_cnt; i++) {
		if ((status = ipadm_set_prop(iph, "max_buf", val_max,
		    proto_entries[i], IPADM_OPT_ACTIVE)) != IPADM_SUCCESS) {
			lxi_warn("%s buf ipadm_set_prop error %d index %d: %s",
			    __FUNCTION__, status, i, ipadm_status2str(status));
		}
		if ((status = ipadm_set_prop(iph, "send_buf", val,
		    proto_entries[i], IPADM_OPT_ACTIVE)) != IPADM_SUCCESS) {
			lxi_warn("%s buf ipadm_set_prop error %d index %d: %s",
			    __FUNCTION__, status, i, ipadm_status2str(status));
		}
		if ((status = ipadm_set_prop(iph, "recv_buf", val,
		    proto_entries[i], IPADM_OPT_ACTIVE)) != IPADM_SUCCESS) {
			lxi_warn("%s buf ipadm_set_prop error %d index %d: %s",
			    __FUNCTION__, status, i, ipadm_status2str(status));
		}
	}
}

void
lxi_net_plumb(const char *iface)
{
	ipadm_status_t status;
	char ifbuf[LIFNAMSIZ];

	/* ipadm_create_if stomps on ifbuf, so create a copy: */
	(void) strncpy(ifbuf, iface, sizeof (ifbuf));

	status = ipadm_create_if(iph, ifbuf, AF_INET, IPADM_OPT_ACTIVE);
	if (status != IPADM_SUCCESS && status != IPADM_IF_EXISTS) {
		lxi_err("ipadm_create_if error %d: %s/v4: %s",
		    status, iface, ipadm_status2str(status));
	}

	if (ipv6_enable) {
		status = ipadm_create_if(iph, ifbuf, AF_INET6,
		    IPADM_OPT_ACTIVE);
		if (status != IPADM_SUCCESS && status != IPADM_IF_EXISTS) {
			lxi_err("ipadm_create_if error %d: %s/v6: %s",
			    status, iface, ipadm_status2str(status));
		}
	}
}

static int
lxi_getif(int af, char *iface, int len, boolean_t first_ipv4_configured)
{
	struct lifreq lifr;
	int s = socket(af, SOCK_DGRAM, 0);
	if (s < 0) {
		lxi_warn("socket error %d: bringing up %s: %s",
		    errno, iface, strerror(errno));
		return (-1);
	}

	/*
	 * We need a new logical interface for every IP address we add, except
	 * for the very first IPv4 address.
	 */
	if (af == AF_INET6 || first_ipv4_configured) {
		(void) strncpy(lifr.lifr_name, iface, sizeof (lifr.lifr_name));
		(void) memset(&lifr.lifr_addr, 0, sizeof (lifr.lifr_addr));
		if (ioctl(s, SIOCLIFADDIF, (caddr_t)&lifr) < 0) {
			if (close(s) != 0) {
				lxi_warn("failed to close socket: %s\n",
				    strerror(errno));
			}
			return (-1);
		}
		(void) strncpy(iface, lifr.lifr_name, len);
	}

	if (close(s) != 0) {
		lxi_warn("failed to close socket: %s\n",
		    strerror(errno));
	}
	return (0);
}

static int
lxi_iface_ip(const char *origiface, const char *addr,
    boolean_t *first_ipv4_configured)
{
	static int addrnum = 0;
	ipadm_status_t status;
	ipadm_addrobj_t ipaddr = NULL;
	char iface[LIFNAMSIZ];
	char aobjname[IPADM_AOBJSIZ];
	int af, err = 0;

	(void) strncpy(iface, origiface, sizeof (iface));

	af = strstr(addr, ":") == NULL ? AF_INET : AF_INET6;
	if (lxi_getif(af, iface, sizeof (iface), *first_ipv4_configured) != 0) {
		lxi_warn("failed to create new logical interface "
		    "on %s: %s", origiface, strerror(errno));
		return (-1);
	}

	(void) snprintf(aobjname, IPADM_AOBJSIZ, "%s/addr%d", iface,
	    addrnum++);

	if ((status = ipadm_create_addrobj(IPADM_ADDR_STATIC, aobjname,
	    &ipaddr)) != IPADM_SUCCESS) {
		lxi_warn("ipadm_create_addrobj error %d: addr %s, "
		    "interface %s: %s\n", status, addr, iface,
		    ipadm_status2str(status));
		return (-2);
	}

	if ((status = ipadm_set_addr(ipaddr, addr, AF_UNSPEC))
	    != IPADM_SUCCESS) {
		lxi_warn("ipadm_set_addr error %d: addr %s"
		    ", interface %s: %s\n", status, addr,
		    iface, ipadm_status2str(status));
		err = -3;
		goto done;
	}

	if ((status = ipadm_create_addr(iph, ipaddr,
	    IPADM_OPT_ACTIVE | IPADM_OPT_UP)) != IPADM_SUCCESS) {
		lxi_warn("ipadm_create_addr error for %s: %s\n", iface,
		    ipadm_status2str(status));
		err = -4;
		goto done;
	}

	if (af == AF_INET) {
		*first_ipv4_configured = B_TRUE;
	}

done:
	ipadm_destroy_addrobj(ipaddr);
	return (err);
}

static int
lxi_iface_dhcp(const char *origiface, boolean_t *first_ipv4_configured)
{
	dhcp_ipc_request_t *dhcpreq = NULL;
	dhcp_ipc_reply_t *dhcpreply = NULL;
	int err = 0, timeout = 5;
	char iface[LIFNAMSIZ];

	(void) strncpy(iface, origiface, sizeof (iface));

	if (lxi_getif(AF_INET, iface, sizeof (iface), *first_ipv4_configured)
	    != 0) {
		lxi_warn("failed to create new logical interface "
		    "on %s: %s", origiface, strerror(errno));
		return (-1);
	}

	if (dhcp_start_agent(timeout) != 0) {
		lxi_err("Failed to start dhcpagent\n");
		/* NOTREACHED */
	}

	dhcpreq = dhcp_ipc_alloc_request(DHCP_START, iface,
	    NULL, 0, DHCP_TYPE_NONE);
	if (dhcpreq == NULL) {
		lxi_warn("Unable to allocate memory "
		    "to start DHCP on %s\n", iface);
		return (-1);
	}

	err = dhcp_ipc_make_request(dhcpreq, &dhcpreply, timeout);
	if (err != 0) {
		free(dhcpreq);
		lxi_warn("Failed to start DHCP on %s: %s\n", iface,
		    dhcp_ipc_strerror(err));
		return (-1);
	}
	err = dhcpreply->return_code;
	if (err != 0) {
		lxi_warn("Failed to start DHCP on %s: %s\n", iface,
		    dhcp_ipc_strerror(err));
		goto done;
	}

	*first_ipv4_configured = B_TRUE;

done:
	free(dhcpreq);
	free(dhcpreply);
	return (err);
}

/*
 * Initialize an IPv6 link-local address on a given interface
 */
static int
lxi_iface_ipv6_link_local(const char *iface)
{
	struct lifreq lifr;
	int s;

	s = socket(AF_INET6, SOCK_DGRAM, 0);
	if (s == -1) {
		lxi_warn("socket error %d: bringing up %s: %s",
		    errno, iface, strerror(errno));
	}

	(void) strncpy(lifr.lifr_name, iface, sizeof (lifr.lifr_name));
	if (ioctl(s, SIOCGLIFFLAGS, (caddr_t)&lifr) < 0) {
		lxi_warn("SIOCGLIFFLAGS error %d: bringing up %s: %s",
		    errno, iface, strerror(errno));
		return (-1);
	}

	lifr.lifr_flags |= IFF_UP;
	if (ioctl(s, SIOCSLIFFLAGS, (caddr_t)&lifr) < 0) {
		lxi_warn("SIOCSLIFFLAGS error %d: bringing up %s: %s",
		    errno, iface, strerror(errno));
		return (-1);
	}

	(void) close(s);
	return (0);
}

static int
lxi_iface_gateway(const char *iface, const char *dst, int dstpfx,
    const char *gwaddr)
{
	int idx, len, sockfd;
	/* For lint-happy alignment, use a uint32_t array... */
	uint32_t rtbuf[RTMBUFSZ / sizeof (uint32_t)];
	struct rt_msghdr *rtm = (struct rt_msghdr *)rtbuf;
	struct sockaddr_in *dst_sin = (struct sockaddr_in *)(rtm + 1);
	struct sockaddr_in *gw_sin = (struct sockaddr_in *)(dst_sin + 1);
	struct sockaddr_in *netmask_sin = (struct sockaddr_in *)(gw_sin + 1);

	(void) bzero(rtm, RTMBUFSZ);
	rtm->rtm_addrs = RTA_DST | RTA_GATEWAY | RTA_NETMASK;
	rtm->rtm_flags = RTF_UP | RTF_STATIC | RTF_GATEWAY;
	rtm->rtm_msglen = sizeof (rtbuf);
	rtm->rtm_pid = getpid();
	rtm->rtm_type = RTM_ADD;
	rtm->rtm_version = RTM_VERSION;


	/*
	 * The destination and netmask components have already been zeroed,
	 * which represents the default gateway.  If we were passed a more
	 * specific destination network, use that instead.
	 */
	dst_sin->sin_family = AF_INET;
	netmask_sin->sin_family = AF_INET;
	if (dst != NULL) {
		struct sockaddr *mask = (struct sockaddr *)netmask_sin;

		if ((inet_pton(AF_INET, dst, &(dst_sin->sin_addr))) != 1 ||
		    plen2mask(dstpfx, AF_INET, mask) != 0) {
			lxi_warn("bad destination network %s/%d: %s", dst,
			    dstpfx, strerror(errno));
			return (-1);
		}
	}

	if ((inet_pton(AF_INET, gwaddr, &(gw_sin->sin_addr))) != 1) {
		lxi_warn("bad gateway %s: %s", gwaddr, strerror(errno));
		return (-1);
	}

	if (iface != NULL) {
		if ((idx = if_nametoindex(iface)) == 0) {
			lxi_warn("unable to get interface index for %s: %s\n",
			    iface, strerror(errno));
			return (-1);
		}
		rtm->rtm_index = idx;
	}

	if ((sockfd = socket(PF_ROUTE, SOCK_RAW, AF_INET)) < 0) {
		lxi_warn("socket(PF_ROUTE): %s\n", strerror(errno));
		return (-1);
	}

	if ((len = write(sockfd, rtbuf, rtm->rtm_msglen)) < 0) {
		lxi_warn("could not write rtmsg: %s", strerror(errno));
		(void) close(sockfd);
		return (-1);
	} else if (len < rtm->rtm_msglen) {
		lxi_warn("write() rtmsg incomplete");
		(void) close(sockfd);
		return (-1);
	}

	(void) close(sockfd);
	return (0);
}


static void
lxi_net_loopback()
{
	const char *iface = "lo0";
	boolean_t first_ipv4_configured = B_FALSE;

	lxi_net_plumb(iface);
	(void) lxi_iface_ip(iface, "127.0.0.1/8", &first_ipv4_configured);
	if (ipv6_enable)
		(void) lxi_iface_ipv6_link_local(iface);
}

static void
lxi_net_setup(zone_dochandle_t handle)
{
	struct zone_nwiftab lookup;
	boolean_t do_addrconf = B_FALSE;

	if (zonecfg_setnwifent(handle) != Z_OK)
		return;
	while (zonecfg_getnwifent(handle, &lookup) == Z_OK) {
		const char *iface = lookup.zone_nwif_physical;
		struct zone_res_attrtab *attrs = lookup.zone_nwif_attrp;
		const char *ipaddrs, *primary, *gateway, *ipv6attr;
		char ipaddrs_copy[MAXNAMELEN], /* cidraddr[BUFSIZ], */
		    *ipaddr, *tmp, *lasts;
		boolean_t first_ipv4_configured = B_FALSE;
		boolean_t *ficp = &first_ipv4_configured;
		boolean_t no_zonecfg, ipv6 = ipv6_enable;

		/*
		 * Regardless of whether we're configured in zonecfg(8), or
		 * configured by other means, make sure we plumb every
		 * physical=<foo> for IPv4 and IPv6.
		 */
		lxi_net_plumb(iface);

		/*
		 * If there is a configured allowed-address, then use
		 * that to determine the single address for this interface.
		 * zoneadmd in the GZ will have taken care of setting the
		 * protection and allowed-ips link property on the interface
		 * anyway so no other address can be configured.
		 */
		if (strlen(lookup.zone_nwif_allowed_address) > 0) {
			ipaddrs = lookup.zone_nwif_allowed_address;
			no_zonecfg = B_FALSE;
		} else if (zone_find_attr(attrs, "ips", &ipaddrs) != 0) {
			/*
			 * Do not panic.  This interface has no in-zonecfg(8)
			 * configuration.  We keep a warning around for now.
			 */
			lxi_warn("Could not find zonecfg(8) network "
			    "configuration for the %s interface", iface);
			no_zonecfg = B_TRUE;
		} else {
			no_zonecfg = B_FALSE;
		}

		if (zone_find_attr(attrs, "ipv6", &ipv6attr) == 0) {
			if (strcmp(ipv6attr, "true") == 0) {
				if (!ipv6) {
					lxi_err("Cannot enable ipv6 for an "
					    "interface when it is disabled for "
					    "the zone.");
				}
			} else if (strcmp(ipv6attr, "false") == 0) {
				ipv6 = B_FALSE;
			} else {
				lxi_err("invalid value for 'ipv6' attribute");
			}
		}

		if (ipv6 && lxi_iface_ipv6_link_local(iface) != 0) {
			lxi_warn("unable to bring up link-local address on "
			    "interface %s", iface);
		}

		/*
		 * Every thing else below only happens if we have zonecfg(8)
		 * network configuration.
		 */
		if (no_zonecfg)
			continue;

		/*
		 * If we're going to be doing DHCP, we have to do it first
		 * since dhcpagent doesn't like to operate on non-zero logical
		 * interfaces.
		 */
		if (strstr(ipaddrs, "dhcp") != NULL &&
		    lxi_iface_dhcp(iface, ficp) != 0) {
			lxi_warn("Failed to start DHCP on %s\n", iface);
		}

		/*
		 * Copy the ipaddrs string, since strtok_r will write NUL
		 * characters into it.
		 */
		(void) strlcpy(ipaddrs_copy, ipaddrs, MAXNAMELEN);
		tmp = ipaddrs_copy;

		/*
		 * Iterate over each IP and then set it up on the interface.
		 */
		while ((ipaddr = strtok_r(tmp, ",", &lasts)) != NULL) {
			tmp = NULL;
			if (strcmp(ipaddr, "addrconf") == 0) {
				do_addrconf = B_TRUE;
			} else if (strcmp(ipaddr, "dhcp") == 0) {
				continue;
			} else if (lxi_iface_ip(iface, ipaddr, ficp) < 0) {
				lxi_warn("Unable to add new IP address (%s) "
				    "to interface %s", ipaddr, iface);
			}
		}

		/*
		 * If a default router is set for this interface, use it.
		 * This will have been configured in conjunction with
		 * allowed-address.
		 */

		gateway = NULL;
		if (strlen(lookup.zone_nwif_defrouter) > 0) {
			gateway = lookup.zone_nwif_defrouter;
		} else if (zone_find_attr(attrs, "primary", &primary) == 0 &&
		    strncmp(primary, "true", MAXNAMELEN) == 0) {
			if (zone_find_attr(attrs, "gateway", &gateway) != 0)
				gateway = NULL;
		}

		if (gateway != NULL &&
		    lxi_iface_gateway(iface, NULL, 0, gateway) != 0) {
			lxi_err("default route on %s -> %s failed",
			    iface, gateway);
		}
	}

	if (do_addrconf) {
		lxi_net_ndpd_start();
	}


	(void) zonecfg_endnwifent(handle);
}

static void
lxi_net_static_route(const char *line)
{
	/*
	 * Each static route line is a string of the form:
	 *
	 *	"10.77.77.2|10.1.1.0/24|false"
	 *
	 * i.e. gateway address, destination network, and whether this is
	 * a "link local" route or a next hop route.
	 */
	custr_t *cu = NULL;
	char *gw = NULL, *dst = NULL;
	int pfx = -1;
	int i;

	if (custr_alloc(&cu) != 0) {
		lxi_err("custr_alloc failure");
	}

	for (i = 0; line[i] != '\0'; i++) {
		if (gw == NULL) {
			if (line[i] == '|') {
				if ((gw = strdup(custr_cstr(cu))) == NULL) {
					lxi_err("strdup failure");
				}
				custr_reset(cu);
			} else {
				if (custr_appendc(cu, line[i]) != 0) {
					lxi_err("custr_appendc failure");
				}
			}
			continue;
		}

		if (dst == NULL) {
			if (line[i] == '/') {
				if ((dst = strdup(custr_cstr(cu))) == NULL) {
					lxi_err("strdup failure");
				}
				custr_reset(cu);
			} else {
				if (custr_appendc(cu, line[i]) != 0) {
					lxi_err("custr_appendc failure");
				}
			}
			continue;
		}

		if (pfx == -1) {
			if (line[i] == '|') {
				pfx = atoi(custr_cstr(cu));
				custr_reset(cu);
			} else {
				if (custr_appendc(cu, line[i]) != 0) {
					lxi_err("custr_appendc failure");
				}
			}
			continue;
		}

		if (custr_appendc(cu, line[i]) != 0) {
			lxi_err("custr_appendc failure");
		}
	}

	/*
	 * We currently only support "next hop" routes, so ensure that
	 * "linklocal" is false:
	 */
	if (strcmp(custr_cstr(cu), "false") != 0) {
		lxi_warn("invalid static route: %s", line);
	}

	if (lxi_iface_gateway(NULL, dst, pfx, gw) != 0) {
		lxi_err("failed to add route: %s/%d -> %s", dst, pfx, gw);
	}

	custr_free(cu);
	free(gw);
	free(dst);
}

static void
lxi_net_static_routes(void)
{
	const char *cmd = "/native/usr/lib/brand/lx/routeinfo";
	char *const argv[] = { "routeinfo", NULL };
	char *const envp[] = { NULL };
	int code;
	struct stat st;
	char errbuf[512];

	if (stat(cmd, &st) != 0 || !S_ISREG(st.st_mode)) {
		/*
		 * This binary is (potentially) shipped from another
		 * consolidation.  If it does not exist, then the platform does
		 * not currently support static routes for LX-branded zones.
		 */
		return;
	}

	/*
	 * Run the command, firing the callback for each line that it
	 * outputs.  When this function returns, static route processing
	 * is complete.
	 */
	if (run_command(cmd, argv, envp, errbuf, sizeof (errbuf),
	    lxi_net_static_route, &code) != 0 || code != 0) {
		lxi_err("failed to run \"%s\": %s", cmd, errbuf);
	}
}

static void
lxi_config_close(zone_dochandle_t handle)
{
	zonecfg_fini_handle(handle);
}

static void
lxi_hook_postnet()
{
	char cmd[MAXPATHLEN];
	const char *zroot = zone_get_nroot();
	pid_t pid;
	int status;

	(void) snprintf(cmd, sizeof (cmd), "%s%s", zroot, HOOK_POSTNET_PATH);
	if (access(cmd, X_OK) != 0) {
		/* If no suitable script is present, soldier on. */
		return;
	}

	if ((pid = fork()) < 0) {
		lxi_err("fork() failed: %s", strerror(errno));
	}
	if (pid == 0) {
		char *argv[] = { NULL, NULL };
		char *const envp[] = { NULL };
		argv[0] = cmd;

		/* wire up stderr first, in case the hook wishes to use it */
		if (dup2(1, 2) < 0) {
			lxi_err("dup2() failed: %s", cmd, strerror(errno));
		}

		/* child executes the hook */
		(void) execve(cmd, argv, envp);

		/*
		 * Since this is running as root, access(2) is less strict than
		 * necessary to ensure a successful exec.  If the permissions
		 * on the hook are busted, ignore the failure and move on.
		 */
		if (errno == EACCES) {
			exit(0);
		}

		lxi_err("execve(%s) failed: %s", cmd, strerror(errno));
		/* NOTREACHED */
	}

	/* Parent waits for the hook to complete */
	while (wait(&status) != pid) {
		;
	}
	if (WIFEXITED(status)) {
		if (WEXITSTATUS(status) != 0) {
			lxi_err("%s[%d] exited: %d", cmd, (int)pid,
			    WEXITSTATUS(status));
		}
	} else if (WIFSIGNALED(status)) {
		lxi_err("%s[%d] died on signal: %d", cmd, (int)pid,
		    WTERMSIG(status));
	} else {
		lxi_err("%s[%d] failed in unknown way", cmd, (int)pid);
	}
}

static void
lxi_init_exec(char **argv)
{
	const char *cmd = "/sbin/init";
	char *const envp[] = { "container=zone", NULL };
	int e;

	argv[0] = "init";

	/*
	 * systemd uses the 'container' env var to determine it is running
	 * inside a container. It only supports a few well-known types and
	 * treats anything else as 'other' but this is enough to make it
	 * behave better inside a zone. See 'detect_container' in systemd.
	 */
	(void) execve(cmd, argv, envp);
	e = errno;

	/*
	 * Because stdout was closed prior to exec, it must be opened again in
	 * the face of failure to log the error.
	 */
	lxi_log_open();
	lxi_err("execve(%s) failed: %s", cmd, strerror(e));
}

/*ARGSUSED*/
int
main(int argc, char *argv[])
{
	zone_dochandle_t handle;

	lxi_log_open();

	lxi_net_ipmgmtd_start();
	lxi_net_ipadm_open();
	handle = lxi_config_open();
	lxi_init(handle);
	lxi_net_loopback();
	lxi_net_setup(handle);

	lxi_normalize_protocols(handle, iph);

	lxi_config_close(handle);

	lxi_net_static_routes();

	lxi_net_ipadm_close();

	lxi_hook_postnet();

	lxi_log_close();

	lxi_init_exec(argv);

	/* NOTREACHED */
	return (0);
}
