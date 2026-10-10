// SPDX-License-Identifier: GPL-2.0-or-later
//
// Just enough DNS (RFC 1035) for redirecting games to revival servers: parse a query, answer it
// with an IPv4 address, and ask a DNS server for a name's A record.
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace dns {

struct Question {
	u16 id = 0;
	u16 flags = 0;
	std::string name;  // lowercase, no trailing dot
	u16 qtype = 0, qclass = 0;
	size_t questionEnd = 0;  // offset just past the first question in the message
};

constexpr u16 TYPE_A = 1;
constexpr u16 CLASS_IN = 1;

// The first question of a DNS query; false if `msg` isn't a query with one.
bool ParseQuery(const u8 *msg, size_t size, Question *q);
// A response to `query` (its bytes and parsed question) carrying one A record, `ipv4` in network
// byte order.
std::vector<u8> BuildAnswer(const std::vector<u8> &query, const Question &q, u32 ipv4, u32 ttl = 60);
// A query for `name`'s A record.
std::vector<u8> BuildQuery(u16 id, const std::string &name);
// The first A record in a response to query `id` (following CNAMEs); false if none.
bool ParseAnswer(const u8 *msg, size_t size, u16 id, u32 *ipv4);
// Asks DNS server `server` (IPv4, network order) for `name`'s address over UDP, trying twice
// within `timeoutMs`. Blocks.
bool Lookup(u32 server, const std::string &name, u32 *ipv4, u32 timeoutMs);

}  // namespace dns
