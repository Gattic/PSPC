// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/np_ticket.h"

#include <algorithm>
#include <cstring>

namespace np {

namespace {

constexpr u32 kVersion21 = 0x21010000;
constexpr u16 T_NULL = 0, T_INT = 1, T_LONG = 2, T_STRING = 4, T_DATE = 7, T_BINARY = 8;
constexpr u16 SECTION_BODY = 0x3000, SECTION_FOOTER = 0x3002;

void Put16(std::vector<u8> &out, u16 v) {
	out.push_back((u8)(v >> 8));
	out.push_back((u8)v);
}
void Put32(std::vector<u8> &out, u32 v) {
	Put16(out, (u16)(v >> 16));
	Put16(out, (u16)v);
}
void Put64(std::vector<u8> &out, u64 v) {
	Put32(out, (u32)(v >> 32));
	Put32(out, (u32)v);
}
void Param(std::vector<u8> &out, u16 type, const void *data, u16 len) {
	Put16(out, type);
	Put16(out, len);
	out.insert(out.end(), (const u8 *)data, (const u8 *)data + len);
}
void FixedParam(std::vector<u8> &out, u16 type, const std::string &s, u16 len) {
	std::vector<u8> buf(len, 0);
	memcpy(buf.data(), s.data(), std::min<size_t>(s.size(), len));
	Param(out, type, buf.data(), len);
}
template <typename T>
void NumParam(std::vector<u8> &out, u16 type, T v) {
	std::vector<u8> b;
	if (sizeof(T) == 8) Put64(b, (u64)v);
	else Put32(b, (u32)v);
	Param(out, type, b.data(), (u16)b.size());
}

u16 Get16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
u32 Get32(const u8 *p) { return ((u32)Get16(p) << 16) | Get16(p + 2); }
u64 Get64(const u8 *p) { return ((u64)Get32(p) << 32) | Get32(p + 4); }

bool SplitParams(const u8 *p, size_t size, std::vector<TicketParam> &out) {
	size_t pos = 0;
	while (pos + 4 <= size) {
		TicketParam tp;
		tp.type = Get16(p + pos);
		const u16 len = Get16(p + pos + 2);
		pos += 4;
		if (pos + len > size) return false;
		tp.data.assign(p + pos, p + pos + len);
		pos += len;
		out.push_back(std::move(tp));
	}
	return pos == size;
}

std::string Text(const std::vector<u8> &d) { return std::string((const char *)d.data(), strnlen((const char *)d.data(), d.size())); }

}  // namespace

std::vector<u8> BuildLocalTicket(const TicketFields &f) {
	std::vector<u8> body;
	Param(body, T_BINARY, f.serial, 20);
	NumParam(body, T_INT, f.issuerId);
	NumParam(body, T_DATE, f.issuedMs);
	NumParam(body, T_DATE, f.expiresMs);
	NumParam(body, T_LONG, f.accountId);
	FixedParam(body, T_STRING, f.onlineId, 32);
	FixedParam(body, T_BINARY, f.country, 4);
	FixedParam(body, T_STRING, f.domain, 4);
	FixedParam(body, T_BINARY, f.serviceId, 24);
	NumParam(body, T_INT, f.status);
	Param(body, T_NULL, nullptr, 0);
	Param(body, T_NULL, nullptr, 0);

	std::vector<u8> footer;
	FixedParam(footer, T_BINARY, "PSPL", 4);
	const u8 noSignature[32] = {};
	Param(footer, T_BINARY, noSignature, 32);

	std::vector<u8> t;
	Put32(t, kVersion21);
	Put32(t, (u32)(4 + body.size() + 4 + footer.size()));
	Put16(t, SECTION_BODY);
	Put16(t, (u16)body.size());
	t.insert(t.end(), body.begin(), body.end());
	Put16(t, SECTION_FOOTER);
	Put16(t, (u16)footer.size());
	t.insert(t.end(), footer.begin(), footer.end());
	return t;
}

bool ParseTicket(const std::vector<u8> &t, TicketFields *f, std::vector<TicketParam> *bodyParams) {
	if (t.size() < 12) return false;
	const u32 version = Get32(t.data());
	if ((version >> 24) != 0x21 && (version >> 24) != 0x31 && (version >> 24) != 0x41) return false;
	if (Get32(t.data() + 4) != t.size() - 8) return false;
	std::vector<TicketParam> body, footer;
	bool haveBody = false;
	for (size_t pos = 8; pos + 4 <= t.size();) {
		const u16 type = Get16(&t[pos]), size = Get16(&t[pos + 2]);
		pos += 4;
		if (pos + size > t.size()) return false;
		if (type == SECTION_BODY) {
			if (!SplitParams(&t[pos], size, body)) return false;
			haveBody = true;
		} else if (type == SECTION_FOOTER) {
			if (!SplitParams(&t[pos], size, footer)) return false;
		}
		pos += size;
	}
	if (!haveBody || body.size() < 10) return false;
	auto num = [&](int i, int bytes) -> u64 {
		const auto &d = body[(size_t)i].data;
		if ((int)d.size() < bytes) return 0;
		return bytes == 8 ? Get64(d.data()) : Get32(d.data());
	};
	if (f) {
		memcpy(f->serial, body[0].data.data(), std::min<size_t>(20, body[0].data.size()));
		f->issuerId = (u32)num(1, 4);
		f->issuedMs = num(2, 8);
		f->expiresMs = num(3, 8);
		f->accountId = num(4, 8);
		f->onlineId = Text(body[5].data);
		f->country = Text(body[6].data);
		f->domain = Text(body[7].data);
		f->serviceId = Text(body[8].data);
		f->status = (u32)num(9, 4);
		f->cipher = footer.empty() ? std::string() : Text(footer[0].data);
	}
	if (bodyParams) *bodyParams = std::move(body);
	return true;
}

}  // namespace np
