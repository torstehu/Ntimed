/*-
 * Copyright (c) 2014 Poul-Henning Kamp
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>		/* Compat for NetBSD */
#include <sys/types.h>		/* Compat for OpenBSD */
#include <sys/socket.h>

#include "ntimed.h"
#include "udp.h"

struct udp_socket {
	unsigned		magic;
#define UDP_SOCKET_MAGIC	0x302a563f

	int			fd4;
	int			fd6;
};

static int
udp_sock(int fam)
{
	int fd;
	int i;

	fd = socket(fam, SOCK_DGRAM, 0);
	if (fd < 0)
		return (fd);

#ifdef IP_RECVDSTADDR
	i = 1;
	if (setsockopt(fd, IPPROTO_IP, IP_RECVDSTADDR, &i, sizeof i) != 0) {
		AZ(close(fd));
		Fail(ocx, 1, "setsockopt(IP_RECVDSTADDR) failed");
	}
#endif
#ifdef SO_TIMESTAMPNS
	i = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPNS, &i, sizeof i);
#elif defined(SO_TIMESTAMP)
	i = 1;
	(void)setsockopt(fd, SOL_SOCKET, SO_TIMESTAMP, &i, sizeof i);
#endif
	return (fd);
}

struct udp_socket *
UdpTimedSocket(struct ocx *ocx)
{
	struct udp_socket *usc;

	ALLOC_OBJ(usc, UDP_SOCKET_MAGIC);
	AN(usc);
	usc->fd4 = udp_sock(AF_INET);
	usc->fd6 = udp_sock(AF_INET6);
	if (usc->fd4 < 0 && usc->fd6 < 0)
		Fail(ocx, 1, "socket(2) failed");
	return (usc);
}

ssize_t
UdpTimedRx(struct ocx *ocx, const struct udp_socket *usc,
    struct sockaddr_storage *ss, socklen_t *sl,
    struct timestamp *ts, void *buf, ssize_t len, double tmo)
{
	struct msghdr msg;
	struct iovec iov;
	struct cmsghdr *cmsg;
	u_char ctrl[1024];
	ssize_t rl;
	int i, j;
	int tmo_msec;
	struct pollfd pfd[2];

	CHECK_OBJ_NOTNULL(usc, UDP_SOCKET_MAGIC);
	AN(ss);
	AN(sl);
	AN(ts);
	AN(buf);
	assert(len > 0);


	j = 0;
	if (usc->fd4 >= 0) {
		pfd[j].fd = usc->fd4;
		pfd[j].events = POLLIN;
		pfd[j++].revents = 0;
	}
	if (usc->fd6 >= 0) {
		pfd[j].fd = usc->fd6;
		pfd[j].events = POLLIN;
		pfd[j++].revents = 0;
	}

	if (tmo == 0.0) {
		tmo_msec = -1;
	} else {
		tmo_msec = lround(1e3 * tmo);
		if (tmo_msec <= 0)
			tmo_msec = 0;
	}
	i = poll(pfd, j, tmo_msec);

	if (i < 0)
		Fail(ocx, 1, "poll(2) failed\n");

	if (i == 0)
		return (0);

	memset(&msg, 0, sizeof msg);
	msg.msg_name = (void*)ss;
	msg.msg_namelen = sizeof *ss;
	msg.msg_iov = &iov;
	msg.msg_iovlen = 1;
	msg.msg_control = ctrl;
	msg.msg_controllen = sizeof ctrl;
	iov.iov_base = buf;
	iov.iov_len = (size_t)len;
	memset(ctrl, 0, sizeof ctrl);
	cmsg = (void*)ctrl;

	for (i = 0; i < j; i++)
		if (pfd[i].revents & POLLIN)
			break;

	if (i == j)
		return (0);

	rl = recvmsg(pfd[i].fd, &msg, 0);
	if (rl <= 0)
		return (rl);

	*sl = msg.msg_namelen;

	for(;cmsg != NULL; cmsg = CMSG_NXTHDR(&msg, cmsg)) {
#ifdef SCM_TIMESTAMPNS
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_TIMESTAMPNS &&
		    cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval))) {
			struct timespec tsc;
			memcpy(&tsc, CMSG_DATA(cmsg), sizeof tsc);
			(void)TS_Nanosec(ts, tsc.tv_sec, tsc.tv_nsec);
			continue;
		}
#endif
#ifdef SCM_TIMESTAMP
		if (cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_TIMESTAMP &&
		    cmsg->cmsg_len == CMSG_LEN(sizeof(struct timeval))) {
			struct timeval tv;
			memcpy(&tv, CMSG_DATA(cmsg), sizeof tv);
			(void)TS_Nanosec(ts, tv.tv_sec, tv.tv_usec * 1000LL);
			continue;
		}
#endif
#ifdef IP_RECVDSTADDR
		if (cmsg->cmsg_level == IPPROTO_IP &&
		    cmsg->cmsg_type == IP_RECVDSTADDR &&
		    cmsg->cmsg_len == CMSG_LEN(sizeof(in_addr_t))) {
			continue;
			/* XXX */
		}
#endif
		Debug(ocx, "RX-msg: %d %d %u ",
		    cmsg->cmsg_level, cmsg->cmsg_type, cmsg->cmsg_len);
		DebugHex(ocx, CMSG_DATA(cmsg), cmsg->cmsg_len);
		Debug(ocx, "\n");

	}
	return (rl);
}

ssize_t
Udp_Send(struct ocx *ocx, const struct udp_socket *usc,
    const void *ss, socklen_t sl, const void *buf, ssize_t len)
{
	const struct sockaddr *sa;

	(void)ocx;
	CHECK_OBJ_NOTNULL(usc, UDP_SOCKET_MAGIC);
	AN(ss);
	AN(sl);
	AN(buf);
	AN(len);
	sa = ss;
	if (sa->sa_family == AF_INET)
		return (sendto(usc->fd4, buf, len, 0, ss, sl));
	if (sa->sa_family == AF_INET6)
		return (sendto(usc->fd6, buf, len, 0, ss, sl));

	WRONG("Wrong AF_");
	return (0);
}
