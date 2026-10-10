// SPDX-License-Identifier: GPL-2.0-or-later
//
// NP tickets (the proof of PSN sign-in a game hands to its server), in Sony's version 2.1 layout
// as PSP games receive them. The layout, field by field, is documented with the server that signs
// them: server/pspc_network/ticket.py. pspc builds a ticket itself only without a PSPC Network
// server (cipher id "PSPL", an unchecked signature); the server's tickets carry "PSPC".
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace np {

struct TicketFields {
	u8 serial[20] = {};
	u32 issuerId = 0x100;
	u64 issuedMs = 0, expiresMs = 0;
	u64 accountId = 0;
	std::string onlineId;
	std::string country = "us";  // 2 letters
	std::string domain = "c9";
	std::string serviceId;       // up to 24 characters
	u32 status = 24u << 24;      // (age & 0x7f) << 24 | flags
	std::string cipher;          // footer cipher id: "PSPC", "PSPL", or Sony's
};

// A ticket made on this PC (cipher "PSPL").
std::vector<u8> BuildLocalTicket(const TicketFields &f);

// One parameter of the ticket body, as sceNpAuthGetTicketParam returns them.
struct TicketParam {
	u16 type = 0;
	std::vector<u8> data;
};
// Splits a ticket; false if it isn't one.
bool ParseTicket(const std::vector<u8> &ticket, TicketFields *fields, std::vector<TicketParam> *bodyParams = nullptr);

}  // namespace np
