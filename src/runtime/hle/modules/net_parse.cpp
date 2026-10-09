// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceParseUri and sceParseHttp: the pure string libraries of the network stack
// (libparse_uri.prx / libparse_http.prx).
//
// Structure and function set ported from PPSSPP (Core/HLE/sceParseUri.cpp, Core/HLE/sceParseHttp.cpp),
// Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later. The URI splitting, escaping and the exact
// sizes / error codes follow what libparse_uri.prx does on hardware (pspautotests net/parseuri).
#include <algorithm>
#include <cctype>
#include <string>

#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_internal.h"

namespace {

// libparse_uri error codes (the 0x804330xx facility).
constexpr u32 SCE_PARSE_URI_ERROR_INVALID_VALUE = 0x80433060;     // malformed URI / escape sequence
constexpr u32 SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE = 0x80433022;  // output area too small
constexpr u32 SCE_PARSE_URI_ERROR_INVALID_ARGUMENT = 0x804331FE;   // nowhere to put the result

// libparse_http error codes.
constexpr u32 SCE_PARSE_HTTP_ERROR_NOT_FOUND = 0x80432025;
constexpr u32 SCE_PARSE_HTTP_ERROR_INVALID_RESPONSE = 0x80432060;
constexpr u32 SCE_PARSE_HTTP_ERROR_INVALID_VALUE = 0x804321FE;

// The structure sceUriParse fills in. Every member except noSlash and port points into the work
// area supplied by the caller.
struct PSPParsedUri {
	s32 noSlash;  // 1 when the scheme isn't followed by "//"
	u32 schemeAddr;
	u32 userInfoUserNameAddr;
	u32 userInfoPasswordAddr;
	u32 hostAddr;
	u32 pathAddr;
	u32 queryAddr;
	u32 fragmentAddr;
	u16 port;
	u8 unknown[10];
};
static_assert(sizeof(PSPParsedUri) == 44, "PSPParsedUri layout");

bool IsAlpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsAlnum(char c) { return IsAlpha(c) || IsDigit(c); }
bool IsSchemeChar(char c) { return IsAlnum(c) || c == '+' || c == '-' || c == '.'; }
// A host name is read as far as these go; whatever stops it starts the path. A first character
// outside the set ("[::1]", "_abc", "%41bc") leaves the host empty and the rest is all path.
bool IsHostChar(char c) { return IsAlnum(c) || c == '-' || c == '.'; }
bool EndsAuthority(char c) { return c == 0 || c == '/' || c == '?' || c == '#'; }

bool EqualsNoCase(const std::string &a, const char *b) {
	size_t n = strlen(b);
	if (a.size() != n) return false;
	for (size_t i = 0; i < n; i++) {
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
	}
	return true;
}

struct UriParts {
	std::string scheme, user, password, host, path, query, fragment;
	s32 noSlash = 1;
	u16 port = 0;

	u32 WorkAreaSize() const {
		return (u32)(scheme.size() + user.size() + password.size() + host.size() + path.size() +
		             query.size() + fragment.size() + 7);
	}
};

u16 DefaultPort(const std::string &scheme) {
	if (EqualsNoCase(scheme, "http")) return 80;
	if (EqualsNoCase(scheme, "https")) return 443;
	return 0;
}

// scheme ":" ["//"] [user [":" password] "@"] host [":" port] path ["?" query] ["#" fragment]
// With or without "//" the part after the scheme is read as an authority (so "mailto:a@b" has a
// user and a host). Without a scheme a leading "/" means there's no authority at all.
bool SplitUri(const char *s, UriParts *out) {
	size_t p = 0;

	// Scheme.
	if (IsAlpha(s[0])) {
		size_t i = 1;
		while (IsSchemeChar(s[i])) i++;
		if (s[i] == ':') {
			out->scheme.assign(s, i);
			p = i + 1;
			if (s[p] == '/' && s[p + 1] == '/') {
				out->noSlash = 0;
				p += 2;
			}
		}
	}
	out->port = DefaultPort(out->scheme);

	// User info: an '@' before the authority ends.
	size_t at = p;
	while (!EndsAuthority(s[at]) && s[at] != '@') at++;
	if (s[at] == '@') {
		std::string userInfo(s + p, at - p);
		size_t colon = userInfo.find(':');
		if (colon != std::string::npos) {
			out->user = userInfo.substr(0, colon);
			out->password = userInfo.substr(colon + 1);
		} else {
			out->user = userInfo;
		}
		p = at + 1;
	}

	// Host.
	size_t h = p;
	while (IsHostChar(s[h])) h++;
	out->host.assign(s + p, h - p);
	p = h;

	// Port: at most five digits, and up to 65536 (which wraps to 0, as on hardware). Nothing may
	// follow the digits inside the authority. An empty port keeps the scheme's default.
	if (s[p] == ':') {
		p++;
		size_t start = p;
		u32 value = 0;
		while (IsDigit(s[p])) {
			if (p - start >= 5) return false;
			value = value * 10 + (u32)(s[p] - '0');
			p++;
		}
		if (!EndsAuthority(s[p])) return false;
		if (p != start) {
			if (value > 65536) return false;
			out->port = (u16)value;
		}
	}

	// Path, query (with its '?'), fragment (with its '#').
	size_t q = p;
	while (s[q] && s[q] != '?' && s[q] != '#') q++;
	out->path.assign(s + p, q - p);
	p = q;
	if (s[p] == '?') {
		while (s[q] && s[q] != '#') q++;
		out->query.assign(s + p, q - p);
		p = q;
	}
	if (s[p] == '#') out->fragment.assign(s + p);
	return true;
}

int sceUriParse(u32 parsedUriAddr, u32 urlAddr, u32 workAreaAddr, u32 workAreaSizeAddr, u32 workAreaLength) {
	const char *url = urlAddr ? mem::GetCString(urlAddr) : nullptr;
	if (!url) {
		LOGD("NET", "sceUriParse: no uri");
		return (int)SCE_PARSE_URI_ERROR_INVALID_VALUE;
	}
	UriParts parts;
	if (!SplitUri(url, &parts)) {
		LOGD("NET", "sceUriParse(%s): malformed", url);
		return (int)SCE_PARSE_URI_ERROR_INVALID_VALUE;
	}
	const u32 required = parts.WorkAreaSize();
	const bool haveSizeOut = mem::IsValidRange(workAreaSizeAddr, 4);

	// Size query: either output area missing.
	if (parsedUriAddr == 0 || workAreaAddr == 0) {
		if (!haveSizeOut) return (int)SCE_PARSE_URI_ERROR_INVALID_ARGUMENT;
		GWrite32(workAreaSizeAddr, required);
		return 0;
	}
	if (required > workAreaLength || !mem::IsValidRange(workAreaAddr, required) ||
	    !mem::IsValidRange(parsedUriAddr, sizeof(PSPParsedUri))) {
		return (int)SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE;
	}

	PSPParsedUri parsed{};
	u32 offset = 0;
	auto put = [&](const std::string &str) {
		u32 addr = workAreaAddr + offset;
		mem::WriteBytes(addr, str.c_str(), (u32)str.size() + 1);
		offset += (u32)str.size() + 1;
		return addr;
	};
	parsed.noSlash = parts.noSlash;
	parsed.schemeAddr = put(parts.scheme);
	parsed.userInfoUserNameAddr = put(parts.user);
	parsed.userInfoPasswordAddr = put(parts.password);
	parsed.hostAddr = put(parts.host);
	parsed.pathAddr = put(parts.path);
	parsed.queryAddr = put(parts.query);
	parsed.fragmentAddr = put(parts.fragment);
	parsed.port = parts.port;
	mem::WriteBytes(parsedUriAddr, &parsed, sizeof(parsed));
	if (haveSizeOut) GWrite32(workAreaSizeAddr, offset);
	LOGD("NET", "sceUriParse(%s): scheme=%s host=%s port=%u path=%s", url, parts.scheme.c_str(),
	     parts.host.c_str(), parts.port, parts.path.c_str());
	return 0;
}

// Builds a URI from the pieces of a parsed URI selected by flags.
int sceUriBuild(u32 workAreaAddr, u32 workAreaSizeAddr, u32 workAreaSize, u32 parsedUriAddr, u32 flags) {
	if (!mem::IsValidRange(parsedUriAddr, sizeof(PSPParsedUri))) return (int)SCE_PARSE_URI_ERROR_INVALID_ARGUMENT;
	PSPParsedUri parsed;
	mem::ReadBytes(&parsed, parsedUriAddr, sizeof(parsed));

	auto component = [&](u32 addr, u32 mask) -> std::string {
		if (addr == 0 || (flags & mask) == 0) return "";
		return mem::ReadString(addr);
	};
	std::string scheme = component(parsed.schemeAddr, 0x1);
	std::string host = component(parsed.hostAddr, 0x2);
	std::string path = component(parsed.pathAddr, 0x8);
	std::string user = component(parsed.userInfoUserNameAddr, 0x10);
	std::string password = component(parsed.userInfoPasswordAddr, 0x20);
	std::string query = component(parsed.queryAddr, 0x40);
	std::string fragment = component(parsed.fragmentAddr, 0x80);

	std::string uri;
	if (!scheme.empty()) uri += scheme + ":";
	if (parsed.noSlash == 0) uri += "//";
	if (!user.empty()) {
		uri += user;
		if (!password.empty()) uri += ":" + password;
		uri += "@";
	}
	uri += host;
	if ((flags & 0x4) && parsed.port != 0) {
		std::string realScheme = parsed.schemeAddr ? mem::ReadString(parsed.schemeAddr) : "";
		if (parsed.port != DefaultPort(realScheme)) uri += ":" + std::to_string(parsed.port);
	}
	uri += path;
	uri += query;
	uri += fragment;

	const u32 required = (u32)uri.size() + 1;
	if (mem::IsValidRange(workAreaSizeAddr, 4)) GWrite32(workAreaSizeAddr, required);
	if (workAreaAddr == 0) return 0;
	if (required > workAreaSize || !mem::IsValidRange(workAreaAddr, required)) return (int)SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE;
	mem::WriteBytes(workAreaAddr, uri.c_str(), required);
	LOGD("NET", "sceUriBuild: %s", uri.c_str());
	return 0;
}

// Percent-encodes everything but letters, digits and "-_.*".
bool UriKeepsUnescaped(u8 c) { return IsAlnum((char)c) || c == '-' || c == '_' || c == '.' || c == '*'; }

int sceUriEscape(u32 escapedAddr, u32 escapedLengthAddr, u32 escapedBufferLength, u32 sourceAddr) {
	const char *source = sourceAddr ? mem::GetCString(sourceAddr) : nullptr;
	if (!source) return (int)SCE_PARSE_URI_ERROR_INVALID_ARGUMENT;
	static const char hex[] = "0123456789ABCDEF";
	u32 required = 1;
	for (const u8 *s = (const u8 *)source; *s; s++) required += UriKeepsUnescaped(*s) ? 1 : 3;

	if (escapedAddr == 0) {
		if (mem::IsValidRange(escapedLengthAddr, 4)) GWrite32(escapedLengthAddr, required);
		return 0;
	}
	// Written as it goes: running out of room leaves what fitted and doesn't report a length.
	u32 o = 0;
	for (const u8 *s = (const u8 *)source; *s; s++) {
		char unit[3];
		u32 n;
		if (UriKeepsUnescaped(*s)) {
			unit[0] = (char)*s;
			n = 1;
		} else {
			unit[0] = '%';
			unit[1] = hex[*s >> 4];
			unit[2] = hex[*s & 0xF];
			n = 3;
		}
		if (o + n > escapedBufferLength || !mem::IsValidRange(escapedAddr + o, n)) return (int)SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE;
		mem::WriteBytes(escapedAddr + o, unit, n);
		o += n;
	}
	if (o + 1 > escapedBufferLength || !mem::IsValidRange(escapedAddr + o, 1)) return (int)SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE;
	GWrite8(escapedAddr + o, 0);
	if (mem::IsValidRange(escapedLengthAddr, 4)) GWrite32(escapedLengthAddr, o + 1);
	return 0;
}

int HexValue(char c) {
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Decodes %XX sequences ('+' is left alone). On success the result is strlen(source) plus two per
// escape sequence, as libparse_uri.prx returns; the length written is the decoded size with its NUL.
int sceUriUnescape(u32 unescapedAddr, u32 unescapedLengthAddr, u32 unescapedBufferLength, u32 sourceAddr) {
	const char *source = sourceAddr ? mem::GetCString(sourceAddr) : nullptr;
	if (!source) return (int)SCE_PARSE_URI_ERROR_INVALID_ARGUMENT;
	std::string decoded;
	u32 escapes = 0;
	for (size_t i = 0; source[i];) {
		if (source[i] == '%') {
			int hi = source[i + 1] ? HexValue(source[i + 1]) : -1;
			int lo = hi >= 0 && source[i + 2] ? HexValue(source[i + 2]) : -1;
			if (hi < 0 || lo < 0) return (int)SCE_PARSE_URI_ERROR_INVALID_VALUE;
			decoded.push_back((char)(hi * 16 + lo));
			escapes++;
			i += 3;
		} else {
			decoded.push_back(source[i++]);
		}
	}
	const int result = (int)(strlen(source) + 2 * escapes);
	const u32 required = (u32)decoded.size() + 1;

	if (unescapedAddr == 0) {
		if (mem::IsValidRange(unescapedLengthAddr, 4)) GWrite32(unescapedLengthAddr, required);
		return result;
	}
	u32 fit = std::min<u32>((u32)decoded.size(), unescapedBufferLength);
	if (fit && mem::IsValidRange(unescapedAddr, fit)) mem::WriteBytes(unescapedAddr, decoded.data(), fit);
	if (required > unescapedBufferLength || !mem::IsValidRange(unescapedAddr, required)) return (int)SCE_PARSE_URI_ERROR_INSUFFICIENT_SIZE;
	GWrite8(unescapedAddr + (u32)decoded.size(), 0);
	if (mem::IsValidRange(unescapedLengthAddr, 4)) GWrite32(unescapedLengthAddr, required);
	return result;
}

// ---------------------------------------------------------------- sceParseHttp

// Looks up a header field (case-insensitively) in a raw response header block and returns a pointer
// to its value inside the block.
int sceParseHttpResponseHeader(u32 headerAddr, u32 headerLength, const char *fieldName, u32 valueAddr, u32 valueLengthAddr) {
	if (!mem::IsValidRange(headerAddr, headerLength) || !mem::IsValidRange(valueAddr, 4) ||
	    !mem::IsValidRange(valueLengthAddr, 4)) {
		return (int)SCE_PARSE_HTTP_ERROR_INVALID_VALUE;
	}
	auto trim = [](std::string s) {
		size_t b = s.find_first_not_of(" \t");
		if (b == std::string::npos) return std::string();
		size_t e = s.find_last_not_of(" \t");
		return s.substr(b, e - b + 1);
	};
	const std::string field = trim(fieldName ? fieldName : "");
	const std::string headers((const char *)GPtr(headerAddr), headerLength);

	size_t lineStart = 0;
	while (lineStart < headers.size()) {
		size_t lineEnd = headers.find_first_of("\r\n", lineStart);
		if (lineEnd == std::string::npos) lineEnd = headers.size();
		const std::string line = headers.substr(lineStart, lineEnd - lineStart);
		size_t colon = line.find(':');
		std::string key = trim(line.substr(0, colon));
		if (!key.empty() && key.size() == field.size() && EqualsNoCase(key, field.c_str())) {
			size_t valueStart = colon == std::string::npos ? line.size() : colon + 1;
			while (valueStart < line.size() && line[valueStart] == ' ') valueStart++;
			const u32 len = (u32)(line.size() - valueStart);
			GWrite32(valueAddr, headerAddr + (u32)(lineStart + valueStart));
			GWrite32(valueLengthAddr, len);
			return (int)len;
		}
		lineStart = lineEnd + 1;
	}
	GWrite32(valueAddr, 0);
	GWrite32(valueLengthAddr, 0);
	return (int)SCE_PARSE_HTTP_ERROR_NOT_FOUND;
}

// Parses "HTTP/<major>.<minor> <code> <comment>" from the first line of a header block.
int sceParseHttpStatusLine(u32 headerAddr, u32 headerLength, u32 majorAddr, u32 minorAddr, u32 statusCodeAddr,
                           u32 commentAddr, u32 commentLengthAddr) {
	if (!mem::IsValidRange(headerAddr, headerLength)) return (int)SCE_PARSE_HTTP_ERROR_INVALID_VALUE;
	const char *h = (const char *)GPtr(headerAddr);
	size_t end = 0;
	while (end < headerLength && h[end] != '\r' && h[end] != '\n') end++;
	const std::string line(h, end);

	size_t start = line.find("HTTP/");
	if (start == std::string::npos) return (int)SCE_PARSE_HTTP_ERROR_INVALID_RESPONSE;
	size_t p = start + 5;
	if (p + 3 > line.size() || !IsDigit(line[p]) || line[p + 1] != '.' || !IsDigit(line[p + 2])) {
		return (int)SCE_PARSE_HTTP_ERROR_INVALID_RESPONSE;
	}
	const u32 major = (u32)(line[p] - '0');
	const u32 minor = (u32)(line[p + 2] - '0');
	p += 3;
	size_t ws = p;
	while (p < line.size() && (line[p] == ' ' || line[p] == '\t')) p++;
	if (p == ws || p >= line.size() || !IsDigit(line[p])) return (int)SCE_PARSE_HTTP_ERROR_INVALID_RESPONSE;
	u32 code = 0;
	while (p < line.size() && IsDigit(line[p])) code = code * 10 + (u32)(line[p++] - '0');

	if (mem::IsValidRange(majorAddr, 4)) GWrite32(majorAddr, major);
	if (mem::IsValidRange(minorAddr, 4)) GWrite32(minorAddr, minor);
	if (mem::IsValidRange(statusCodeAddr, 4)) GWrite32(statusCodeAddr, code);
	if (mem::IsValidRange(commentAddr, 4)) GWrite32(commentAddr, headerAddr + (u32)p);
	if (mem::IsValidRange(commentLengthAddr, 4)) GWrite32(commentLengthAddr, (u32)(line.size() - p));
	return 0;
}

}  // namespace

namespace net {

void RegisterParseLibraries() {
	static const hle::HleFunction parseUri[] = {
		HLEFN(0x49E950EC, sceUriEscape),
		HLEFN(0x062BB07E, sceUriUnescape),
		HLEFN(0x568518C9, sceUriParse),
		HLEFN(0x7EE318AF, sceUriBuild),
	};
	hle::RegisterModule("sceParseUri", parseUri);

	static const hle::HleFunction parseHttp[] = {
		HLEFN(0x8077A433, sceParseHttpStatusLine),
		HLEFN(0xAD7BFDEF, sceParseHttpResponseHeader),
	};
	hle::RegisterModule("sceParseHttp", parseHttp);
}

}  // namespace net
