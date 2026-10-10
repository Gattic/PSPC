// SPDX-License-Identifier: GPL-2.0-or-later
//
// The "pro ad hoc server" protocol (aemu / PPSSPP): how PSP ad hoc play is carried over the
// internet. Every player keeps one TCP connection to an ad hoc server (port 27312). The server knows
// who plays which game in which ad hoc group and tells the members of a group about each other (MAC,
// nickname, IP address); the game data itself then goes directly between the players (UDP for PDP,
// TCP for PTP, PSP port + an offset).
//
// Client -> server: LOGIN (once, first), then CONNECT <group>, DISCONNECT, SCAN, CHAT, PING.
// Server -> client: CONNECT <peer> for every other member of the group joined (and later joiners),
// CONNECT_BSSID <founder's MAC> once the join is complete, DISCONNECT <peer IP>, SCAN <group, host>
// for every group of the game then SCAN_COMPLETE, CHAT.
//
// Packet layouts and opcodes from PPSSPP (Core/HLE/proAdhoc.h, proAdhocServer.h), Copyright (c)
// 2013- PPSSPP Project, GPL-2.0-or-later, originally from Coldbird's aemu.
#pragma once

#include <cstddef>

#include "common/common.h"

namespace adhocproto {

constexpr u16 kServerPort = 27312;

enum Opcode : u8 {
	OPCODE_PING = 0,
	OPCODE_LOGIN = 1,
	OPCODE_CONNECT = 2,
	OPCODE_DISCONNECT = 3,
	OPCODE_SCAN = 4,
	OPCODE_SCAN_COMPLETE = 5,
	OPCODE_CONNECT_BSSID = 6,
	OPCODE_CHAT = 7,
};

constexpr int kMacLen = 6;
constexpr int kGroupNameLen = 8;
constexpr int kNicknameLen = 128;
constexpr int kProductCodeLen = 9;
constexpr int kChatLen = 64;

#pragma pack(push, 1)
struct LoginC2S {
	u8 opcode;
	u8 mac[kMacLen];
	u8 name[kNicknameLen];
	char game[kProductCodeLen];  // product code, e.g. ULUS10000 (A-Z, 0-9)
};
struct ConnectC2S {
	u8 opcode;
	u8 group[kGroupNameLen];
};
struct ChatC2S {
	u8 opcode;
	char message[kChatLen];
};
struct ConnectS2C {
	u8 opcode;
	u8 name[kNicknameLen];
	u8 mac[kMacLen];
	u32 ip;  // network byte order, as the server sees the peer
};
struct DisconnectS2C {
	u8 opcode;
	u32 ip;
};
struct ScanS2C {
	u8 opcode;
	u8 group[kGroupNameLen];
	u8 mac[kMacLen];  // the group's founder
};
struct ConnectBssidS2C {
	u8 opcode;
	u8 mac[kMacLen];
};
struct ChatS2C {
	u8 opcode;
	char message[kChatLen];
	u8 name[kNicknameLen];
};
#pragma pack(pop)

static_assert(sizeof(LoginC2S) == 144, "LoginC2S");
static_assert(sizeof(ConnectC2S) == 9, "ConnectC2S");
static_assert(sizeof(ChatC2S) == 65, "ChatC2S");
static_assert(sizeof(ConnectS2C) == 139, "ConnectS2C");
static_assert(sizeof(DisconnectS2C) == 5, "DisconnectS2C");
static_assert(sizeof(ScanS2C) == 15, "ScanS2C");
static_assert(sizeof(ConnectBssidS2C) == 7, "ConnectBssidS2C");
static_assert(sizeof(ChatS2C) == 193, "ChatS2C");

// Size of a complete server -> client packet starting with `opcode` (0: unknown opcode).
inline size_t S2CPacketSize(u8 opcode) {
	switch (opcode) {
	case OPCODE_PING: return 1;
	case OPCODE_CONNECT: return sizeof(ConnectS2C);
	case OPCODE_DISCONNECT: return sizeof(DisconnectS2C);
	case OPCODE_SCAN: return sizeof(ScanS2C);
	case OPCODE_SCAN_COMPLETE: return 1;
	case OPCODE_CONNECT_BSSID: return sizeof(ConnectBssidS2C);
	case OPCODE_CHAT: return sizeof(ChatS2C);
	default: return 0;
	}
}

// Size of a complete client -> server packet starting with `opcode` (0: unknown opcode).
inline size_t C2SPacketSize(u8 opcode) {
	switch (opcode) {
	case OPCODE_PING: return 1;
	case OPCODE_LOGIN: return sizeof(LoginC2S);
	case OPCODE_CONNECT: return sizeof(ConnectC2S);
	case OPCODE_DISCONNECT: return 1;
	case OPCODE_SCAN: return 1;
	case OPCODE_CHAT: return sizeof(ChatC2S);
	default: return 0;
	}
}

// Group names: up to 8 characters A-Z, a-z, 0-9 (NUL ends a shorter name).
inline bool ValidGroupName(const u8 *g) {
	for (int i = 0; i < kGroupNameLen; i++) {
		u8 c = g[i];
		if (c == 0) break;
		if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))) return false;
	}
	return true;
}

}  // namespace adhocproto
