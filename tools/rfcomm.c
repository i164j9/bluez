// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2002-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <stdio.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>
#include <signal.h>
#include <termios.h>
#include <poll.h>
#include <sys/param.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>

#include "bluetooth/bluetooth.h"
#include "bluetooth/hci.h"
#include "bluetooth/hci_lib.h"
#include "bluetooth/rfcomm.h"
#include "bluetooth/sdp.h"
#include "bluetooth/sdp_lib.h"

static int rfcomm_raw_tty = 0;
static int auth = 0;
static int encryption = 0;
static int secure = 0;
static int central = 0;
static int linger = 0;
static int connect_timeout_ms = -1;
static const char *service_name = NULL;

#define RFCOMM_CHANNEL_MIN 1
#define RFCOMM_CHANNEL_MAX 30
#define RFCOMM_DEFAULT_CHANNEL 1

struct local_sdp_service {
	sdp_session_t *session;
	sdp_record_t *record;
	bdaddr_t device;
	int device_record;
};

static char *rfcomm_state[] = {
	"unknown",
	"connected",
	"clean",
	"bound",
	"listening",
	"connecting",
	"connecting",
	"config",
	"disconnecting",
	"closed"
};

static volatile sig_atomic_t __io_canceled = 0;

static void sig_hup(int sig)
{
	(void) sig;
	return;
}
static void sig_term(int sig)
{
	(void) sig;
	__io_canceled = 1;
}

static int parse_uint_arg(const char *arg, const char *label,
				unsigned long min, unsigned long max,
				unsigned long *value)
{
	char *endptr;
	unsigned long parsed;

	errno = 0;
	parsed = strtoul(arg, &endptr, 10);
	if (errno || endptr == arg || *endptr != '\0' || parsed < min || parsed > max) {
		fprintf(stderr, "Invalid %s: %s\n", label, arg);
		return -1;
	}

	*value = parsed;
	return 0;
}

static int parse_bdaddr_arg(const char *arg, bdaddr_t *bdaddr)
{
	if (bachk(arg) < 0) {
		fprintf(stderr, "Invalid Bluetooth address: %s\n", arg);
		return -1;
	}

	str2ba(arg, bdaddr);
	return 0;
}

static int parse_rfcomm_channel_arg(const char *arg, int allow_zero,
					uint8_t *channel)
{
	unsigned long parsed;
	unsigned long min = allow_zero ? 0 : RFCOMM_CHANNEL_MIN;

	if (parse_uint_arg(arg, "RFCOMM channel", min,
					RFCOMM_CHANNEL_MAX, &parsed) < 0)
		return -1;

	*channel = parsed;
	return 0;
}

static int parse_hci_device_arg(const char *arg, bdaddr_t *bdaddr)
{
	unsigned long dev_id;

	if (parse_uint_arg(arg + 3, "HCI device", 0, UINT_MAX, &dev_id) < 0)
		return -1;

	if (hci_devba(dev_id, bdaddr) < 0) {
		perror("Can't get HCI device address");
		return -1;
	}

	return 0;
}

static int parse_rfcomm_dev_id(const char *arg, int *dev_id)
{
	const char *value = arg;
	unsigned long parsed;

	if (strncmp(value, "/dev/rfcomm", 11) == 0)
		value += 11;
	else if (strncmp(value, "rfcomm", 6) == 0)
		value += 6;

	if (parse_uint_arg(value, "RFCOMM device", 0,
					RFCOMM_MAX_DEV - 1, &parsed) < 0)
		return -1;

	*dev_id = parsed;
	return 0;
}

static int select_serial_port_channel_from_records(sdp_list_t *rsp,
						const char *name,
						uint8_t *channel,
						unsigned int *matches)
{
	sdp_list_t *entry;
	unsigned int total_matches = 0;
	int err = -1;

	for (entry = rsp; entry; entry = entry->next) {
		sdp_record_t *record = entry->data;
		sdp_list_t *protos = NULL;
		char remote_name[256];
		int have_name;
		int port;

		remote_name[0] = '\0';
		have_name = sdp_get_service_name(record, remote_name,
						sizeof(remote_name)) == 0;

		if (name && (!have_name || strcmp(remote_name, name) != 0))
			continue;

		if (sdp_get_access_protos(record, &protos) < 0)
			continue;

		port = sdp_get_proto_port(protos, RFCOMM_UUID);
		sdp_list_free_proto_descs(protos);

		if (port < RFCOMM_CHANNEL_MIN || port > RFCOMM_CHANNEL_MAX)
			continue;

		total_matches++;
		if (total_matches == 1) {
			*channel = port;
			err = 0;
		}

		if (name)
			break;
	}

	if (matches)
		*matches = total_matches;

	return err;
}

static int lookup_serial_port_channel(const bdaddr_t *src,
					const bdaddr_t *dst,
					const char *name,
					uint8_t *channel)
{
	sdp_session_t *sdp;
	sdp_list_t *srch = NULL, *attrs = NULL, *rsp = NULL, *entry;
	uuid_t svclass;
	uint16_t attr_name = SDP_ATTR_SVCNAME_PRIMARY;
	uint16_t attr_proto = SDP_ATTR_PROTO_DESC_LIST;
	unsigned int matches = 0;
	int err = -1;

	sdp = sdp_connect(src, dst, SDP_RETRY_IF_BUSY);
	if (!sdp)
		return -1;

	sdp_uuid16_create(&svclass, SERIAL_PORT_SVCLASS_ID);
	srch = sdp_list_append(NULL, &svclass);
	attrs = sdp_list_append(NULL, &attr_name);
	attrs = sdp_list_append(attrs, &attr_proto);

	if (!srch || !attrs)
		goto done;

	if (sdp_service_search_attr_req(sdp, srch,
				SDP_ATTR_REQ_INDIVIDUAL, attrs, &rsp) < 0)
		goto done;

	err = select_serial_port_channel_from_records(rsp, name, channel,
							&matches);

	if (!name && matches > 1)
		fprintf(stderr,
			"Multiple Serial Port SDP records matched; using the first channel. Use --service-name to select a specific service.\n");

done:
	if (rsp)
		sdp_list_free(rsp, (sdp_free_func_t) sdp_record_free);
	if (attrs)
		sdp_list_free(attrs, NULL);
	if (srch)
		sdp_list_free(srch, NULL);
	sdp_close(sdp);

	return err;
}

static int resolve_remote_channel(const bdaddr_t *src, const bdaddr_t *dst,
					uint8_t *channel)
{
	char addr[18];

	if (lookup_serial_port_channel(src, dst, service_name, channel) == 0)
		return 0;

	ba2str(dst, addr);
	if (service_name)
		fprintf(stderr,
			"Can't find Serial Port RFCOMM channel for %s via SDP matching service '%s'\n",
			addr, service_name);
	else
		fprintf(stderr,
			"Can't find Serial Port RFCOMM channel for %s via SDP\n",
			addr);
	return -1;
}

static int register_local_serial_port(const bdaddr_t *src, uint8_t channel,
					struct local_sdp_service *service)
{
	sdp_list_t *svclass_id = NULL, *apseq = NULL, *aproto = NULL;
	sdp_list_t *proto[2] = { NULL, NULL }, *profiles = NULL, *root = NULL;
	sdp_profile_desc_t profile;
	uuid_t root_uuid, sp_uuid, l2cap, rfcomm;
	sdp_data_t *channel_data = NULL;
	sdp_record_t *record = NULL;
	sdp_session_t *session = NULL;
	int err;

	memset(service, 0, sizeof(*service));

	session = sdp_connect(BDADDR_ANY, BDADDR_LOCAL, 0);
	if (!session) {
		perror("Can't connect to local SDP server");
		return -1;
	}

	record = sdp_record_alloc();
	if (!record) {
		fprintf(stderr, "Can't allocate SDP record\n");
		sdp_close(session);
		return -1;
	}

	sdp_uuid16_create(&root_uuid, PUBLIC_BROWSE_GROUP);
	root = sdp_list_append(NULL, &root_uuid);
	sdp_set_browse_groups(record, root);

	sdp_uuid16_create(&sp_uuid, SERIAL_PORT_SVCLASS_ID);
	svclass_id = sdp_list_append(NULL, &sp_uuid);
	sdp_set_service_classes(record, svclass_id);

	sdp_uuid16_create(&profile.uuid, SERIAL_PORT_PROFILE_ID);
	profile.version = 0x0102;
	profiles = sdp_list_append(NULL, &profile);
	sdp_set_profile_descs(record, profiles);

	sdp_uuid16_create(&l2cap, L2CAP_UUID);
	proto[0] = sdp_list_append(NULL, &l2cap);
	apseq = sdp_list_append(NULL, proto[0]);

	sdp_uuid16_create(&rfcomm, RFCOMM_UUID);
	proto[1] = sdp_list_append(NULL, &rfcomm);
	channel_data = sdp_data_alloc(SDP_UINT8, &channel);
	proto[1] = sdp_list_append(proto[1], channel_data);
	apseq = sdp_list_append(apseq, proto[1]);

	aproto = sdp_list_append(NULL, apseq);
	sdp_set_access_protos(record, aproto);

	sdp_add_lang_attr(record);
	sdp_set_info_attr(record, "Serial Port", "BlueZ", "RFCOMM serial port");
	sdp_set_service_id(record, sp_uuid);

	if (bacmp(src, BDADDR_ANY) == 0) {
		err = sdp_record_register(session, record, 0);
	} else {
		bacpy(&service->device, src);
		service->device_record = 1;
		err = sdp_device_record_register(session, &service->device,
							record, 0);
	}

	if (err < 0) {
		perror("Can't register Serial Port service");
		sdp_record_free(record);
		sdp_close(session);
		service->device_record = 0;
		session = NULL;
		record = NULL;
		err = -1;
		goto done;
	}

	service->session = session;
	service->record = record;
	err = 0;

done:
	sdp_data_free(channel_data);
	if (root)
		sdp_list_free(root, NULL);
	if (svclass_id)
		sdp_list_free(svclass_id, NULL);
	if (profiles)
		sdp_list_free(profiles, NULL);
	if (proto[0])
		sdp_list_free(proto[0], NULL);
	if (proto[1])
		sdp_list_free(proto[1], NULL);
	if (apseq)
		sdp_list_free(apseq, NULL);
	if (aproto)
		sdp_list_free(aproto, NULL);

	return err;
}

static void unregister_local_serial_port(struct local_sdp_service *service)
{
	if (!service->session || !service->record)
		return;

	if (service->device_record) {
		if (sdp_device_record_unregister(service->session, &service->device,
							 service->record) < 0)
			perror("Can't unregister Serial Port service");
	} else if (sdp_record_unregister(service->session, service->record) < 0)
		perror("Can't unregister Serial Port service");

	sdp_record_free(service->record);
	sdp_close(service->session);
	memset(service, 0, sizeof(*service));
}

static char *rfcomm_flagstostr(uint32_t flags)
{
	static char str[100];
	str[0] = 0;

	strcat(str, "[");

	if (flags & (1 << RFCOMM_REUSE_DLC))
		strcat(str, "reuse-dlc ");

	if (flags & (1 << RFCOMM_RELEASE_ONHUP))
		strcat(str, "release-on-hup ");

	if (flags & (1 << RFCOMM_TTY_ATTACHED))
		strcat(str, "tty-attached");

	strcat(str, "]");
	return str;
}

static void print_dev_info(struct rfcomm_dev_info *di)
{
	char src[18], dst[18], addr[40];

	ba2str(&di->src, src); ba2str(&di->dst, dst);

	if (bacmp(&di->src, BDADDR_ANY) == 0)
		sprintf(addr, "%s", dst);
	else
		sprintf(addr, "%s -> %s", src, dst);

	printf("rfcomm%d: %s channel %d %s %s\n",
		di->id, addr, di->channel,
		rfcomm_state[di->state],
		di->flags ? rfcomm_flagstostr(di->flags) : "");
}

static void print_dev_list(int ctl, int flags)
{
	struct rfcomm_dev_list_req *dl;
	struct rfcomm_dev_info *di;
	int i;

	(void) flags;

	dl = malloc(sizeof(*dl) + RFCOMM_MAX_DEV * sizeof(*di));
	if (!dl) {
		perror("Can't allocate memory");
		exit(1);
	}

	dl->dev_num = RFCOMM_MAX_DEV;
	di = dl->dev_info;

	if (ioctl(ctl, RFCOMMGETDEVLIST, (void *) dl) < 0) {
		perror("Can't get device list");
		free(dl);
		exit(1);
	}

	for (i = 0; i < dl->dev_num; i++)
		print_dev_info(di + i);
	free(dl);
}

static int set_rfcomm_link_mode(int sk)
{
	int lm;

	lm = 0;
	if (central)
		lm |= RFCOMM_LM_MASTER;

	if (!lm)
		return 0;

	if (setsockopt(sk, SOL_RFCOMM, RFCOMM_LM, &lm, sizeof(lm)) < 0)
		return -errno;

	return 0;
}

static int set_rfcomm_socket_security(int sk)
{
	int level;

	if (secure)
		level = BT_SECURITY_HIGH;
	else if (auth || encryption)
		level = BT_SECURITY_MEDIUM;
	else
		return 0;

	if (setsockopt(sk, SOL_BLUETOOTH, BT_SECURITY, &level,
								 sizeof(level)) < 0)
		return -errno;

	return 0;
}

static int create_dev(int ctl, int dev, uint32_t flags, bdaddr_t *bdaddr, int argc, char **argv)
{
	struct rfcomm_dev_req req;
	uint8_t channel;
	int err;

	memset(&req, 0, sizeof(req));
	req.dev_id = dev;
	req.flags = flags;
	bacpy(&req.src, bdaddr);

	if (argc < 2) {
		fprintf(stderr, "Missing dev parameter");
		return -EINVAL;
	}

	if (parse_bdaddr_arg(argv[1], &req.dst) < 0)
		return -EINVAL;

	if (argc > 2) {
		if (parse_rfcomm_channel_arg(argv[2], 0, &channel) < 0)
			return -EINVAL;
	} else {
		if (resolve_remote_channel(bdaddr, &req.dst, &channel) < 0)
			return -EINVAL;
	}

	req.channel = channel;

	err = ioctl(ctl, RFCOMMCREATEDEV, &req);
	if (err == -1) {
		err = -errno;

		if (err == -EOPNOTSUPP)
			fprintf(stderr, "RFCOMM TTY support not available\n");
		else
			perror("Can't create device");
	}

	return err;
}

static int release_dev(int ctl, int dev, uint32_t flags)
{
	struct rfcomm_dev_req req;
	int err;

	(void) flags;

	memset(&req, 0, sizeof(req));
	req.dev_id = dev;

	err = ioctl(ctl, RFCOMMRELEASEDEV, &req);
	if (err < 0)
		perror("Can't release device");

	return err;
}

static int release_all(int ctl)
{
	struct rfcomm_dev_list_req *dl;
	struct rfcomm_dev_info *di;
	int i;

	dl = malloc(sizeof(*dl) + RFCOMM_MAX_DEV * sizeof(*di));
	if (!dl) {
		perror("Can't allocate memory");
		exit(1);
	}

	dl->dev_num = RFCOMM_MAX_DEV;
	di = dl->dev_info;

	if (ioctl(ctl, RFCOMMGETDEVLIST, (void *) dl) < 0) {
		perror("Can't get device list");
		free(dl);
		exit(1);
	}

	for (i = 0; i < dl->dev_num; i++)
		release_dev(ctl, (di + i)->id, 0);

	free(dl);
	return 0;
}

static int run_cmdline(struct pollfd *p, sigset_t *sigs, char *devname,
			int argc, char **argv)
{
	int i;
	pid_t pid;
	char **cmdargv;
	struct sigaction sa;
	int ret = 0;

	cmdargv = malloc((argc + 1) * sizeof(char *));
	if (!cmdargv)
		return 1;

	for (i = 0; i < argc; i++)
		cmdargv[i] = (strcmp(argv[i], "{}") == 0) ? devname : argv[i];
	cmdargv[i] = NULL;

	pid = fork();

	switch (pid) {
	case 0:
		memset(&sa, 0, sizeof(sa));
		sa.sa_handler = SIG_DFL;
		sigaction(SIGCHLD, &sa, NULL);
		sigaction(SIGPIPE, &sa, NULL);

		execvp(cmdargv[0], cmdargv);
		fprintf(stderr, "Couldn't execute command %s (errno=%d:%s)\n",
				cmdargv[0], errno, strerror(errno));
		_exit(EXIT_FAILURE);
	case -1:
		fprintf(stderr, "Couldn't fork to execute command %s\n",
				cmdargv[0]);
		ret = 1;
		break;
	default:
		while (1) {
			int status;
			pid_t child;
			struct timespec ts;
			int poll_result;

			child = waitpid(pid, &status, WNOHANG);
			if (child == pid) {
				if (WIFEXITED(status))
					ret = WEXITSTATUS(status);
				else if (WIFSIGNALED(status))
					ret = 128 + WTERMSIG(status);
				else
					ret = 1;
				break;
			}

			if (child < 0) {
				if (errno == EINTR)
					continue;

				ret = 1;
				break;
			}

			p->revents = 0;
			ts.tv_sec  = 0;
			ts.tv_nsec = 200 * 1000 * 1000;
			poll_result = ppoll(p, 1, &ts, sigs);
			if (poll_result < 0) {
				if (errno == EINTR)
					continue;

				ret = 1;
				break;
			}

			if (poll_result > 0 || __io_canceled) {
				kill(pid, SIGTERM);
				waitpid(pid, &status, 0);
				if (WIFEXITED(status))
					ret = WEXITSTATUS(status);
				else if (WIFSIGNALED(status))
					ret = 128 + WTERMSIG(status);
				else
					ret = 1;
				break;
			}
		}
		break;
	}

	free(cmdargv);
	return ret;
}

static int cmd_connect(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	struct sockaddr_rc laddr, raddr;
	struct rfcomm_dev_req req;
	struct termios ti;
	struct sigaction sa;
	struct pollfd p;
	sigset_t sigs;
	socklen_t alen;
	char dst[18], devname[MAXPATHLEN];
	uint8_t channel;
	int sk, fd, flags, try = 30;

	(void) ctl;

	laddr.rc_family = AF_BLUETOOTH;
	bacpy(&laddr.rc_bdaddr, bdaddr);
	laddr.rc_channel = 0;

	if (argc < 2) {
		fprintf(stderr, "Missing dev parameter");
		return 1;
	}

	raddr.rc_family = AF_BLUETOOTH;
	if (parse_bdaddr_arg(argv[1], &raddr.rc_bdaddr) < 0)
		return 1;

	if (argc > 2) {
		if (parse_rfcomm_channel_arg(argv[2], 0, &channel) < 0)
			return 1;
	} else {
		if (resolve_remote_channel(bdaddr, &raddr.rc_bdaddr, &channel) < 0)
			return 1;
	}

	raddr.rc_channel = channel;

	sk = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0) {
		perror("Can't create RFCOMM socket");
		return 1;
	}

	if (connect_timeout_ms >= 0) {
		flags = fcntl(sk, F_GETFL);
		if (flags < 0 || fcntl(sk, F_SETFL, flags | O_NONBLOCK) < 0) {
			perror("Can't set RFCOMM socket nonblocking");
			close(sk);
			return 1;
		}
	}

	if (set_rfcomm_link_mode(sk) < 0) {
		perror("Can't set RFCOMM link mode");
		close(sk);
		return 1;
	}

	if (set_rfcomm_socket_security(sk) < 0) {
		perror("Can't set RFCOMM socket security");
		close(sk);
		return 1;
	}

	if (linger) {
		struct linger l = { .l_onoff = 1, .l_linger = linger };

		if (setsockopt(sk, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) {
			perror("Can't set linger option");
			close(sk);
			return 1;
		}
	}

	if (bind(sk, (struct sockaddr *) &laddr, sizeof(laddr)) < 0) {
		perror("Can't bind RFCOMM socket");
		close(sk);
		return 1;
	}

	if (connect(sk, (struct sockaddr *) &raddr, sizeof(raddr)) < 0) {
		if (errno != EINPROGRESS && errno != EAGAIN) {
			perror("Can't connect RFCOMM socket");
			close(sk);
			return 1;
		}

		p.fd = sk;
		p.events = POLLOUT | POLLERR | POLLHUP;

		while (poll(&p, 1, connect_timeout_ms) < 0) {
			if (errno == EINTR)
				continue;

			perror("Can't wait for RFCOMM socket");
			close(sk);
			return 1;
		}

		if (!p.revents) {
			errno = ETIMEDOUT;
			perror("Can't connect RFCOMM socket");
			close(sk);
			return 1;
		}

		if (p.revents & (POLLERR | POLLHUP | POLLNVAL | POLLOUT)) {
			int err = 0;
			socklen_t len = sizeof(err);

			if (getsockopt(sk, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
				if (errno == ENOSYS)
					goto connect_ready;

				perror("Can't connect RFCOMM socket");
			} else if (err && err != ENOSYS) {
				errno = err;
				perror("Can't connect RFCOMM socket");
			} else if (!(p.revents & POLLOUT))
				fprintf(stderr, "Can't connect RFCOMM socket\n");
			else
				goto connect_ready;

			close(sk);
			return 1;
		}
	}

connect_ready:
	if (connect_timeout_ms >= 0) {
		flags = fcntl(sk, F_GETFL);
		if (flags >= 0)
			fcntl(sk, F_SETFL, flags & ~O_NONBLOCK);
	}

	alen = sizeof(laddr);
	if (getsockname(sk, (struct sockaddr *)&laddr, &alen) < 0) {
		perror("Can't get RFCOMM socket name");
		close(sk);
		return 1;
	}

	memset(&req, 0, sizeof(req));
	req.dev_id = dev;
	req.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP);

	bacpy(&req.src, &laddr.rc_bdaddr);
	bacpy(&req.dst, &raddr.rc_bdaddr);
	req.channel = raddr.rc_channel;

	dev = ioctl(sk, RFCOMMCREATEDEV, &req);
	if (dev < 0) {
		perror("Can't create RFCOMM TTY");
		close(sk);
		return 1;
	}

	snprintf(devname, MAXPATHLEN - 1, "/dev/rfcomm%d", dev);
	while ((fd = open(devname, O_RDONLY | O_NOCTTY)) < 0) {
		if (errno == EACCES) {
			perror("Can't open RFCOMM device");
			goto release;
		}

		snprintf(devname, MAXPATHLEN - 1, "/dev/bluetooth/rfcomm/%d", dev);
		if ((fd = open(devname, O_RDONLY | O_NOCTTY)) < 0) {
			if (try--) {
				snprintf(devname, MAXPATHLEN - 1, "/dev/rfcomm%d", dev);
				usleep(100 * 1000);
				continue;
			}
			perror("Can't open RFCOMM device");
			goto release;
		}
	}

	if (rfcomm_raw_tty) {
		tcflush(fd, TCIOFLUSH);

		cfmakeraw(&ti);
		tcsetattr(fd, TCSANOW, &ti);
	}

	close(sk);

	ba2str(&req.dst, dst);
	printf("Connected %s to %s on channel %d\n", devname, dst, req.channel);
	printf("Press CTRL-C for hangup\n");

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags   = SA_NOCLDSTOP;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);

	sigfillset(&sigs);
	sigdelset(&sigs, SIGCHLD);
	sigdelset(&sigs, SIGPIPE);
	sigdelset(&sigs, SIGTERM);
	sigdelset(&sigs, SIGINT);
	sigdelset(&sigs, SIGHUP);

	p.fd = fd;
	p.events = POLLERR | POLLHUP;

	while (!__io_canceled) {
		p.revents = 0;
		if (ppoll(&p, 1, NULL, &sigs) > 0)
			break;
	}

	printf("Disconnected\n");

	close(fd);
	return 0;

release:
	memset(&req, 0, sizeof(req));
	req.dev_id = dev;
	req.flags = (1 << RFCOMM_HANGUP_NOW);
	ioctl(ctl, RFCOMMRELEASEDEV, &req);

	close(sk);
	return 1;
}

static int cmd_listen(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	struct sockaddr_rc laddr, raddr;
	struct rfcomm_dev_req req;
	struct local_sdp_service sdp_service;
	struct termios ti;
	struct sigaction sa;
	struct pollfd p;
	sigset_t sigs;
	socklen_t alen;
	char dst[18], devname[MAXPATHLEN];
	uint8_t channel;
	int sk, nsk, fd, ret = 0, try = 30;

	memset(&sdp_service, 0, sizeof(sdp_service));

	laddr.rc_family = AF_BLUETOOTH;
	bacpy(&laddr.rc_bdaddr, bdaddr);
	if (argc < 2)
		channel = RFCOMM_DEFAULT_CHANNEL;
	else if (parse_rfcomm_channel_arg(argv[1], 1, &channel) < 0)
		return 1;

	laddr.rc_channel = channel;

	sk = socket(AF_BLUETOOTH, SOCK_STREAM, BTPROTO_RFCOMM);
	if (sk < 0) {
		perror("Can't create RFCOMM socket");
		return 1;
	}

	if (set_rfcomm_link_mode(sk) < 0) {
		perror("Can't set RFCOMM link mode");
		close(sk);
		return 1;
	}

	if (set_rfcomm_socket_security(sk) < 0) {
		perror("Can't set RFCOMM socket security");
		close(sk);
		return 1;
	}

	if (bind(sk, (struct sockaddr *)&laddr, sizeof(laddr)) < 0) {
		perror("Can't bind RFCOMM socket");
		close(sk);
		return 1;
	}

	alen = sizeof(laddr);
	if (getsockname(sk, (struct sockaddr *)&laddr, &alen) < 0) {
		perror("Can't get RFCOMM socket name");
		close(sk);
		return 1;
	}

	printf("Waiting for connection on channel %d\n", laddr.rc_channel);

	if (register_local_serial_port(&laddr.rc_bdaddr, laddr.rc_channel,
					&sdp_service) < 0) {
		close(sk);
		return 1;
	}

	if (listen(sk, 10) < 0) {
		perror("Can't listen on RFCOMM socket");
		unregister_local_serial_port(&sdp_service);
		close(sk);
		return 1;
	}

	alen = sizeof(raddr);
	nsk = accept(sk, (struct sockaddr *) &raddr, &alen);
	if (nsk < 0) {
		perror("Can't accept RFCOMM connection");
		unregister_local_serial_port(&sdp_service);
		close(sk);
		return 1;
	}

	alen = sizeof(laddr);
	if (getsockname(nsk, (struct sockaddr *)&laddr, &alen) < 0) {
		perror("Can't get RFCOMM socket name");
		unregister_local_serial_port(&sdp_service);
		close(nsk);
		close(sk);
		return 1;
	}

	if (linger) {
		struct linger l = { .l_onoff = 1, .l_linger = linger };

		if (setsockopt(nsk, SOL_SOCKET, SO_LINGER, &l, sizeof(l)) < 0) {
			perror("Can't set linger option");
			unregister_local_serial_port(&sdp_service);
			close(nsk);
			close(sk);
			return 1;
		}
	}

	memset(&req, 0, sizeof(req));
	req.dev_id = dev;
	req.flags = (1 << RFCOMM_REUSE_DLC) | (1 << RFCOMM_RELEASE_ONHUP);

	bacpy(&req.src, &laddr.rc_bdaddr);
	bacpy(&req.dst, &raddr.rc_bdaddr);
	req.channel = raddr.rc_channel;

	dev = ioctl(nsk, RFCOMMCREATEDEV, &req);
	if (dev < 0) {
		perror("Can't create RFCOMM TTY");
		unregister_local_serial_port(&sdp_service);
		close(nsk);
		close(sk);
		return 1;
	}

	snprintf(devname, MAXPATHLEN - 1, "/dev/rfcomm%d", dev);
	while ((fd = open(devname, O_RDONLY | O_NOCTTY)) < 0) {
		if (errno == EACCES) {
			perror("Can't open RFCOMM device");
			goto release;
		}

		snprintf(devname, MAXPATHLEN - 1, "/dev/bluetooth/rfcomm/%d", dev);
		if ((fd = open(devname, O_RDONLY | O_NOCTTY)) < 0) {
			if (try--) {
				snprintf(devname, MAXPATHLEN - 1, "/dev/rfcomm%d", dev);
				usleep(100 * 1000);
				continue;
			}
			perror("Can't open RFCOMM device");
			goto release;
		}
	}

	if (rfcomm_raw_tty) {
		tcflush(fd, TCIOFLUSH);

		cfmakeraw(&ti);
		tcsetattr(fd, TCSANOW, &ti);
	}

	close(sk);
	close(nsk);

	ba2str(&req.dst, dst);
	printf("Connection from %s to %s\n", dst, devname);
	printf("Press CTRL-C for hangup\n");

	memset(&sa, 0, sizeof(sa));
	sa.sa_flags   = SA_NOCLDSTOP;
	sa.sa_handler = SIG_IGN;
	sigaction(SIGCHLD, &sa, NULL);
	sigaction(SIGPIPE, &sa, NULL);

	sa.sa_handler = sig_term;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	sa.sa_handler = sig_hup;
	sigaction(SIGHUP, &sa, NULL);

	sigfillset(&sigs);
	sigdelset(&sigs, SIGCHLD);
	sigdelset(&sigs, SIGPIPE);
	sigdelset(&sigs, SIGTERM);
	sigdelset(&sigs, SIGINT);
	sigdelset(&sigs, SIGHUP);

	p.fd = fd;
	p.events = POLLERR | POLLHUP;

	if (argc <= 2) {
		while (!__io_canceled) {
			p.revents = 0;
			if (ppoll(&p, 1, NULL, &sigs) > 0)
				break;
		}
	} else
		ret = run_cmdline(&p, &sigs, devname, argc - 2, argv + 2);

	sa.sa_handler = NULL;
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);

	printf("Disconnected\n");
	unregister_local_serial_port(&sdp_service);

	close(fd);
	return ret;

release:
	unregister_local_serial_port(&sdp_service);
	memset(&req, 0, sizeof(req));
	req.dev_id = dev;
	req.flags = (1 << RFCOMM_HANGUP_NOW);
	ioctl(ctl, RFCOMMRELEASEDEV, &req);

	close(sk);
	return 1;
}

static int cmd_watch(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	int err = 0;

	while (!__io_canceled) {
		err = cmd_listen(ctl, dev, bdaddr, argc, argv);
		if (err)
			return err;
		usleep(10000);
	}

	return 0;
}

static int cmd_create(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	return create_dev(ctl, dev, 0, bdaddr, argc, argv);
}

static int cmd_release(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	(void) bdaddr;
	(void) argc;

	if (strcmp(argv[0], "all") == 0)
		return release_all(ctl);

	return release_dev(ctl, dev, 0);
}

static int cmd_show(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv)
{
	(void) dev;
	(void) bdaddr;
	(void) argc;

	if (strcmp(argv[0], "all") == 0)
		print_dev_list(ctl, 0);
	else {
		struct rfcomm_dev_info di = { .id = atoi(argv[0]) };
		if (ioctl(ctl, RFCOMMGETDEVINFO, &di) < 0) {
			perror("Get info failed");
			return 1;
		}

		print_dev_info(&di);
	}

	return 0;
}

struct {
	char *cmd;
	char *alt;
	int (*func)(int ctl, int dev, bdaddr_t *bdaddr, int argc, char **argv);
	char *opt;
	char *doc;
} command[] = {
	{ "bind",    "create", cmd_create,  "<dev> <bdaddr> [channel]", "Bind device (SDP if omitted)" },
	{ "release", "unbind", cmd_release, "<dev>",                    "Release device" },
	{ "show",    "info",   cmd_show,    "<dev>",                    "Show device"    },
	{ "connect", "conn",   cmd_connect, "<dev> <bdaddr> [channel]", "Connect device (SDP if omitted)" },
	{ "listen",  "server", cmd_listen,  "<dev> [channel [cmd]]",    "Listen and advertise SPP" },
	{ "watch",   "watch",  cmd_watch,   "<dev> [channel [cmd]]",    "Watch and advertise SPP" },
	{ NULL, NULL, NULL, 0, 0 }
};

static int find_command(const char *name)
{
	int i;

	for (i = 0; command[i].cmd; i++) {
		if (!strcmp(command[i].cmd, name) || !strcmp(command[i].alt, name))
			return i;
	}

	return -1;
}

static void usage(void)
{
	int i;

	printf("RFCOMM configuration utility ver %s\n", VERSION);

	printf("Usage:\n"
		"\trfcomm [options] <command> <dev>\n"
		"\n");

	printf("Options:\n"
		"\t-i, --device [hciX|bdaddr]     Local HCI device or BD Address\n"
		"\t-h, --help                     Display help\n"
		"\t-r, --raw                      Switch TTY into raw mode\n"
		"\t-A, --auth                     Enable authentication\n"
		"\t-E, --encrypt                  Enable encryption\n"
		"\t-S, --secure                   Secure connection\n"
		"\t-C, --central                  Become the central of a piconet\n"
		"\t-L, --linger [seconds]         Set linger timeout\n"
		"\t-n, --service-name name        Match a specific remote SDP service name\n"
		"\t-a                             Show all devices (default)\n"
		"\n"
		"When connect/bind omit [channel], rfcomm resolves the Serial Port RFCOMM\n"
		"channel via SDP. Use --service-name when a peer advertises multiple Serial\n"
		"Port records. listen/watch publish a local Serial Port SDP record while\n"
		"they are serving.\n"
		"\n");

	printf("Commands:\n");
	for (i = 0; command[i].cmd; i++)
		printf("\t%-8s %-24s\t%s\n",
			command[i].cmd,
			command[i].opt ? command[i].opt : " ",
			command[i].doc);
	printf("\n");
}

static struct option main_options[] = {
	{ "help",	0, 0, 'h' },
	{ "device",	1, 0, 'i' },
	{ "raw",	0, 0, 'r' },
	{ "auth",	0, 0, 'A' },
	{ "encrypt",	0, 0, 'E' },
	{ "secure",	0, 0, 'S' },
	{ "master",	0, 0, 'M' }, /* Deprecated. Kept for compatibility. */
	{ "central",	0, 0, 'C' },
	{ "linger",	1, 0, 'L' },
		{ "service-name", 1, 0, 'n' },
	{ 0, 0, 0, 0 }
};

int main(int argc, char *argv[])
{
	bdaddr_t bdaddr;
	const char *progname;
	unsigned long linger_value;
	int cmd_index, opt, ctl, dev_id, show_all = 0;

	setvbuf(stdout, NULL, _IOLBF, 0);

	progname = strrchr(argv[0], '/');
	progname = progname ? progname + 1 : argv[0];

	if (strstr(progname, "exitfix") != NULL)
		connect_timeout_ms = 9000;

	bacpy(&bdaddr, BDADDR_ANY);

	while ((opt = getopt_long(argc, argv, "+i:rahAESMCL:", main_options,
								NULL)) != -1) {
		switch(opt) {
		case 'i':
			if (strncmp(optarg, "hci", 3) == 0) {
				if (parse_hci_device_arg(optarg, &bdaddr) < 0)
					exit(1);
			} else if (parse_bdaddr_arg(optarg, &bdaddr) < 0) {
				exit(1);
			}
			break;

		case 'r':
			rfcomm_raw_tty = 1;
			break;

		case 'a':
			show_all = 1;
			break;

		case 'h':
			usage();
			exit(0);

		case 'A':
			auth = 1;
			break;

		case 'E':
			encryption = 1;
			break;

		case 'S':
			secure = 1;
			break;

		case 'M': /* Deprecated. Kept for compatibility. */
		case 'C':
			central = 1;
			break;

		case 'L':
			if (parse_uint_arg(optarg, "linger seconds", 0,
						ULONG_MAX > INT_MAX ? INT_MAX : ULONG_MAX,
						&linger_value) < 0)
				exit(1);
			linger = linger_value;
			break;

		case 'n':
			service_name = optarg;
			break;
		default:
			exit(1);
		}
	}

	argc -= optind;
	argv += optind;
	optind = 0;

	if (argc < 2) {
		if (argc != 0) {
			usage();
			exit(1);
		} else
			show_all = 1;
	}

	cmd_index = -1;
	if (!show_all) {
		cmd_index = find_command(argv[0]);
		if (cmd_index < 0) {
			usage();
			exit(1);
		}
	}

	ctl = socket(AF_BLUETOOTH, SOCK_RAW, BTPROTO_RFCOMM);
	if (ctl < 0) {
		perror("Can't open RFCOMM control socket");
		exit(1);
	}

	if (show_all) {
		print_dev_list(ctl, 0);
		close(ctl);
		exit(0);
	}

	if (strcmp(argv[1], "all") == 0)
		dev_id = 0;
	else if (parse_rfcomm_dev_id(argv[1], &dev_id) < 0) {
		close(ctl);
		exit(1);
	}

	argc--;
	argv++;
	opt = command[cmd_index].func(ctl, dev_id, &bdaddr, argc, argv);

	close(ctl);

	return opt == 0 ? 0 : 1;
}
