// SPDX-License-Identifier: GPL-2.0-or-later
// Real guest HLE and loopback UDP, without game assets or persistent runtime configuration.
#include "allegrex/guestmem.h"
#include "net/host_net.h"
#include "runtime/core/memory.h"
#include "runtime/hle/hle.h"
#include "runtime/hle/modules/misc_libs.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string_view>

namespace {
void Require(bool condition, const char *message) {
	if (!condition) throw std::runtime_error(message);
}

int Call(u32 nid, u32 a0 = 0, u32 a1 = 0, u32 a2 = 0) {
	const auto *fn = hle::FindFunction("sceNetInet", nid);
	Require(fn && fn->fn, "missing registered network function");
	Ctx c{};
	c.r[4] = a0; c.r[5] = a1; c.r[6] = a2;
	fn->fn(&c);
	return (int)c.r[2];
}

void Check(bool oversized) {
	Require(mem::Init(32u << 20), "guest memory initialization failed");
	hle::Init();
	Register_NetLibraries();
	Require(Call(0x17943399) == 0, "sceNetInetInit failed");
	// PSP AF_INET, SOCK_DGRAM | SOCK_NONBLOCK: a missing datagram must fail, not block.
	const int guest = Call(0x8B7B220F, 2, 2 | 0x20000000, 0);
	Require(guest >= 0, "sceNetInetSocket failed");
	constexpr u32 addr = 0x08804000, len = addr + 32, msg = addr + 64;
	constexpr u32 iov = addr + 128, peer = addr + 160, buf = addr + 256;
	constexpr u32 capacity = 4, canary = 0xDEADC0DE;
	hostnet::PspSockaddrIn local{};
	local.len = sizeof(local); local.family = 2; local.addr = htonl(INADDR_LOOPBACK);
	mem::WriteBytes(addr, &local, sizeof(local));
	Require(Call(0x1A33F9AE, guest, addr, sizeof(local)) == 0, "sceNetInetBind failed");
	GWrite32(len, sizeof(local));
	Require(Call(0x162E6FD5, guest, addr, len) == 0, "sceNetInetGetsockname failed");
	mem::ReadBytes(&local, addr, sizeof(local));
	const auto destination = hostnet::ToHost(local);
	auto sender = hostnet::OpenSocket(AF_INET, SOCK_DGRAM, 0, nullptr);
	Require(sender.Valid(), "host sender failed");
	const char payload[] = "ABCDEFGH";
	const int bytes = oversized ? 8 : capacity;
	Require(hostnet::SendTo(sender.s, payload, bytes, 0, (const sockaddr *)&destination, sizeof(destination)) == bytes,
	        "loopback datagram send failed");
	sockaddr_in source{};
	int sourceLen = sizeof(source);
	Require(hostnet::GetSockName(sender.s, (sockaddr *)&source, &sourceLen) == 0, "sender address unavailable");

	// Peek must report the same truncation without consuming the datagram. Then consume it.
	for (u32 flags : {hostnet::PSP_MSG_PEEK, 0}) {
		for (u32 off = 0; off < 28; off += 4) GWrite32(msg + off, 0);
		GWrite32(iov, buf); GWrite32(iov + 4, capacity);
		GWrite32(msg, peer); GWrite32(msg + 4, sizeof(local));
		GWrite32(msg + 8, iov); GWrite32(msg + 12, 1);
		GWrite32(msg + 24, 0xFFFFFFFF);
		GWrite32(buf, 0); GWrite32(buf + capacity, canary);
		const int received = Call(0xEECE61D2, guest, msg, flags);
		const u32 resultFlags = GRead32(msg + 24);
		const u32 expectedFlags = oversized ? 0x10 : 0;
		std::printf("sent=%d capacity=%u peek=%d received=%d flags=0x%02x expected=0x%02x\n",
		            bytes, capacity, flags != 0, received, resultFlags, expectedFlags);
		Require(received == capacity, "recvmsg must return the copied length, not the original datagram length");
		Require(std::memcmp(GPtr(buf), payload, capacity) == 0, "incorrect received payload prefix");
		Require(GRead32(buf + capacity) == canary, "receive overran the guest buffer");
		Require(resultFlags == expectedFlags, "incorrect guest PSP_MSG_TRUNC flag");
		hostnet::PspSockaddrIn from{};
		mem::ReadBytes(&from, peer, sizeof(from));
		Require(GRead32(msg + 4) == sizeof(from) && from.family == 2 &&
		        from.addr == htonl(INADDR_LOOPBACK) && from.port == source.sin_port,
		        "incorrect source address after receive");
	}
	Require(Call(0xEECE61D2, guest, msg, 0) == -1 && Call(0xFBABE411) == hostnet::PSP_EAGAIN,
	        "consume must discard the entire datagram, including its truncated tail");
	hostnet::CloseSocket(sender);
	Require(Call(0x8D7284EA, guest) == 0, "sceNetInetClose failed");
	hostnet::StopPoller();
	Require(mem::BadAccessCount() == 0, "invalid guest memory access");
	mem::Shutdown();
}
}  // namespace

int main(int argc, char **argv) {
	if (argc != 2 || (std::string_view(argv[1]) != "exact_fit" && std::string_view(argv[1]) != "oversized")) {
		std::fprintf(stderr, "usage: pspc_net_recvmsg_tests <exact_fit|oversized>\n");
		return 2;
	}
	try {
		Check(std::string_view(argv[1]) == "oversized");
		return 0;
	} catch (const std::exception &error) {
		std::fprintf(stderr, "recvmsg test: %s\n", error.what());
		return 1;
	}
}
