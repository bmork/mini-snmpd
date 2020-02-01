/* Linux backend
 *
 * Copyright (C) 2008-2010  Robert Ernst <robert.ernst@linux-solutions.at>
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 *
 * See COPYING for GPL licensing information.
 */
#ifdef __linux__

#include <sys/sysinfo.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/vfs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <math.h>

#include "mini_snmpd.h"


/* We need the uptime in 1/100 seconds, so we can't use sysinfo() */
unsigned int get_process_uptime(void)
{
	static unsigned int uptime_start = 0;
	unsigned int uptime_now = get_system_uptime();

	if (uptime_start == 0)
		uptime_start = uptime_now;

	return uptime_now - uptime_start;
}

/* We need the uptime in 1/100 seconds, so we can't use sysinfo() */
unsigned int get_system_uptime(void)
{
	char buf[128];

	if (read_file("/proc/uptime", buf, sizeof(buf)) == -1)
		return -1;

	return (unsigned int)(atof(buf) * 100);
}

void get_loadinfo(loadinfo_t *loadinfo)
{
	char buf[128];
	char *ptr;
	int i;

	memset(loadinfo, 0, sizeof(loadinfo_t));
	if (read_file("/proc/loadavg", buf, sizeof(buf)) == -1)
		return;

	ptr = buf;
	for (i = 0; i < 3; i++) {
		while (isspace(*ptr))
			ptr++;

		if (*ptr == 0)
			continue;

		loadinfo->avg[i] = strtod(ptr, &ptr) * 100;
	}
}

void get_meminfo(meminfo_t *meminfo)
{
	field_t fields[] = {
		{ "MemTotal",  1, { &meminfo->total   }},
		{ "MemFree",   1, { &meminfo->free    }},
		{ "MemShared", 1, { &meminfo->shared  }},
		{ "Buffers",   1, { &meminfo->buffers }},
		{ "Cached",    1, { &meminfo->cached  }},
		{ NULL }
	};

	memset(meminfo, 0, sizeof(meminfo_t));
	parse_file("/proc/meminfo", fields, 255);
}

void get_cpuinfo(cpuinfo_t *cpuinfo)
{
	field_t fields[] = {
		{ "cpu ",  4, { &cpuinfo->user, &cpuinfo->nice, &cpuinfo->system, &cpuinfo->idle }},
		{ "intr ", 1, { &cpuinfo->irqs   }},
		{ "ctxt ", 1, { &cpuinfo->cntxts }},
		{ NULL }
	};

	memset(cpuinfo, 0, sizeof(cpuinfo_t));
	parse_file("/proc/stat", fields, 255);
}

void get_diskinfo(diskinfo_t *diskinfo)
{
	struct statfs fs;
	size_t i;

	memset(diskinfo, 0, sizeof(*diskinfo));
	for (i = 0; i < g_disk_list_length; i++) {
		if (statfs(g_disk_list[i], &fs) == -1)
			continue;

		diskinfo->total[i] = ((float)fs.f_blocks * fs.f_bsize) / 1024;
		diskinfo->free[i]  = ((float)fs.f_bfree  * fs.f_bsize) / 1024;
		diskinfo->used[i]  = ((float)(fs.f_blocks - fs.f_bfree) * fs.f_bsize) / 1024;
		diskinfo->blocks_used_percent[i] =
			((float)(fs.f_blocks - fs.f_bfree) * 100 + fs.f_blocks - 1) / fs.f_blocks;
		if (fs.f_files <= 0)
			diskinfo->inodes_used_percent[i] = 0;
		else
			diskinfo->inodes_used_percent[i] =
				((float)(fs.f_files - fs.f_ffree) * 100 + fs.f_files - 1) / fs.f_files;
	}
}

void get_netinfo(netinfo_t *netinfo)
{
	struct ifreq ifreq;
	field_t fields[MAX_NR_INTERFACES + 1];
	size_t i;
	int sd;

	memset(netinfo, 0, sizeof(*netinfo));

	sd = socket(AF_INET, SOCK_DGRAM, 0);
	if (-1 == sd)
		return;

	memset(fields, 0, (MAX_NR_INTERFACES + 1) * sizeof(field_t));
	for (i = 0; i < g_interface_list_length; i++) {
		fields[i].prefix    = g_interface_list[i];
		fields[i].len       = 12;
		fields[i].value[0]  = &netinfo->rx_bytes[i];
		fields[i].value[1]  = &netinfo->rx_packets[i];
		fields[i].value[2]  = &netinfo->rx_errors[i];
		fields[i].value[3]  = &netinfo->rx_drops[i];
		fields[i].value[8]  = &netinfo->tx_bytes[i];
		fields[i].value[9]  = &netinfo->tx_packets[i];
		fields[i].value[10] = &netinfo->tx_errors[i];
		fields[i].value[11] = &netinfo->tx_drops[i];

		if (-1 == read_file_value(&netinfo->if_mtu[i], "/sys/class/net/%s/mtu", g_interface_list[i]))
			netinfo->if_mtu[i] = 1500; /* Fallback */

		snprintf(ifreq.ifr_name, sizeof(ifreq.ifr_name), "%s", g_interface_list[i]);
		if (ioctl(sd, SIOCGIFFLAGS, &ifreq) == -1) {
			netinfo->status[i] = 4;
			continue;
		}

		if (ifreq.ifr_flags & IFF_UP)
			netinfo->status[i] = (ifreq.ifr_flags & IFF_RUNNING) ? 1 : 7;
		else
			netinfo->status[i] = 2;

		if (ifreq.ifr_flags & IFF_POINTOPOINT)
			netinfo->if_type[i] = 23; /* ppp(23) */
		else if (ifreq.ifr_flags & IFF_LOOPBACK)
			netinfo->if_type[i] = 24; /* softwareLoopback(24) */
		else
			netinfo->if_type[i] = 6; /* ethernetCsmacd(6) */

		if (ioctl(sd, SIOCGIFHWADDR, &ifreq) == -1)
			continue;

		memcpy(&netinfo->mac_addr[i][0], &ifreq.ifr_hwaddr.sa_data[0], 6);
	}

	parse_file("/proc/net/dev", fields, 255);
	close(sd);
}

#endif /* __linux__ */

/* vim: ts=4 sts=4 sw=4 nowrap
 */
