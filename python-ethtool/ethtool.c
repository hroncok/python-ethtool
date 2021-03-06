/*
 * Copyright (C) 2008-2013 Red Hat Inc.
 *
 * Arnaldo Carvalho de Melo <acme@redhat.com>
 * David Sommerseth <davids@redhat.com>
 *
 * First bits from a Red Hat config tool by Harald Hoyer.
 *
 * This application is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; version 2.
 *
 * This application is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */
#include <Python.h>
#include <bytesobject.h>

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <ifaddrs.h>
#include <netlink/route/addr.h>
#include <net/if.h>

#include "etherinfo_struct.h"
#include "etherinfo_obj.h"
#include "etherinfo.h"

extern PyTypeObject PyEtherInfo_Type;

#ifndef IFF_DYNAMIC
#define IFF_DYNAMIC     0x8000          /* dialup device with changing addresses*/
#endif

typedef unsigned long long u64;
typedef __uint32_t u32;
typedef __uint16_t u16;
typedef __uint8_t u8;

#include "ethtool-copy.h"
#include <linux/sockios.h> /* for SIOCETHTOOL */

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

#define _PATH_PROCNET_DEV "/proc/net/dev"

static PyObject *get_active_devices(PyObject *self __unused, PyObject *args __unused)
{
	PyObject *list;
	struct ifaddrs *ifaddr, *ifa;

	if (getifaddrs(&ifaddr) == -1)
		return PyErr_SetFromErrno(PyExc_OSError);

	list = PyList_New(0);
	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		PyObject *str = PyBytes_FromString(ifa->ifa_name);
		/* names are not unique (listed for both ipv4 and ipv6) */
		if (!PySequence_Contains(list, str) && (ifa->ifa_flags & (IFF_UP))) {
			PyList_Append(list, str);
		}
	Py_DECREF(str);
	}

	freeifaddrs(ifaddr);

	return list;
}

static PyObject *get_devices(PyObject *self __unused, PyObject *args __unused)
{
	char buffer[256];
	char *ret;
	PyObject *list = PyList_New(0);
	FILE *fd = fopen(_PATH_PROCNET_DEV, "r");

	if (fd == NULL) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}
	/* skip over first two lines */
	ret = fgets(buffer, 256, fd);
	ret = fgets(buffer, 256, fd);
	if( !ret ) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	while (!feof(fd)) {
		PyObject *str;
		char *name = buffer;
		char *end = buffer;

		if (fgets(buffer, 256, fd) == NULL)
			break;
		/* find colon */
		while (*end && *end != ':')
			end++;
		*end = 0; /* terminate where colon was */
		while (*name == ' ')
			name++; /* skip over leading whitespace if any */

		str = PyBytes_FromString(name);
		PyList_Append(list, str);
		Py_DECREF(str);
	}
	fclose(fd);
	return list;
}

static PyObject *get_hwaddress(PyObject *self __unused, PyObject *args)
{
	struct ifreq ifr;
	int fd, err;
	const char *devname;
	char hwaddr[20];

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our request structure. */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCGIFHWADDR, &ifr);
	if (err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);

	sprintf(hwaddr, "%02x:%02x:%02x:%02x:%02x:%02x",
		(unsigned int)ifr.ifr_hwaddr.sa_data[0] % 256,
		(unsigned int)ifr.ifr_hwaddr.sa_data[1] % 256,
		(unsigned int)ifr.ifr_hwaddr.sa_data[2] % 256,
		(unsigned int)ifr.ifr_hwaddr.sa_data[3] % 256,
		(unsigned int)ifr.ifr_hwaddr.sa_data[4] % 256,
		(unsigned int)ifr.ifr_hwaddr.sa_data[5] % 256);

	return PyBytes_FromString(hwaddr);
}

static PyObject *get_ipaddress(PyObject *self __unused, PyObject *args)
{
	struct ifreq ifr;
	int fd, err;
	const char *devname;
	char ipaddr[20];

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our request structure. */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCGIFADDR, &ifr);
	if (err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);

	sprintf(ipaddr, "%u.%u.%u.%u",
		(unsigned int)ifr.ifr_addr.sa_data[2] % 256,
		(unsigned int)ifr.ifr_addr.sa_data[3] % 256,
		(unsigned int)ifr.ifr_addr.sa_data[4] % 256,
		(unsigned int)ifr.ifr_addr.sa_data[5] % 256);

	return PyBytes_FromString(ipaddr);
}


/**
 * Retrieves the current information about all interfaces.  All interfaces will be
 * returned as a list of objects per interface.
 *
 * @param self Not used
 * @param args Python arguments - device name(s) as either a string or a list
 *
 * @return Python list of objects on success, otherwise NULL.
 */
static PyObject *get_interfaces_info(PyObject *self __unused, PyObject *args) {
	PyObject *devlist = NULL;
	PyObject *inargs = NULL;
	char **fetch_devs = NULL;
	int i = 0, fetch_devs_len = 0;

	if (!PyArg_ParseTuple(args, "|O", &inargs)) {
		PyErr_SetString(PyExc_LookupError,
				"Argument must be either a string, list or a tuple");
		return NULL;
	}

	/* Parse input arguments if we got them */
	if( inargs != NULL ) {
		if( PyBytes_Check(inargs) ) { /* Input argument is just a string */
			fetch_devs_len = 1;
			fetch_devs = calloc(1, sizeof(char *));
			fetch_devs[0] = PyBytes_AsString(inargs);
#if PY_MAJOR_VERSION >= 3
		} else if( PyUnicode_Check(inargs) ) { /* Input argument is just a ustring */
			fetch_devs_len = 1;
			fetch_devs = calloc(1, sizeof(char *));
			fetch_devs[0] = PyUnicode_AsUTF8(inargs);
#endif
		} else if( PyTuple_Check(inargs) ) { /* Input argument is a tuple list with devices */
			int j = 0;

			fetch_devs_len = PyTuple_Size(inargs);
			fetch_devs = calloc(fetch_devs_len+1, sizeof(char *));
			for( i = 0; i < fetch_devs_len; i++ ) {
				PyObject *elmt = PyTuple_GetItem(inargs, i);
				if( elmt && PyBytes_Check(elmt) ) {
					fetch_devs[j++] = PyBytes_AsString(elmt);
				}
			}
			fetch_devs_len = j;
		} else if( PyList_Check(inargs) ) { /* Input argument is a list with devices */
			int j = 0;

			fetch_devs_len = PyList_Size(inargs);
			fetch_devs = calloc(fetch_devs_len+1, sizeof(char *));
			for( i = 0; i < fetch_devs_len; i++ ) {
				PyObject *elmt = PyList_GetItem(inargs, i);
				if( elmt && PyBytes_Check(elmt) ) {
					fetch_devs[j++] = PyBytes_AsString(elmt);
				}
			}
			fetch_devs_len = j;
		} else {
			PyErr_SetString(PyExc_LookupError,
					"Argument must be either a string, list or a tuple");
			return NULL;
		}
	}

	devlist = PyList_New(0);
	for( i = 0; i < fetch_devs_len; i++ ) {
                PyEtherInfo *dev = NULL;

		/* Store the device name and a reference to the NETLINK connection for
		 * objects to use when quering for device info
		 */

                dev = PyObject_New(PyEtherInfo, &PyEtherInfo_Type);
                if( !dev ) {
			PyErr_SetFromErrno(PyExc_OSError);
			free(fetch_devs);
			return NULL;
                }
		dev->device = PyBytes_FromString(fetch_devs[i]);
		dev->hwaddress = NULL;
		dev->index = -1;

		/* Append device object to the device list */
		PyList_Append(devlist, (PyObject *)dev);
		Py_DECREF(dev);
	}
	free(fetch_devs);

	return devlist;
}


static PyObject *get_flags (PyObject *self __unused, PyObject *args)
{
	struct ifreq ifr;
	const char *devname;
	int fd, err;

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our request structure. */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}
	err = ioctl(fd, SIOCGIFFLAGS, &ifr);
	if(err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);

	return Py_BuildValue("h", ifr.ifr_flags);


}
static PyObject *get_netmask (PyObject *self __unused, PyObject *args)
{
	struct ifreq ifr;
	int fd, err;
	const char *devname;
	char netmask[20];

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our request structure. */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCGIFNETMASK, &ifr);
	if (err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);

	sprintf(netmask, "%u.%u.%u.%u",
		(unsigned int)ifr.ifr_netmask.sa_data[2] % 256,
		(unsigned int)ifr.ifr_netmask.sa_data[3] % 256,
		(unsigned int)ifr.ifr_netmask.sa_data[4] % 256,
		(unsigned int)ifr.ifr_netmask.sa_data[5] % 256);

	return PyBytes_FromString(netmask);
}

static PyObject *get_broadcast(PyObject *self __unused, PyObject *args)
{
	struct ifreq ifr;
	int fd, err;
	const char *devname;
	char broadcast[20];

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our request structure. */
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCGIFBRDADDR, &ifr);
	if (err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);

	sprintf(broadcast, "%u.%u.%u.%u",
		(unsigned int)ifr.ifr_broadaddr.sa_data[2] % 256,
		(unsigned int)ifr.ifr_broadaddr.sa_data[3] % 256,
		(unsigned int)ifr.ifr_broadaddr.sa_data[4] % 256,
		(unsigned int)ifr.ifr_broadaddr.sa_data[5] % 256);

	return PyBytes_FromString(broadcast);
}

static PyObject *get_module(PyObject *self __unused, PyObject *args)
{
	struct ethtool_cmd ecmd;
	struct ifreq ifr;
	int fd, err;
	char buf[2048];
	const char *devname;

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our control structures. */
	memset(&ecmd, 0, sizeof(ecmd));
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;
	ifr.ifr_data = (caddr_t) &buf;
	ecmd.cmd = ETHTOOL_GDRVINFO;
	memcpy(&buf, &ecmd, sizeof(ecmd));

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCETHTOOL, &ifr);

	if (err < 0) {  /* failed? */
		PyErr_SetFromErrno(PyExc_IOError);
		FILE *file;
		int found = 0;
		char driver[101], dev[101];
		close(fd);

		/* Before bailing, maybe it is a PCMCIA/PC Card? */
		file = fopen("/var/lib/pcmcia/stab", "r");
		if (file == NULL) {
			return NULL;
		}

		while (!feof(file)) {
			if (fgets(buf, 2048, file) == NULL)
				break;
			buf[2047] = '\0';
			if (strncmp(buf, "Socket", 6) != 0) {
				if (sscanf(buf, "%*d\t%*s\t%100s\t%*d\t%100s\n", driver, dev) > 0) {
					driver[99] = '\0';
					dev[99] = '\0';
					if (strcmp(devname, dev) == 0) {
						found = 1;
						break;
					}
				}
			}
		}
		fclose(file);
		if (!found) {
			return NULL;
		} else {
			PyErr_Clear();
			return PyBytes_FromString(driver);
		}
	}

	close(fd);
	return PyBytes_FromString(((struct ethtool_drvinfo *)buf)->driver);
}

static PyObject *get_businfo(PyObject *self __unused, PyObject *args)
{
	struct ethtool_cmd ecmd;
	struct ifreq ifr;
	int fd, err;
	char buf[1024];
	const char *devname;

	if (!PyArg_ParseTuple(args, "s*", &devname))
		return NULL;

	/* Setup our control structures. */
	memset(&ecmd, 0, sizeof(ecmd));
	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;
	ifr.ifr_data = (caddr_t) &buf;
	ecmd.cmd = ETHTOOL_GDRVINFO;
	memcpy(&buf, &ecmd, sizeof(ecmd));

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0);
	if (fd < 0) {
		return PyErr_SetFromErrno(PyExc_OSError);
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCETHTOOL, &ifr);

	if (err < 0) {  /* failed? */
		PyErr_SetFromErrno(PyExc_IOError);
		close(fd);
		return NULL;
	}

	close(fd);
	return PyBytes_FromString(((struct ethtool_drvinfo *)buf)->bus_info);
}

static int send_command(int cmd, const char *devname, void *value)
{
	/* Setup our request structure. */
	int fd, err;
	struct ifreq ifr;
	struct ethtool_value *eval = value;

	memset(&ifr, 0, sizeof(ifr));
	strncpy(&ifr.ifr_name[0], devname, IFNAMSIZ);
	ifr.ifr_name[IFNAMSIZ - 1] = 0;
	ifr.ifr_data = (caddr_t)eval;
	eval->cmd = cmd;

	/* Open control socket. */
	fd = socket(AF_INET, SOCK_DGRAM, 0), err;
	if (fd < 0) {
		PyErr_SetFromErrno(PyExc_OSError);
		return -1;
	}

	/* Get current settings. */
	err = ioctl(fd, SIOCETHTOOL, &ifr);
	if (err < 0) {
		PyErr_SetFromErrno(PyExc_IOError);
	}

	close(fd);
	return err;
}

static int get_dev_value(int cmd, PyObject *args, void *value)
{
	const char *devname;
	int err = -1;

	if (PyArg_ParseTuple(args, "s*", &devname))
		err = send_command(cmd, devname, value);

	return err;
}

static int get_dev_int_value(int cmd, PyObject *args, int *value)
{
	struct ethtool_value eval;
	int rc = get_dev_value(cmd, args, &eval);

	if (rc == 0)
		*value = *(int *)&eval.data;

	return rc;
}

static int dev_set_int_value(int cmd, PyObject *args)
{
	struct ethtool_value eval;
	const char *devname;

	if (!PyArg_ParseTuple(args, "s*i", &devname, &eval.data))
		return -1;

	return send_command(cmd, devname, &eval);
}

static PyObject *get_tso(PyObject *self __unused, PyObject *args)
{
	int value = 0;

	if (get_dev_int_value(ETHTOOL_GTSO, args, &value) < 0)
		return NULL;

	return Py_BuildValue("b", value);
}

static PyObject *set_tso(PyObject *self __unused, PyObject *args)
{
	if (dev_set_int_value(ETHTOOL_STSO, args) < 0)
		return NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

static PyObject *get_ufo(PyObject *self __unused, PyObject *args)
{
	int value = 0;

	if (get_dev_int_value(ETHTOOL_GUFO, args, &value) < 0)
		return NULL;

	return Py_BuildValue("b", value);
}

static PyObject *get_gso(PyObject *self __unused, PyObject *args)
{
	int value = 0;

	if (get_dev_int_value(ETHTOOL_GGSO, args, &value) < 0)
		return NULL;

	return Py_BuildValue("b", value);
}

static PyObject *get_sg(PyObject *self __unused, PyObject *args)
{
	int value = 0;

	if (get_dev_int_value(ETHTOOL_GSG, args, &value) < 0)
		return NULL;

	return Py_BuildValue("b", value);
}

struct struct_desc {
	char	       *name;
	unsigned short offset;
	unsigned short size;
};

#define member_desc(type, member_name) { \
	.name = #member_name, \
	.offset = offsetof(type, member_name), \
	.size = sizeof(((type *)0)->member_name), }

struct struct_desc ethtool_coalesce_desc[] = {
	member_desc(struct ethtool_coalesce, rx_coalesce_usecs),
	member_desc(struct ethtool_coalesce, rx_max_coalesced_frames),
	member_desc(struct ethtool_coalesce, rx_coalesce_usecs_irq),
	member_desc(struct ethtool_coalesce, rx_max_coalesced_frames_irq),
	member_desc(struct ethtool_coalesce, tx_coalesce_usecs),
	member_desc(struct ethtool_coalesce, tx_max_coalesced_frames),
	member_desc(struct ethtool_coalesce, tx_coalesce_usecs_irq),
	member_desc(struct ethtool_coalesce, tx_max_coalesced_frames_irq),
	member_desc(struct ethtool_coalesce, stats_block_coalesce_usecs),
	member_desc(struct ethtool_coalesce, use_adaptive_rx_coalesce),
	member_desc(struct ethtool_coalesce, use_adaptive_tx_coalesce),
	member_desc(struct ethtool_coalesce, pkt_rate_low),
	member_desc(struct ethtool_coalesce, rx_coalesce_usecs_low),
	member_desc(struct ethtool_coalesce, rx_max_coalesced_frames_low),
	member_desc(struct ethtool_coalesce, tx_coalesce_usecs_low),
	member_desc(struct ethtool_coalesce, tx_max_coalesced_frames_low),
	member_desc(struct ethtool_coalesce, pkt_rate_high),
	member_desc(struct ethtool_coalesce, rx_coalesce_usecs_high),
	member_desc(struct ethtool_coalesce, rx_max_coalesced_frames_high),
	member_desc(struct ethtool_coalesce, tx_coalesce_usecs_high),
	member_desc(struct ethtool_coalesce, tx_max_coalesced_frames_high),
	member_desc(struct ethtool_coalesce, rate_sample_interval),
};

static PyObject *__struct_desc_create_dict(struct struct_desc *table,
					   int nr_entries, void *values)
{
	int i;
	PyObject *dict = PyDict_New();

	if (dict == NULL)
		goto out;

	for (i = 0; i < nr_entries; ++i) {
		struct struct_desc *d = &table[i];
		PyObject *objval = NULL;
		void *val = values + d->offset;

		switch (d->size) {
		case sizeof(uint32_t):
			objval = PyLong_FromLong(*(uint32_t *)val);
			break;
		}

		if (objval == NULL)
			goto free_dict;

		if (PyDict_SetItemString(dict, d->name, objval) != 0) {
			Py_DECREF(objval);
			goto free_dict;
		}
			
		Py_DECREF(objval);
	}
out:
	return dict;
free_dict:
	goto out;
	dict = NULL;
}

#define struct_desc_create_dict(table, values) \
	__struct_desc_create_dict(table, ARRAY_SIZE(table), values)

static int __struct_desc_from_dict(struct struct_desc *table,
				   int nr_entries, void *to, PyObject *dict)
{
	char buf[2048];
	int i;

	for (i = 0; i < nr_entries; ++i) {
		struct struct_desc *d = &table[i];
		void *val = to + d->offset;
		PyObject *obj;

		switch (d->size) {
		case sizeof(uint32_t):
			obj = PyDict_GetItemString(dict, d->name);
			if (obj == NULL) {
				snprintf(buf, sizeof(buf),
					"Missing dict entry for field %s",
					d->name);
				PyErr_SetString(PyExc_IOError, buf);
				return -1;
			}
			*(uint32_t *)val = PyLong_AsLong(obj);
			break;
		default:
			snprintf(buf, sizeof(buf),
				 "Invalid type size %d for field %s",
				 d->size, d->name);
			PyErr_SetString(PyExc_IOError, buf);
			return -1;
		}
	}

	return 0;
}

#define struct_desc_from_dict(table, to, dict) \
	__struct_desc_from_dict(table, ARRAY_SIZE(table), to, dict)

static PyObject *get_coalesce(PyObject *self __unused, PyObject *args)
{
	struct ethtool_coalesce coal;

	if (get_dev_value(ETHTOOL_GCOALESCE, args, &coal) < 0)
		return NULL;

	return struct_desc_create_dict(ethtool_coalesce_desc, &coal);
}

static PyObject *set_coalesce(PyObject *self __unused, PyObject *args)
{
	struct ethtool_coalesce coal;
	const char *devname;
	PyObject *dict;

	if (!PyArg_ParseTuple(args, "s*O", &devname, &dict))
		return NULL;

	if (struct_desc_from_dict(ethtool_coalesce_desc, &coal, dict) != 0)
		return NULL;

	if (send_command(ETHTOOL_SCOALESCE, devname, &coal))
		return NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

struct struct_desc ethtool_ringparam_desc[] = {
	member_desc(struct ethtool_ringparam, rx_max_pending),
	member_desc(struct ethtool_ringparam, rx_mini_max_pending),
	member_desc(struct ethtool_ringparam, rx_jumbo_max_pending),
	member_desc(struct ethtool_ringparam, tx_max_pending),
	member_desc(struct ethtool_ringparam, rx_pending),
	member_desc(struct ethtool_ringparam, rx_mini_pending),
	member_desc(struct ethtool_ringparam, rx_jumbo_pending),
	member_desc(struct ethtool_ringparam, tx_pending),
};

static PyObject *get_ringparam(PyObject *self __unused, PyObject *args)
{
	struct ethtool_ringparam ring;

	if (get_dev_value(ETHTOOL_GRINGPARAM, args, &ring) < 0)
		return NULL;

	return struct_desc_create_dict(ethtool_ringparam_desc, &ring);
}

static PyObject *set_ringparam(PyObject *self __unused, PyObject *args)
{
	struct ethtool_ringparam ring;
	const char *devname;
	PyObject *dict;

	if (!PyArg_ParseTuple(args, "s*O", &devname, &dict))
		return NULL;

	if (struct_desc_from_dict(ethtool_ringparam_desc, &ring, dict) != 0)
		return NULL;

	if (send_command(ETHTOOL_SRINGPARAM, devname, &ring))
		return NULL;

	Py_INCREF(Py_None);
	return Py_None;
}

static struct PyMethodDef PyEthModuleMethods[] = {
	{
		.ml_name = "get_module",
		.ml_meth = (PyCFunction)get_module,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_businfo",
		.ml_meth = (PyCFunction)get_businfo,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_hwaddr",
		.ml_meth = (PyCFunction)get_hwaddress,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_ipaddr",
		.ml_meth = (PyCFunction)get_ipaddress,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_interfaces_info",
		.ml_meth = (PyCFunction)get_interfaces_info,
		.ml_flags = METH_VARARGS,
		.ml_doc = "Accepts a string, list or tupples of interface names. "
		"Returns a list of ethtool.etherinfo objets with device information."
	},
	{
		.ml_name = "get_netmask",
		.ml_meth = (PyCFunction)get_netmask,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_broadcast",
		.ml_meth = (PyCFunction)get_broadcast,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_coalesce",
		.ml_meth = (PyCFunction)get_coalesce,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "set_coalesce",
		.ml_meth = (PyCFunction)set_coalesce,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_devices",
		.ml_meth = (PyCFunction)get_devices,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_active_devices",
		.ml_meth = (PyCFunction)get_active_devices,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_ringparam",
		.ml_meth = (PyCFunction)get_ringparam,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "set_ringparam",
		.ml_meth = (PyCFunction)set_ringparam,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_tso",
		.ml_meth = (PyCFunction)get_tso,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "set_tso",
		.ml_meth = (PyCFunction)set_tso,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_ufo",
		.ml_meth = (PyCFunction)get_ufo,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_gso",
		.ml_meth = (PyCFunction)get_gso,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_sg",
		.ml_meth = (PyCFunction)get_sg,
		.ml_flags = METH_VARARGS,
	},
	{
		.ml_name = "get_flags",
		.ml_meth = (PyCFunction)get_flags,
		.ml_flags = METH_VARARGS,
	},
	{	.ml_name = NULL, },
};

#if PY_MAJOR_VERSION >= 3
  #define MOD_ERROR_VAL NULL
  #define MOD_SUCCESS_VAL(val) val
  #define MOD_INIT(name) PyMODINIT_FUNC PyInit_##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          static struct PyModuleDef moduledef = { \
            PyModuleDef_HEAD_INIT, name, doc, -1, methods, }; \
          ob = PyModule_Create(&moduledef);
#else
  #define MOD_ERROR_VAL
  #define MOD_SUCCESS_VAL(val)
  #define MOD_INIT(name) void init##name(void)
  #define MOD_DEF(ob, name, doc, methods) \
          ob = Py_InitModule3(name, methods, doc);
#endif

MOD_INIT(ethtool)
{
	PyObject *m;
	MOD_DEF(m, "ethtool", "Python ethtool module", PyEthModuleMethods);

	// Prepare the ethtool.etherinfo class
	if (PyType_Ready(&PyEtherInfo_Type) < 0)
		return MOD_ERROR_VAL;

	// Prepare the ethtool IPv6 and IPv4 address types
	if (PyType_Ready(&ethtool_netlink_ip_address_Type))
		return MOD_ERROR_VAL;

	// Setup constants
	PyModule_AddIntConstant(m, "IFF_UP", IFF_UP);			/* Interface is up. */
	PyModule_AddIntConstant(m, "IFF_BROADCAST", IFF_BROADCAST);	/* Broadcast address valid. */
	PyModule_AddIntConstant(m, "IFF_DEBUG", IFF_DEBUG);		/* Turn on debugging. */
	PyModule_AddIntConstant(m, "IFF_LOOPBACK", IFF_LOOPBACK);	/* Is a loopback net */
	PyModule_AddIntConstant(m, "IFF_POINTOPOINT", IFF_POINTOPOINT); /* Is a point-to-point link */
	PyModule_AddIntConstant(m, "IFF_NOTRAILERS", IFF_NOTRAILERS);	/* Avoid use of trailers */
	PyModule_AddIntConstant(m, "IFF_RUNNING", IFF_RUNNING);		/* Resources allocated */
	PyModule_AddIntConstant(m, "IFF_NOARP", IFF_NOARP);		/* No address resolution protocol. */
	PyModule_AddIntConstant(m, "IFF_PROMISC", IFF_PROMISC);		/* Receive all packets. */
	PyModule_AddIntConstant(m, "IFF_ALLMULTI", IFF_ALLMULTI);	/* Receive all multicast packets. */
	PyModule_AddIntConstant(m, "IFF_MASTER", IFF_MASTER);		/* Master of a load balancer. */
	PyModule_AddIntConstant(m, "IFF_SLAVE", IFF_SLAVE);		/* Slave of a load balancer. */
	PyModule_AddIntConstant(m, "IFF_MULTICAST", IFF_MULTICAST);	/* Supports multicast. */
	PyModule_AddIntConstant(m, "IFF_PORTSEL", IFF_PORTSEL);		/* Can set media type. */
	PyModule_AddIntConstant(m, "IFF_AUTOMEDIA", IFF_AUTOMEDIA);	/* Auto media select active. */
	PyModule_AddIntConstant(m, "IFF_DYNAMIC", IFF_DYNAMIC);		/* Dialup device with changing addresses.  */
	PyModule_AddIntConstant(m, "AF_INET", AF_INET);                 /* IPv4 interface */
	PyModule_AddIntConstant(m, "AF_INET6", AF_INET6);               /* IPv6 interface */
	PyModule_AddStringConstant(m, "version", "python-ethtool v" VERSION);

	return MOD_SUCCESS_VAL(m);
}


/*
Local variables:
c-basic-offset: 8
indent-tabs-mode: y
End:
*/

