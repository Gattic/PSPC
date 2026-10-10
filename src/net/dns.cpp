// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/dns.h"

#include <winsock2.h>

#include <algorithm>
#include <cctype>
#include <cstring>

namespace dns {

namespace {

u16 Get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
void Put16(std::vector<u8> &out, u16 v) {
	out.push_back((u8)(v >> 8));
	out.push_back((u8)v);
}
void Put32(std::vector<u8> &out, u32 v) {
	Put16(out, (u16)(v >> 16));
	Put16(out, (u16)v);
}

// A (possibly compressed) name at `pos`; returns the offset after it in the message, 0 on error.
size_t ReadName(const u8 *msg, size_t size, size_t pos, std::string *name) {
	size_t end = 0;
	int jumps = 0;
	std::string out;
	while (pos < size) {
		const u8 len = msg[pos];
		if (len == 0) {
			if (!end) end = pos + 1;
			if (name) *name = out;
			return end;
		}
		if ((len & 0xC0) == 0xC0) {
			if (pos + 1 >= size || ++jumps > 16) return 0;
			if (!end) end = pos + 2;
			pos = (size_t)(((len & 0x3F) << 8) | msg[pos + 1]);
			continue;
		}
		if (len > 63 || pos + 1 + len > size) return 0;
		if (!out.empty()) out.push_back('.');
		for (size_t i = 0; i < len; i++) out.push_back((char)tolower(msg[pos + 1 + i]));
		pos += 1 + len;
	}
	return 0;
}

void PutName(std::vector<u8> &out, const std::string &name) {
	size_t start = 0;
	while (start < name.size()) {
		size_t dot = name.find('.', start);
		if (dot == std::string::npos) dot = name.size();
		const size_t len = std::min<size_t>(dot - start, 63);
		out.push_back((u8)len);
		out.insert(out.end(), name.begin() + (std::ptrdiff_t)start, name.begin() + (std::ptrdiff_t)(start + len));
		start = dot + 1;
	}
	out.push_back(0);
}

}  // namespace

bool ParseQuery(const u8 *msg, size_t size, Question *q) {
	if (size < 12) return false;
	const u16 flags = Get16(msg + 2);
	if (flags & 0x8000) return false;           // a response
	if (((flags >> 11) & 0xF) != 0) return false;  // not a standard query
	if (Get16(msg + 4) < 1) return false;
	std::string name;
	const size_t pos = ReadName(msg, size, 12, &name);
	if (!pos || pos + 4 > size) return false;
	q->id = Get16(msg);
	q->flags = flags;
	q->name = name;
	q->qtype = Get16(msg + pos);
	q->qclass = Get16(msg + pos + 2);
	q->questionEnd = pos + 4;
	return true;
}

std::vector<u8> BuildAnswer(const std::vector<u8> &query, const Question &q, u32 ipv4, u32 ttl) {
	std::vector<u8> out;
	Put16(out, q.id);
	Put16(out, (u16)(0x8000 | 0x0400 | (q.flags & 0x0100) | 0x0080));  // response, authoritative, RD copied, RA
	Put16(out, 1);  // one question
	Put16(out, 1);  // one answer
	Put16(out, 0);
	Put16(out, 0);
	out.insert(out.end(), query.begin() + 12, query.begin() + (std::ptrdiff_t)q.questionEnd);
	Put16(out, 0xC00C);  // the name: a pointer to the question's
	Put16(out, TYPE_A);
	Put16(out, CLASS_IN);
	Put32(out, ttl);
	Put16(out, 4);
	const u8 *a = (const u8 *)&ipv4;  // network byte order already
	out.insert(out.end(), a, a + 4);
	return out;
}

std::vector<u8> BuildQuery(u16 id, const std::string &name) {
	std::vector<u8> out;
	Put16(out, id);
	Put16(out, 0x0100);  // recursion desired
	Put16(out, 1);
	Put16(out, 0);
	Put16(out, 0);
	Put16(out, 0);
	PutName(out, name);
	Put16(out, TYPE_A);
	Put16(out, CLASS_IN);
	return out;
}

bool Lookup(u32 server, const std::string &name, u32 *ipv4, u32 timeoutMs) {
	SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) return false;
	sockaddr_in to{};
	to.sin_family = AF_INET;
	to.sin_port = htons(53);
	to.sin_addr.s_addr = server;
	const u16 id = (u16)(GetTickCount() ^ (uintptr_t)&to);
	const std::vector<u8> query = BuildQuery(id, name);
	bool found = false;
	const DWORD start = GetTickCount();
	for (int attempt = 0; attempt < 2 && !found; attempt++) {
		sendto(s, (const char *)query.data(), (int)query.size(), 0, (const sockaddr *)&to, sizeof(to));
		const DWORD attemptEnd = start + timeoutMs * (DWORD)(attempt + 1) / 2;
		for (;;) {
			const DWORD now = GetTickCount();
			if ((LONG)(attemptEnd - now) <= 0) break;
			fd_set rd;
			FD_ZERO(&rd);
			FD_SET(s, &rd);
			const DWORD left = attemptEnd - now;
			timeval tv{(long)(left / 1000), (long)(left % 1000) * 1000};
			if (select(0, &rd, nullptr, nullptr, &tv) <= 0) break;
			u8 buf[1500];
			sockaddr_in from{};
			int fromLen = sizeof(from);
			const int n = recvfrom(s, (char *)buf, sizeof(buf), 0, (sockaddr *)&from, &fromLen);
			if (n > 0 && from.sin_addr.s_addr == server && ParseAnswer(buf, (size_t)n, id, ipv4)) {
				found = true;
				break;
			}
		}
	}
	closesocket(s);
	return found;
}

bool ParseAnswer(const u8 *msg, size_t size, u16 id, u32 *ipv4) {
	if (size < 12 || Get16(msg) != id || !(Get16(msg + 2) & 0x8000)) return false;
	if ((Get16(msg + 2) & 0xF) != 0) return false;  // an error (NXDOMAIN...)
	const u16 qd = Get16(msg + 4), an = Get16(msg + 6);
	size_t pos = 12;
	for (u16 i = 0; i < qd; i++) {
		pos = ReadName(msg, size, pos, nullptr);
		if (!pos || pos + 4 > size) return false;
		pos += 4;
	}
	for (u16 i = 0; i < an; i++) {
		pos = ReadName(msg, size, pos, nullptr);
		if (!pos || pos + 10 > size) return false;
		const u16 type = Get16(msg + pos), cls = Get16(msg + pos + 2), rdlen = Get16(msg + pos + 8);
		pos += 10;
		if (pos + rdlen > size) return false;
		if (type == TYPE_A && cls == CLASS_IN && rdlen == 4) {
			memcpy(ipv4, msg + pos, 4);
			return true;
		}
		pos += rdlen;  // CNAMEs and others: the A record follows
	}
	return false;
}

}  // namespace dns
