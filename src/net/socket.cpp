// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/socket.h"
#include <algorithm>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/uio.h>
#endif
namespace hostnet {
int LastSocketError() {
#ifdef _WIN32
	return WSAGetLastError();
#else
	// Nonblocking connect has the same pending result as Winsock's EWOULDBLOCK.
	return errno == EINPROGRESS ? EWOULDBLOCK : errno;
#endif
}
int CloseSocketHandle(SocketHandle s) {
#ifdef _WIN32
	return closesocket(s);
#else
	return close(s);
#endif
}
int SocketIoctl(SocketHandle s, long cmd, unsigned long *v) {
#ifdef _WIN32
	return ioctlsocket(s, cmd, v);
#else
	int value = (int)*v;
	int rc = ioctl(s, cmd, &value);
	*v = (unsigned int)value; return rc;
#endif
}
int Poll(PollFd *fds, size_t count, int ms) {
#ifdef _WIN32
	return WSAPoll(fds, (ULONG)count, ms);
#else
	return poll(fds, count, ms);
#endif
}
#ifdef _WIN32
using SockLen = int;
#else
using SockLen = socklen_t;
#endif
SocketHandle Accept(SocketHandle s, sockaddr *addr, int *len) {
	SockLen n = len ? *len : 0;
	auto result = accept(s, addr, len ? &n : nullptr);
	if (len) *len = (int)n;
	return result;
}
int GetSockName(SocketHandle s, sockaddr *addr, int *len) {
	SockLen n = *len; int rc = getsockname(s, addr, &n); *len = (int)n; return rc;
}
int GetPeerName(SocketHandle s, sockaddr *addr, int *len) {
	SockLen n = *len; int rc = getpeername(s, addr, &n); *len = (int)n; return rc;
}
int GetSockOpt(SocketHandle s, int level, int name, char *value, int *len) {
	SockLen n = *len; int rc = getsockopt(s, level, name, value, &n); *len = (int)n; return rc;
}
int RecvFrom(SocketHandle s, char *buf, int size, int flags, sockaddr *addr, int *len) {
#ifdef _WIN32
	SockLen n = len ? *len : 0;
	int rc = (int)recvfrom(s, buf, size, flags, addr, len ? &n : nullptr);
	if (len) *len = (int)n;
#else
	iovec iov{buf, (size_t)size};
	msghdr message{};
	message.msg_name = addr;
	message.msg_namelen = len ? *len : 0;
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	int rc = (int)recvmsg(s, &message, flags);
	if (len) *len = (int)message.msg_namelen;
	// POSIX recvfrom loses this indication; report the same copied-prefix error as Winsock.
	if (rc >= 0 && (message.msg_flags & MSG_TRUNC)) {
		errno = EMSGSIZE;
		rc = SocketError;
	}
#endif
	return rc;
}
int Send(SocketHandle s, const char *buf, int size, int flags) {
#ifndef _WIN32
	flags |= MSG_NOSIGNAL;
#endif
	return (int)send(s, buf, size, flags);
}
int SendTo(SocketHandle s, const char *buf, int size, int flags, const sockaddr *addr, int len) {
#ifndef _WIN32
	flags |= MSG_NOSIGNAL;
#endif
	return (int)sendto(s, buf, size, flags, addr, len);
}
#ifdef _WIN32
EventHandle NewEvent(bool manual) { return CreateEventW(nullptr, manual, FALSE, nullptr); }
void SignalEvent(EventHandle e) { SetEvent(e); }
void ResetEvent(EventHandle e) { ::ResetEvent(e); }
void CloseEvent(EventHandle e) { if (e) CloseHandle(e); }
int WaitEvents(const EventHandle *events, size_t count, int ms) {
	DWORD r = WaitForMultipleObjects((DWORD)count, events, FALSE, ms < 0 ? INFINITE : (DWORD)ms);
	return r == WAIT_TIMEOUT ? -1 : r == WAIT_FAILED ? -2 : (int)(r - WAIT_OBJECT_0);
}
EventHandle SocketEvent(SocketHandle s) {
	WSAEVENT e = WSACreateEvent();
	if (e == WSA_INVALID_EVENT) return nullptr;
	if (WSAEventSelect(s, e, FD_READ | FD_WRITE | FD_OOB | FD_ACCEPT | FD_CONNECT | FD_CLOSE)) {
		WSACloseEvent(e); return nullptr;
	}
	return e;
}
#else
namespace {
struct Event { int fd; bool manual; bool socket; };
}
EventHandle NewEvent(bool manual) {
	int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	return fd < 0 ? nullptr : new Event{fd, manual, false};
}
void SignalEvent(EventHandle handle) {
	if (!handle) return;
	u64 one = 1;
	ssize_t rc;
	do { rc = write(((Event *)handle)->fd, &one, sizeof(one)); } while (rc < 0 && errno == EINTR);
}
void ResetEvent(EventHandle handle) {
	if (!handle) return;
	auto &e = *(Event *)handle;
	if (e.socket) {
		epoll_event events[8];
		while (epoll_wait(e.fd, events, 8, 0) > 0) {}
	} else {
		u64 n;
		while (read(e.fd, &n, sizeof(n)) < 0 && errno == EINTR) {}
	}
}
void CloseEvent(EventHandle handle) {
	if (!handle) return;
	auto *e = (Event *)handle;
	close(e->fd); delete e;
}
int WaitEvents(const EventHandle *events, size_t count, int ms) {
	std::vector<pollfd> fds;
	for (size_t i = 0; i < count; ++i) fds.push_back({events[i] ? ((Event *)events[i])->fd : -1, POLLIN, 0});
	int rc = poll(fds.data(), fds.size(), ms);
	if (rc <= 0) return rc == 0 || errno == EINTR ? -1 : -2;
	for (size_t i = 0; i < fds.size(); ++i) if (fds[i].revents) {
		if (fds[i].revents & POLLNVAL) return -2;
		auto &e = *(Event *)events[i];
		if (!e.manual && !e.socket) ResetEvent(events[i]);
		return (int)i;
	}
	return -1;
}
EventHandle SocketEvent(SocketHandle s) {
	int flags = fcntl(s, F_GETFL);
	if (flags < 0 || fcntl(s, F_SETFL, flags | O_NONBLOCK) || fcntl(s, F_SETFD, FD_CLOEXEC)) return nullptr;
	int fd = epoll_create1(EPOLL_CLOEXEC);
	if (fd < 0) return nullptr;
	epoll_event event{};
	event.events = EPOLLIN | EPOLLOUT | EPOLLPRI | EPOLLRDHUP | EPOLLET;
	if (epoll_ctl(fd, EPOLL_CTL_ADD, s, &event)) { close(fd); return nullptr; }
	return new Event{fd, true, true};
}
#endif
}
