// SPDX-License-Identifier: GPL-2.0-or-later
//
// See adhoc_server.h. The user / game / group bookkeeping and the protocol handling are a port of
// PPSSPP's Core/HLE/proAdhocServer.cpp (Copyright (c) 2014- PPSSPP Project, GPL-2.0-or-later; a port
// of Coldbird's aemu server), restructured around WSAPoll and C++ containers. List orders match the
// original's linked lists (newest first), which decide the order of the CONNECT / SCAN replies and
// which player counts as a group's founder (BSSID).
#include "net/adhoc_server.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "common/log.h"
#include "net/adhoc_proto.h"
#include "net/host_net.h"

namespace adhocserver {

using namespace adhocproto;

namespace {

constexpr int kListenBacklog = 128;
constexpr size_t kMaxUsers = 1024;       // SERVER_USER_MAXIMUM
constexpr u64 kUserTimeoutMs = 15000;    // SERVER_USER_TIMEOUT: no packet (pings included) for 15 s
constexpr size_t kMaxTxBacklog = 1 << 20;
constexpr const char *kShutdownMessage = "ADHOC SERVER HUB IS SHUTTING DOWN!";

std::string MacStr(const u8 *m) {
	char b[18];
	snprintf(b, sizeof(b), "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
	return b;
}

std::string Fixed(const void *p, size_t n) {
	const char *c = (const char *)p;
	size_t len = 0;
	while (len < n && c[len]) len++;
	return std::string(c, len);
}

struct Game;
struct Group;

struct User {
	SOCKET s = INVALID_SOCKET;
	u32 ip = 0;  // network byte order
	u8 mac[kMacLen] = {};
	u8 name[kNicknameLen] = {};
	Game *game = nullptr;    // set by LOGIN
	Group *group = nullptr;  // set by CONNECT
	u64 lastRecvMs = 0;
	std::string rx, tx;
	bool dead = false;
};

struct Group {
	Game *game = nullptr;
	u8 name[kGroupNameLen] = {};
	std::vector<User *> players;  // newest first; the last one founded the group (its BSSID)
};

struct Game {
	char code[kProductCodeLen] = {};
	int playerCount = 0;
	std::vector<std::unique_ptr<Group>> groups;  // newest first
};

}  // namespace

struct Server::Impl {
	Options opt;
	SOCKET listener = INVALID_SOCKET;
	u16 port = 0;
	u32 bindAddr = 0;
	std::atomic<bool> stop{false};
	std::thread thread;
	std::vector<std::unique_ptr<User>> users;  // newest first
	std::vector<std::unique_ptr<Game>> games;  // newest first
	std::atomic<int> userCount{0}, groupCount{0};

	void Log(const char *fmt, ...) {
		char buf[512];
		va_list ap;
		va_start(ap, fmt);
		vsnprintf(buf, sizeof(buf), fmt, ap);
		va_end(ap);
		if (opt.log) opt.log(buf);
		else LOGI("ADHOCSRV", "%s", buf);
	}

	std::string Who(const User *u) {
		char b[256];
		snprintf(b, sizeof(b), "%s (MAC: %s - IP: %s)", Fixed(u->name, kNicknameLen).c_str(), MacStr(u->mac).c_str(),
		         hostnet::Ipv4ToString(u->ip).c_str());
		return b;
	}

	void UpdateCounts() {
		int g = 0;
		for (auto &game : games) g += (int)game->groups.size();
		groupCount = g;
		int n = 0;
		for (auto &u : users)
			if (!u->dead) n++;
		userCount = n;
	}

	// Queues bytes for a user; sends what the socket takes now.
	void Send(User *u, const void *data, size_t len) {
		if (u->dead || u->s == INVALID_SOCKET) return;
		u->tx.append((const char *)data, len);
		Flush(u);
	}

	void Flush(User *u) {
		while (!u->tx.empty() && !u->dead) {
			int n = send(u->s, u->tx.data(), (int)std::min<size_t>(u->tx.size(), 0x10000), 0);
			if (n > 0) {
				u->tx.erase(0, (size_t)n);
				continue;
			}
			int e = WSAGetLastError();
			if (n == SOCKET_ERROR && e == WSAEWOULDBLOCK) break;
			Log("Send error %d to %s", e, Who(u).c_str());
			u->tx.clear();
			break;  // the receive side notices the broken connection
		}
		if (u->tx.size() > kMaxTxBacklog) {
			Log("%s does not read its data: dropping", Who(u).c_str());
			Logout(u);
		}
	}

	// ---- database (login_user_stream / login_user_data / logout_user)

	void LoginStream(SOCKET s, u32 ip) {
		size_t alive = 0;
		bool dupIp = false;
		for (auto &u : users) {
			if (u->dead) continue;
			alive++;
			if (u->ip == ip) dupIp = true;
		}
		if (alive >= kMaxUsers || dupIp) {
			if (dupIp) Log("Already existing IP: %s", hostnet::Ipv4ToString(ip).c_str());
			closesocket(s);
			return;
		}
		auto u = std::make_unique<User>();
		u->s = s;
		u->ip = ip;
		u->lastRecvMs = GetTickCount64();
		Log("New connection from %s", hostnet::Ipv4ToString(ip).c_str());
		users.insert(users.begin(), std::move(u));
	}

	void LoginData(User *u, const LoginC2S &p) {
		bool validCode = true;
		for (int i = 0; i < kProductCodeLen; i++) {
			char c = p.game[i];
			if (!((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'))) validCode = false;
		}
		static const u8 kZero[kMacLen] = {}, kBcast[kMacLen] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
		if (!validCode || !memcmp(p.mac, kZero, kMacLen) || !memcmp(p.mac, kBcast, kMacLen) || p.name[0] == 0) {
			Log("Invalid login packet contents from %s", hostnet::Ipv4ToString(u->ip).c_str());
			Logout(u);
			return;
		}
		for (auto &o : users)
			if (o.get() != u && !o->dead && !memcmp(o->mac, p.mac, kMacLen))
				Log("Already existing MAC: %s [%s]", MacStr(p.mac).c_str(), hostnet::Ipv4ToString(o->ip).c_str());
		Game *game = nullptr;
		for (auto &g : games)
			if (!strncmp(g->code, p.game, kProductCodeLen)) game = g.get();
		if (!game) {
			auto g = std::make_unique<Game>();
			memcpy(g->code, p.game, kProductCodeLen);
			game = g.get();
			games.insert(games.begin(), std::move(g));
		}
		memcpy(u->mac, p.mac, kMacLen);
		memcpy(u->name, p.name, kNicknameLen);
		u->name[kNicknameLen - 1] = 0;
		game->playerCount++;
		u->game = game;
		Log("%s started playing %s", Who(u).c_str(), Fixed(game->code, kProductCodeLen).c_str());
	}

	void Logout(User *u) {
		if (u->dead) return;
		if (u->group) Disconnect(u);
		if (u->dead) return;
		u->dead = true;
		if (u->s != INVALID_SOCKET) closesocket(u->s);
		u->s = INVALID_SOCKET;
		if (u->game) {
			Log("%s stopped playing %s", Who(u).c_str(), Fixed(u->game->code, kProductCodeLen).c_str());
			if (--u->game->playerCount == 0) {
				Game *g = u->game;
				games.erase(std::remove_if(games.begin(), games.end(), [g](const std::unique_ptr<Game> &x) { return x.get() == g; }),
				            games.end());
			}
			u->game = nullptr;
		} else {
			Log("Dropped connection to %s", hostnet::Ipv4ToString(u->ip).c_str());
		}
	}

	// ---- groups (connect_user / disconnect_user / send_scan_results / spread_message)

	void Connect(User *u, const u8 *groupName) {
		if (!ValidGroupName(groupName)) {
			Log("%s attempted to join invalid %s group %s", Who(u).c_str(), Fixed(u->game->code, kProductCodeLen).c_str(),
			    Fixed(groupName, kGroupNameLen).c_str());
			Logout(u);
			return;
		}
		if (u->group) {
			Log("%s attempted to join %s group %s without disconnecting from %s first", Who(u).c_str(),
			    Fixed(u->game->code, kProductCodeLen).c_str(), Fixed(groupName, kGroupNameLen).c_str(),
			    Fixed(u->group->name, kGroupNameLen).c_str());
			Logout(u);
			return;
		}
		Group *g = nullptr;
		for (auto &x : u->game->groups)
			if (!strncmp((const char *)x->name, (const char *)groupName, kGroupNameLen)) g = x.get();
		if (!g) {
			auto ng = std::make_unique<Group>();
			ng->game = u->game;
			memcpy(ng->name, groupName, kGroupNameLen);
			g = ng.get();
			u->game->groups.insert(u->game->groups.begin(), std::move(ng));
		}
		ConnectBssidS2C bssid{};
		bssid.opcode = OPCODE_CONNECT_BSSID;
		memcpy(bssid.mac, u->mac, kMacLen);
		for (size_t i = 0; i < g->players.size(); i++) {
			User *peer = g->players[i];
			ConnectS2C pk{};
			pk.opcode = OPCODE_CONNECT;
			memcpy(pk.name, u->name, kNicknameLen);
			memcpy(pk.mac, u->mac, kMacLen);
			pk.ip = u->ip;
			Send(peer, &pk, sizeof(pk));
			memcpy(pk.name, peer->name, kNicknameLen);
			memcpy(pk.mac, peer->mac, kMacLen);
			pk.ip = peer->ip;
			Send(u, &pk, sizeof(pk));
			if (i + 1 == g->players.size()) memcpy(bssid.mac, peer->mac, kMacLen);  // the founder
		}
		g->players.insert(g->players.begin(), u);
		u->group = g;
		Send(u, &bssid, sizeof(bssid));
		Log("%s joined %s group %s", Who(u).c_str(), Fixed(u->game->code, kProductCodeLen).c_str(), Fixed(g->name, kGroupNameLen).c_str());
	}

	void Disconnect(User *u) {
		Group *g = u->group;
		if (!g) {
			Log("%s attempted to leave %s group without joining one first", Who(u).c_str(), Fixed(u->game->code, kProductCodeLen).c_str());
			Logout(u);
			return;
		}
		g->players.erase(std::remove(g->players.begin(), g->players.end(), u), g->players.end());
		for (User *peer : g->players) {
			DisconnectS2C pk{};
			pk.opcode = OPCODE_DISCONNECT;
			pk.ip = u->ip;
			Send(peer, &pk, sizeof(pk));
		}
		Log("%s left %s group %s", Who(u).c_str(), Fixed(u->game->code, kProductCodeLen).c_str(), Fixed(g->name, kGroupNameLen).c_str());
		u->group = nullptr;
		if (g->players.empty()) {
			auto &gs = g->game->groups;
			gs.erase(std::remove_if(gs.begin(), gs.end(), [g](const std::unique_ptr<Group> &x) { return x.get() == g; }), gs.end());
		}
	}

	void Scan(User *u) {
		if (u->group) {
			Log("%s attempted to scan for %s groups without disconnecting from %s first", Who(u).c_str(),
			    Fixed(u->game->code, kProductCodeLen).c_str(), Fixed(u->group->name, kGroupNameLen).c_str());
			Logout(u);
			return;
		}
		for (auto &g : u->game->groups) {
			ScanS2C pk{};
			pk.opcode = OPCODE_SCAN;
			memcpy(pk.group, g->name, kGroupNameLen);
			if (!g->players.empty()) memcpy(pk.mac, g->players.back()->mac, kMacLen);
			Send(u, &pk, sizeof(pk));
		}
		u8 done = OPCODE_SCAN_COMPLETE;
		Send(u, &done, 1);
		Log("%s requested information on %d %s groups", Who(u).c_str(), (int)u->game->groups.size(),
		    Fixed(u->game->code, kProductCodeLen).c_str());
	}

	void Chat(User *u, const char *message) {
		ChatS2C pk{};
		pk.opcode = OPCODE_CHAT;
		strncpy(pk.message, message, kChatLen - 1);
		if (!u) {
			// A notice to everyone in a group (shutdown).
			for (auto &x : users)
				if (!x->dead && x->group) Send(x.get(), &pk, sizeof(pk));
			return;
		}
		if (!u->group) {
			Log("%s attempted to send a text message without joining a %s group first", Who(u).c_str(),
			    Fixed(u->game->code, kProductCodeLen).c_str());
			Logout(u);
			return;
		}
		memcpy(pk.name, u->name, kNicknameLen);
		int n = 0;
		for (User *peer : u->group->players) {
			if (peer == u) continue;
			Send(peer, &pk, sizeof(pk));
			n++;
		}
		if (n) Log("%s sent \"%s\" to %d players in %s group %s", Who(u).c_str(), pk.message, n, Fixed(u->game->code, kProductCodeLen).c_str(),
		           Fixed(u->group->name, kGroupNameLen).c_str());
	}

	// ---- the loop

	// Handles the complete packets in a user's receive buffer.
	void Process(User *u) {
		while (!u->dead && !u->rx.empty()) {
			u8 op = (u8)u->rx[0];
			if (!u->game) {
				// Waiting for the login packet.
				if (op != OPCODE_LOGIN) {
					Log("Invalid opcode 0x%02X in waiting state from %s", op, hostnet::Ipv4ToString(u->ip).c_str());
					Logout(u);
					return;
				}
				if (u->rx.size() < sizeof(LoginC2S)) return;
				LoginC2S p;
				memcpy(&p, u->rx.data(), sizeof(p));
				u->rx.erase(0, sizeof(p));
				LoginData(u, p);
				continue;
			}
			size_t need = C2SPacketSize(op);
			if (op == OPCODE_LOGIN || need == 0) {
				Log("Invalid opcode 0x%02X in logged-in state from %s", op, Who(u).c_str());
				Logout(u);
				return;
			}
			if (u->rx.size() < need) return;
			std::string pk = u->rx.substr(0, need);
			u->rx.erase(0, need);
			switch (op) {
			case OPCODE_PING: break;
			case OPCODE_CONNECT: {
				u8 group[kGroupNameLen];
				memcpy(group, pk.data() + 1, kGroupNameLen);
				Connect(u, group);
				break;
			}
			case OPCODE_DISCONNECT: Disconnect(u); break;
			case OPCODE_SCAN: Scan(u); break;
			case OPCODE_CHAT: {
				char msg[kChatLen] = {};
				memcpy(msg, pk.data() + 1, kChatLen - 1);
				Chat(u, msg);
				break;
			}
			}
		}
	}

	void AcceptAll() {
		for (;;) {
			sockaddr_in a{};
			int len = sizeof(a);
			SOCKET s = accept(listener, (sockaddr *)&a, &len);
			if (s == INVALID_SOCKET) break;
			u_long on = 1;
			ioctlsocket(s, FIONBIO, &on);
			BOOL nodelay = TRUE;
			setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&nodelay, sizeof(nodelay));
			BOOL keepalive = TRUE;
			setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, (const char *)&keepalive, sizeof(keepalive));
			LoginStream(s, a.sin_addr.s_addr);
		}
	}

	void ReceiveFrom(User *u) {
		char buf[4096];
		for (;;) {
			int n = recv(u->s, buf, sizeof(buf), 0);
			if (n > 0) {
				u->rx.append(buf, (size_t)n);
				u->lastRecvMs = GetTickCount64();
				if (u->rx.size() > 0x10000) {
					Logout(u);
					return;
				}
				continue;
			}
			int e = WSAGetLastError();
			if (n == SOCKET_ERROR && e == WSAEWOULDBLOCK) return;
			Logout(u);  // closed, or broken
			return;
		}
	}

	void Loop() {
		std::vector<WSAPOLLFD> fds;
		std::vector<User *> polled;
		while (!stop) {
			fds.clear();
			polled.clear();
			WSAPOLLFD l{};
			l.fd = listener;
			l.events = POLLRDNORM;
			fds.push_back(l);
			for (auto &u : users) {
				if (u->dead) continue;
				WSAPOLLFD f{};
				f.fd = u->s;
				f.events = POLLRDNORM | (u->tx.empty() ? 0 : POLLWRNORM);
				fds.push_back(f);
				polled.push_back(u.get());
			}
			int r = WSAPoll(fds.data(), (ULONG)fds.size(), 20);
			if (r == SOCKET_ERROR) {
				Sleep(10);
				continue;
			}
			if (fds[0].revents) AcceptAll();
			u64 now = GetTickCount64();
			for (size_t i = 0; i < polled.size(); i++) {
				User *u = polled[i];
				if (u->dead) continue;
				short re = fds[i + 1].revents;
				if (re & (POLLRDNORM | POLLHUP | POLLERR)) ReceiveFrom(u);
				if (u->dead) continue;
				Process(u);
				if (u->dead) continue;
				if (now - u->lastRecvMs >= kUserTimeoutMs) {
					Log("%s timed out", Who(u).c_str());
					Logout(u);
					continue;
				}
				if (!u->tx.empty()) Flush(u);
			}
			users.erase(std::remove_if(users.begin(), users.end(), [](const std::unique_ptr<User> &u) { return u->dead; }), users.end());
			UpdateCounts();
		}
		// free_database: tell the players, then log everybody out.
		bool anyone = false;
		for (auto &u : users)
			if (!u->dead) anyone = true;
		if (anyone) Chat(nullptr, kShutdownMessage);
		for (auto &u : users) {
			if (!u->dead && !u->tx.empty()) Flush(u.get());
			Logout(u.get());
		}
		users.clear();
		games.clear();
		UpdateCounts();
		if (listener != INVALID_SOCKET) closesocket(listener);
		listener = INVALID_SOCKET;
		Log("Server stopped");
	}
};

Server::Server() : impl_(std::make_unique<Impl>()) {}

Server::~Server() { Stop(); }

bool Server::Listen(const Options &options, std::string *error) {
	Impl &m = *impl_;
	m.opt = options;
	if (!hostnet::Startup()) {
		if (error) *error = "Winsock is not available";
		return false;
	}
	u32 addr = 0;
	if (!hostnet::ParseIpv4(options.bindAddress.c_str(), &addr)) {
		if (error) *error = "bad bind address " + options.bindAddress;
		return false;
	}
	SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		if (error) *error = "socket() failed: " + std::to_string(WSAGetLastError());
		return false;
	}
	// Exclusive: no other program can take over the port while the server runs.
	BOOL excl = TRUE;
	setsockopt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *)&excl, sizeof(excl));
	sockaddr_in a{};
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = addr;
	a.sin_port = htons(options.port);
	if (bind(s, (const sockaddr *)&a, sizeof(a)) != 0 || listen(s, kListenBacklog) != 0) {
		int e = WSAGetLastError();
		closesocket(s);
		if (error) {
			*error = "cannot listen on " + options.bindAddress + ":" + std::to_string(options.port) + " (error " + std::to_string(e) +
			         (e == WSAEADDRINUSE ? ": the port is in use - is another ad hoc server running?" : "") + ")";
		}
		return false;
	}
	u_long on = 1;
	ioctlsocket(s, FIONBIO, &on);
	int len = sizeof(a);
	getsockname(s, (sockaddr *)&a, &len);
	m.listener = s;
	m.port = ntohs(a.sin_port);
	m.bindAddr = addr;
	m.stop = false;
	return true;
}

void Server::Run() {
	if (impl_->listener == INVALID_SOCKET) return;
	impl_->Loop();
}

bool Server::Start(const Options &options, std::string *error) {
	if (!Listen(options, error)) return false;
	impl_->thread = std::thread([this] { Run(); });
	return true;
}

void Server::Stop() {
	impl_->stop = true;
	if (impl_->thread.joinable() && impl_->thread.get_id() != std::this_thread::get_id()) impl_->thread.join();
}

u16 Server::Port() const { return impl_->port; }

std::vector<std::string> Server::Addresses() const {
	std::vector<std::string> out;
	if (impl_->bindAddr != 0) {
		out.push_back(hostnet::Ipv4ToString(impl_->bindAddr));
		return out;
	}
	ULONG size = 16384;
	std::vector<u8> buf(size);
	ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
	ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	if (r == ERROR_BUFFER_OVERFLOW) {
		buf.resize(size);
		r = GetAdaptersAddresses(AF_INET, flags, nullptr, (IP_ADAPTER_ADDRESSES *)buf.data(), &size);
	}
	if (r == NO_ERROR) {
		for (auto *ad = (IP_ADAPTER_ADDRESSES *)buf.data(); ad; ad = ad->Next) {
			if (ad->OperStatus != IfOperStatusUp) continue;
			for (auto *ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
				if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
				u32 ip = ((sockaddr_in *)ua->Address.lpSockaddr)->sin_addr.s_addr;
				if (hostnet::IsLoopback(ip)) continue;
				out.push_back(hostnet::Ipv4ToString(ip));
			}
		}
	}
	out.push_back("127.0.0.1");
	return out;
}

int Server::UserCount() const { return impl_->userCount; }
int Server::GroupCount() const { return impl_->groupCount; }

// ---------------------------------------------------------------- pspc --adhoc-server

namespace {

Server *g_cmdServer = nullptr;
HANDLE g_cmdDone = nullptr;

BOOL WINAPI ConsoleHandler(DWORD type) {
	if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT ||
	    type == CTRL_SHUTDOWN_EVENT) {
		if (g_cmdServer) g_cmdServer->Stop();
		// Let the server say goodbye to the players before the process goes.
		if (g_cmdDone) WaitForSingleObject(g_cmdDone, 3000);
		return TRUE;
	}
	return FALSE;
}

void PrintLine(const std::string &s) {
	time_t t = time(nullptr);
	struct tm lt;
	localtime_s(&lt, &t);
	printf("[%02d:%02d:%02d] %s\n", lt.tm_hour, lt.tm_min, lt.tm_sec, s.c_str());
	fflush(stdout);
}

}  // namespace

int RunCommandLine(const Options &options) {
	Options o = options;
	if (!o.log) o.log = PrintLine;
	Server server;
	std::string err;
	if (!server.Listen(o, &err)) {
		fprintf(stderr, "pspc --adhoc-server: %s\n", err.c_str());
		return 1;
	}
	g_cmdServer = &server;
	g_cmdDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	SetConsoleCtrlHandler(ConsoleHandler, TRUE);
	printf("PSPC ad hoc server (PPSSPP-compatible) listening on TCP port %u\n", server.Port());
	printf("Players set adhoc_server= in pspc.ini (PPSSPP: Settings > Networking > Ad hoc server) to one of:\n");
	for (const std::string &a : server.Addresses()) {
		if (server.Port() == kServerPort) printf("  %s\n", a.c_str());
		else printf("  %s:%u\n", a.c_str(), server.Port());
	}
	printf("Press Ctrl+C to stop.\n");
	fflush(stdout);
	server.Run();
	SetConsoleCtrlHandler(ConsoleHandler, FALSE);
	g_cmdServer = nullptr;
	SetEvent(g_cmdDone);
	return 0;
}

}  // namespace adhocserver
