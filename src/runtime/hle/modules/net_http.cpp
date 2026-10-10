// SPDX-License-Identifier: GPL-2.0-or-later
//
// sceHttp (libhttp.prx) and sceSsl (libssl.prx). Templates, connections and requests are created
// and configured as on hardware; sending a request makes it over WinHTTP (http and https, the
// system's proxy settings, redirects, chunked bodies), on a worker thread while the calling guest
// thread sleeps. The body is read as the game asks for it (sceHttpReadData).
//
// Like libhttp, a request needs the PSP to be on a network: without an access point (sceNetApctl not
// at GOT_IP) a host name fails with the resolver's NO_DNS_SERVER and an address with
// SCE_HTTP_ERROR_NETWORK, and the request stays "before send". network=0 always behaves that way.
//
// Function set, the template/connection/request model and argument checks ported from PPSSPP
// (Core/HLE/sceHttp.cpp, sceHttp.h, sceSsl.cpp), Copyright (c) 2012- PPSSPP Project,
// GPL-2.0-or-later. Object IDs follow libhttp.prx: templates 1, 2, ...; connections 0x01000001...;
// requests 0x02000001... (pspautotests net/http).
#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/log.h"
#include "net/http_client.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/errors.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/sysmem.h"

#include <winhttp.h>  // after windows.h (via net_internal.h)

namespace {

constexpr const char *LOG = "NET";

// libhttp error codes (uOFW lib_http.h / vitasdk).
constexpr u32 SCE_HTTP_ERROR_BEFORE_INIT = 0x80431001;
constexpr u32 SCE_HTTP_ERROR_ALREADY_INITED = 0x80431020;
constexpr u32 SCE_HTTP_ERROR_OUT_OF_MEMORY = 0x80431022;
constexpr u32 SCE_HTTP_ERROR_UNKNOWN_SCHEME = 0x80431061;
constexpr u32 SCE_HTTP_ERROR_NETWORK = 0x80431063;
constexpr u32 SCE_HTTP_ERROR_BAD_RESPONSE = 0x80431064;
constexpr u32 SCE_HTTP_ERROR_BEFORE_SEND = 0x80431065;
constexpr u32 SCE_HTTP_ERROR_TIMEOUT = 0x80431068;
constexpr u32 SCE_HTTP_ERROR_NO_CONTENT_LENGTH = 0x80431071;
constexpr u32 SCE_HTTP_ERROR_SSL = 0x80431075;
constexpr u32 SCE_HTTP_ERROR_ABORTED = 0x80431080;
constexpr u32 SCE_HTTP_ERROR_UNKNOWN = 0x80431081;
constexpr u32 SCE_HTTP_ERROR_UNKNOWN_METHOD = 0x8043106B;
constexpr u32 SCE_HTTP_ERROR_INVALID_ID = 0x80431100;
constexpr u32 SCE_HTTP_ERROR_INVALID_VALUE = 0x804311FE;
constexpr u32 SCE_HTTP_ERROR_INVALID_URL = 0x80433060;

constexpr u32 ID_KIND_MASK = 0xFF000000;
constexpr u32 ID_CONNECTION = 0x01000000;
constexpr u32 ID_REQUEST = 0x02000000;

enum HttpMethod { HTTP_METHOD_GET = 0, HTTP_METHOD_POST = 1, HTTP_METHOD_HEAD = 2 };

// Settings a template hands down to its connections, and a connection to its requests.
struct HttpSettings {
	std::string userAgent;
	int httpVersion = 1;
	int proxyMode = 0;
	bool cookie = false, keepAlive = false, cache = false, auth = false, redirect = true;
	u32 connectTimeout = 30 * 1000 * 1000;
	u32 sendTimeout = 120 * 1000 * 1000;
	u32 recvTimeout = 120 * 1000 * 1000;
	u32 resolveTimeout = 1 * 1000 * 1000;
	int resolveRetry = 5;
	std::map<std::string, std::string> headers;
};

struct HttpConnection {
	HttpSettings settings;
	u32 templateId = 0;
	std::string host, scheme;
	u16 port = 80;
};

// The host side of a request: shared with the worker thread doing its WinHTTP calls, which may
// outlive the request (a deleted or aborted request's worker finishes on its own).
struct HttpTransfer {
	hostnet::HttpExchange exchange;
	u32 error = 0;           // WinHTTP error of the last call
	std::vector<u8> buffer;  // body bytes read ahead of the game
	size_t pos = 0;
	bool eof = false;
};

struct HttpRequest {
	HttpSettings settings;
	u32 connectionId = 0;
	int method = HTTP_METHOD_GET;
	std::string url;
	u64 contentLength = 0;
	bool sent = false;
	bool aborted = false;
	std::shared_ptr<HttpTransfer> transfer;
	u64 abortKey = 0;
	int networkErrno = 0;  // sceHttpGetNetworkErrno
	u32 headerAddr = 0;    // sceHttpGetAllHeader's copy (user memory)
};
u64 g_nextHttpAbortKey = 1;

void ReleaseRequest(HttpRequest &r) {
	if (r.transfer) r.transfer->exchange.Cancel();
	r.transfer.reset();
	if (r.abortKey) net::AbortWaits(r.abortKey);
	if (r.headerAddr) kernel::g_userMem.Free(r.headerAddr);
	r.headerAddr = 0;
}

bool g_httpInited = false;
bool g_httpsInited = false;
bool g_httpCacheInited = false;
std::map<u32, HttpSettings> g_templates;
std::map<u32, HttpConnection> g_connections;
std::map<u32, HttpRequest> g_requests;

template <typename M>
u32 FreeId(const M &m, u32 kind) {
	u32 n = 1;
	while (m.count(kind | n)) n++;
	return kind | n;
}

// Settings of a template, connection or request ID.
HttpSettings *SettingsOf(u32 id) {
	switch (id & ID_KIND_MASK) {
	case 0: {
		auto it = g_templates.find(id);
		return it == g_templates.end() ? nullptr : &it->second;
	}
	case ID_CONNECTION: {
		auto it = g_connections.find(id);
		return it == g_connections.end() ? nullptr : &it->second.settings;
	}
	case ID_REQUEST: {
		auto it = g_requests.find(id);
		return it == g_requests.end() ? nullptr : &it->second.settings;
	}
	default: return nullptr;
	}
}

// Templates and connections only (the options that are fixed once a request exists).
HttpSettings *TemplateOrConnection(u32 id) { return (id & ID_KIND_MASK) == ID_REQUEST ? nullptr : SettingsOf(id); }

HttpRequest *RequestOf(u32 id) {
	auto it = g_requests.find(id);
	return it == g_requests.end() ? nullptr : &it->second;
}

bool EqualsNoCase(const std::string &a, const char *b) {
	if (a.size() != strlen(b)) return false;
	for (size_t i = 0; i < a.size(); i++) {
		if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
	}
	return true;
}

// Minimal absolute-URL split: scheme://[user[:pass]@]host[:port][/...]. Returns false when the
// URL isn't absolute or the port is malformed.
bool SplitHttpUrl(const std::string &url, std::string *scheme, std::string *host, u16 *port) {
	size_t sep = url.find("://");
	if (sep == std::string::npos || sep == 0) return false;
	*scheme = url.substr(0, sep);
	size_t start = sep + 3;
	size_t end = url.find_first_of("/?#", start);
	std::string authority = url.substr(start, end == std::string::npos ? std::string::npos : end - start);
	size_t at = authority.rfind('@');
	if (at != std::string::npos) authority.erase(0, at + 1);
	size_t colon = authority.find(':');
	*port = EqualsNoCase(*scheme, "https") ? 443 : 80;
	if (colon != std::string::npos) {
		std::string p = authority.substr(colon + 1);
		authority.erase(colon);
		if (!p.empty()) {
			if (p.size() > 5 || p.find_first_not_of("0123456789") != std::string::npos) return false;
			u32 v = (u32)atoi(p.c_str());
			if (v > 65535) return false;
			*port = (u16)v;
		}
	}
	*host = authority;
	return !host->empty();
}

bool IsDottedAddress(const std::string &host) {
	int dots = 0;
	for (char c : host) {
		if (c == '.') dots++;
		else if (c < '0' || c > '9') return false;
	}
	return dots == 3;
}

#define HTTP_REQUIRE_INIT() \
	do {                    \
		if (!g_httpInited) return (int)SCE_HTTP_ERROR_BEFORE_INIT; \
	} while (0)

int sceHttpInit(u32 poolSize) {
	if (g_httpInited) return (int)SCE_HTTP_ERROR_ALREADY_INITED;
	g_templates.clear();
	g_connections.clear();
	g_requests.clear();
	g_httpInited = true;
	LOGI(LOG, "sceHttpInit(%u)", poolSize);
	return 0;
}

int sceHttpEnd() {
	HTTP_REQUIRE_INIT();
	for (auto &r : g_requests) ReleaseRequest(r.second);
	g_templates.clear();
	g_connections.clear();
	g_requests.clear();
	g_httpInited = false;
	g_httpCacheInited = false;
	return 0;
}

int sceHttpCreateTemplate(const char *userAgent, int httpVersion, int proxyMode) {
	HTTP_REQUIRE_INIT();
	u32 id = FreeId(g_templates, 0);
	HttpSettings s;
	s.userAgent = userAgent ? userAgent : "";
	s.httpVersion = httpVersion;
	s.proxyMode = proxyMode;
	g_templates[id] = s;
	LOGD(LOG, "sceHttpCreateTemplate(%s, %d, %d) -> %u", s.userAgent.c_str(), httpVersion, proxyMode, id);
	return (int)id;
}

int sceHttpDeleteTemplate(u32 id) {
	HTTP_REQUIRE_INIT();
	if (!g_templates.erase(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	return 0;
}

int CreateConnection(u32 templateId, const std::string &host, const std::string &scheme, u16 port) {
	auto it = g_templates.find(templateId);
	if (it == g_templates.end()) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!EqualsNoCase(scheme, "http") && !EqualsNoCase(scheme, "https")) return (int)SCE_HTTP_ERROR_UNKNOWN_SCHEME;
	u32 id = FreeId(g_connections, ID_CONNECTION);
	HttpConnection c;
	c.settings = it->second;
	c.templateId = templateId;
	c.host = host;
	c.scheme = scheme;
	c.port = port;
	g_connections[id] = c;
	LOGD(LOG, "sceHttpCreateConnection(%s://%s:%u) -> %08x", scheme.c_str(), host.c_str(), port, id);
	return (int)id;
}

int sceHttpCreateConnection(u32 templateId, const char *host, const char *scheme, u32 port, int keepAlive) {
	HTTP_REQUIRE_INIT();
	if (!host || !scheme) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	return CreateConnection(templateId, host, scheme, (u16)port);
}

int sceHttpCreateConnectionWithURL(u32 templateId, const char *url, int keepAlive) {
	HTTP_REQUIRE_INIT();
	if (!url) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	if (!g_templates.count(templateId)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	std::string scheme, host;
	u16 port;
	if (!SplitHttpUrl(url, &scheme, &host, &port)) return (int)SCE_HTTP_ERROR_INVALID_URL;
	return CreateConnection(templateId, host, scheme, port);
}

int sceHttpDeleteConnection(u32 id) {
	HTTP_REQUIRE_INIT();
	if (!g_connections.erase(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	return 0;
}

int CreateRequest(u32 connectionId, int method, const std::string &url, u64 contentLength) {
	auto it = g_connections.find(connectionId);
	if (it == g_connections.end()) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (method < HTTP_METHOD_GET || method > HTTP_METHOD_HEAD) return (int)SCE_HTTP_ERROR_UNKNOWN_METHOD;
	u32 id = FreeId(g_requests, ID_REQUEST);
	HttpRequest r;
	r.settings = it->second.settings;
	r.connectionId = connectionId;
	r.method = method;
	r.url = url;
	r.contentLength = contentLength;
	r.abortKey = (4ull << 40) | g_nextHttpAbortKey++;
	g_requests[id] = r;
	LOGD(LOG, "sceHttpCreateRequest(%08x, %d, %s) -> %08x", connectionId, method, url.c_str(), id);
	return (int)id;
}

int sceHttpCreateRequest(u32 connectionId, int method, const char *path, u64 contentLength) {
	HTTP_REQUIRE_INIT();
	if (!path) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	return CreateRequest(connectionId, method, path, contentLength);
}

int sceHttpCreateRequestWithURL(u32 connectionId, int method, const char *url, u64 contentLength) {
	HTTP_REQUIRE_INIT();
	if (!url) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	std::string u = url;
	if (u.find("://") != std::string::npos) {
		std::string scheme, host;
		u16 port;
		if (!SplitHttpUrl(u, &scheme, &host, &port)) return (int)SCE_HTTP_ERROR_INVALID_URL;
	}
	return CreateRequest(connectionId, method, u, contentLength);
}

int sceHttpDeleteRequest(u32 id) {
	HTTP_REQUIRE_INIT();
	auto it = g_requests.find(id);
	if (it == g_requests.end()) return (int)SCE_HTTP_ERROR_INVALID_ID;
	ReleaseRequest(it->second);
	g_requests.erase(it);
	return 0;
}

// libhttp result of a failed WinHTTP call, and the PSP errno sceHttpGetNetworkErrno reports.
u32 HttpErrorFrom(u32 err, int *networkErrno) {
	*networkErrno = 0;
	switch (err) {
	case ERROR_WINHTTP_NAME_NOT_RESOLVED: return SCE_NET_RESOLVER_ERROR_INVALID_HOST;
	case ERROR_WINHTTP_CANNOT_CONNECT:
		*networkErrno = net::INET_ECONNREFUSED;
		return SCE_HTTP_ERROR_NETWORK;
	case ERROR_WINHTTP_CONNECTION_ERROR:
		*networkErrno = net::INET_ECONNRESET;
		return SCE_HTTP_ERROR_NETWORK;
	case ERROR_WINHTTP_TIMEOUT:
		*networkErrno = net::INET_ETIMEDOUT;
		return SCE_HTTP_ERROR_TIMEOUT;
	case ERROR_WINHTTP_OPERATION_CANCELLED: return SCE_HTTP_ERROR_ABORTED;
	case ERROR_WINHTTP_INVALID_SERVER_RESPONSE:
	case ERROR_WINHTTP_REDIRECT_FAILED:
	case ERROR_WINHTTP_HEADER_SIZE_OVERFLOW: return SCE_HTTP_ERROR_BAD_RESPONSE;
	case ERROR_WINHTTP_INVALID_URL: return SCE_HTTP_ERROR_INVALID_URL;
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return SCE_HTTP_ERROR_UNKNOWN_SCHEME;
	case ERROR_WINHTTP_SECURE_FAILURE:
	case ERROR_WINHTTP_SECURE_INVALID_CA:
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
	case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
	case ERROR_WINHTTP_SECURE_INVALID_CERT:
	case ERROR_WINHTTP_SECURE_CERT_REVOKED:
	case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE:
	case ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED: return SCE_HTTP_ERROR_SSL;
	default: return SCE_HTTP_ERROR_UNKNOWN;
	}
}

// The absolute URL of a request: its own, or the connection's scheme, host and port + its path.
std::string RequestUrl(const HttpRequest &r) {
	if (r.url.find("://") != std::string::npos) return r.url;
	auto it = g_connections.find(r.connectionId);
	if (it == g_connections.end()) return r.url;
	const HttpConnection &c = it->second;
	bool https = EqualsNoCase(c.scheme, "https");
	std::string url = (https ? "https://" : "http://") + c.host;
	if (c.port != (https ? 443 : 80)) url += ":" + std::to_string(c.port);
	if (r.url.empty() || r.url[0] != '/') url += "/";
	return url + r.url;
}

u32 MsFromUs(u32 us) { return us ? std::max<u32>(us / 1000, 1) : 0; }

int sceHttpSendRequest(u32 id, u32 dataAddr, u32 dataSize) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (dataSize > 0 && !mem::IsValidRange(dataAddr, dataSize)) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	std::string url = RequestUrl(*r);
	std::string scheme, host;
	u16 port = 0;
	if (!SplitHttpUrl(url, &scheme, &host, &port)) return (int)SCE_HTTP_ERROR_INVALID_URL;
	// Resolving the host is the first thing sending does, and with no DNS server it fails there.
	if (!net::Enabled() || !net::HaveDnsServer()) {
		LOGI(LOG, "sceHttpSendRequest(%08x): %s - not connected to a network", id, url.c_str());
		if (IsDottedAddress(host)) return (int)SCE_HTTP_ERROR_NETWORK;
		return (int)net::SCE_NET_RESOLVER_NO_DNS_SERVER;
	}
	hostnet::HttpRequestSpec spec;
	spec.method = r->method == HTTP_METHOD_POST ? "POST" : r->method == HTTP_METHOD_HEAD ? "HEAD" : "GET";
	spec.url = url;
	spec.userAgent = r->settings.userAgent;
	spec.http10 = r->settings.httpVersion == 0;
	for (auto &h : r->settings.headers)
		if (!EqualsNoCase(h.first, "user-agent")) spec.headers.push_back(h);
	if (dataSize) spec.body.assign((const u8 *)GPtr(dataAddr), (const u8 *)GPtr(dataAddr) + dataSize);
	spec.followRedirects = r->settings.redirect;
	spec.cookies = r->settings.cookie;
	spec.resolveTimeoutMs = MsFromUs(r->settings.resolveTimeout) * (u32)std::max(1, r->settings.resolveRetry);
	spec.connectTimeoutMs = MsFromUs(r->settings.connectTimeout);
	spec.sendTimeoutMs = MsFromUs(r->settings.sendTimeout);
	spec.recvTimeoutMs = MsFromUs(r->settings.recvTimeout);

	// A request can be sent again; the previous response goes.
	if (r->transfer) r->transfer->exchange.Cancel();
	auto xfer = std::make_shared<HttpTransfer>();
	r->transfer = xfer;
	r->sent = false;
	r->aborted = false;
	r->networkErrno = 0;
	u64 key = r->abortKey;
	LOGI(LOG, "sceHttpSendRequest(%08x): %s %s", id, spec.method.c_str(), url.c_str());
	bool done = net::RunBlockingOnHost(
		[xfer, spec, host]() mutable {
			// The game's server may live on as a revival server (net_infra.cpp).
			u32 revival = 0;
			if (!IsDottedAddress(host) && net::InfraRedirect(host, &revival)) spec.connectAddress = hostnet::Ipv4ToString(revival);
			xfer->error = xfer->exchange.Send(spec);
		},
		0, key);
	r = RequestOf(id);
	if (!r || r->transfer != xfer) return (int)SCE_HTTP_ERROR_ABORTED;  // deleted or re-sent meanwhile
	if (!done || r->aborted) {
		xfer->exchange.Cancel();
		return (int)SCE_HTTP_ERROR_ABORTED;
	}
	if (xfer->error) {
		u32 e = HttpErrorFrom(xfer->error, &r->networkErrno);
		LOGI(LOG, "sceHttpSendRequest(%08x): %s -> %08x", id, hostnet::HttpExchange::ErrorName(xfer->error), e);
		return (int)e;
	}
	r->sent = true;
	LOGI(LOG, "sceHttpSendRequest(%08x): %s", id, xfer->exchange.statusLine.c_str());
	return 0;
}

int sceHttpAbortRequest(u32 id) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	r->aborted = true;
	if (r->transfer) r->transfer->exchange.Cancel();
	net::AbortWaits(r->abortKey);
	return 0;
}

// The response is there once a send succeeded.
int sceHttpGetStatusCode(u32 id, u32 statusCodeAddr) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!mem::IsValidRange(statusCodeAddr, 4)) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	if (!r->sent || !r->transfer) return (int)SCE_HTTP_ERROR_BEFORE_SEND;
	GWrite32(statusCodeAddr, (u32)r->transfer->exchange.status);
	return 0;
}

int sceHttpGetContentLength(u32 id, u32 contentLengthAddr) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!mem::IsValidRange(contentLengthAddr, 8)) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	if (!r->sent || !r->transfer) return (int)SCE_HTTP_ERROR_BEFORE_SEND;
	// A chunked (or connection-close) body has no length up front.
	if (!r->transfer->exchange.haveContentLength) return (int)SCE_HTTP_ERROR_NO_CONTENT_LENGTH;
	u64 len = r->transfer->exchange.contentLength;
	GWrite32(contentLengthAddr, (u32)len);
	GWrite32(contentLengthAddr + 4, (u32)(len >> 32));
	return 0;
}

// The header block as received (status line, headers, blank line), NUL-terminated, in a buffer of
// the library's; *headerSize counts the NUL (libhttp 6.60, as in PPSSPP).
int sceHttpGetAllHeader(u32 id, u32 headerAddrPtr, u32 headerSizePtr) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!mem::IsValidRange(headerAddrPtr, 4) || !mem::IsValidRange(headerSizePtr, 4)) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	if (!r->sent || !r->transfer) return (int)SCE_HTTP_ERROR_BEFORE_SEND;
	const std::string &h = r->transfer->exchange.rawHeaders;
	u32 size = (u32)h.size() + 1;
	if (r->headerAddr) kernel::g_userMem.Free(r->headerAddr);
	r->headerAddr = kernel::g_userMem.Alloc(size, false, "sceHttp response headers");
	if (!r->headerAddr) return (int)SCE_HTTP_ERROR_OUT_OF_MEMORY;
	mem::WriteBytes(r->headerAddr, h.c_str(), size);
	GWrite32(headerAddrPtr, r->headerAddr);
	GWrite32(headerSizePtr, size);
	return 0;
}

// Copies what the game asks for; the body is fetched from the host in pieces of at least 16 KB.
int sceHttpReadData(u32 id, u32 dataAddr, u32 dataSize) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (dataSize && !mem::IsValidRange(dataAddr, dataSize)) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	if (!r->sent || !r->transfer) return (int)SCE_HTTP_ERROR_BEFORE_SEND;
	if (!dataSize || r->method == HTTP_METHOD_HEAD) return 0;
	std::shared_ptr<HttpTransfer> xfer = r->transfer;
	if (xfer->pos >= xfer->buffer.size() && !xfer->eof) {
		u32 want = std::max<u32>(dataSize, 16 * 1024);
		u64 key = r->abortKey;
		bool done = net::RunBlockingOnHost(
		    [xfer, want] {
			    xfer->buffer.resize(want);
			    xfer->pos = 0;
			    u32 got = 0;
			    xfer->error = xfer->exchange.Read(xfer->buffer.data(), want, &got);
			    xfer->buffer.resize(xfer->error ? 0 : got);
			    if (!xfer->error && got == 0) xfer->eof = true;
		    },
		    0, key);
		r = RequestOf(id);
		if (!r || r->transfer != xfer || !done || r->aborted) return (int)SCE_HTTP_ERROR_ABORTED;
		if (xfer->error) {
			u32 e = HttpErrorFrom(xfer->error, &r->networkErrno);
			LOGI(LOG, "sceHttpReadData(%08x): %s -> %08x", id, hostnet::HttpExchange::ErrorName(xfer->error), e);
			return (int)e;
		}
	}
	u32 n = (u32)std::min<size_t>(dataSize, xfer->buffer.size() - xfer->pos);
	if (n) mem::WriteBytes(dataAddr, xfer->buffer.data() + xfer->pos, n);
	xfer->pos += n;
	return (int)n;
}

int sceHttpGetNetworkErrno(u32 id, u32 errnoAddr) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (mem::IsValidRange(errnoAddr, 4)) GWrite32(errnoAddr, (u32)r->networkErrno);
	return 0;
}

int sceHttpAddExtraHeader(u32 id, const char *name, const char *value, int mode) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = SettingsOf(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!name || !value) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	// The PSP browser sets its user agent this way.
	if (mode == 0 && EqualsNoCase(name, "user-agent")) s->userAgent = value;
	s->headers[name] = value;
	return 0;
}

int sceHttpDeleteHeader(u32 id, const char *name) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = SettingsOf(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (!name) return (int)SCE_HTTP_ERROR_INVALID_VALUE;
	s->headers.erase(name);
	return 0;
}

int sceHttpSetConnectTimeOut(u32 id, u32 timeout) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = SettingsOf(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->connectTimeout = timeout;
	return 0;
}
int sceHttpSetSendTimeOut(u32 id, u32 timeout) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = SettingsOf(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->sendTimeout = timeout;
	return 0;
}
int sceHttpSetRecvTimeOut(u32 id, u32 timeout) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = TemplateOrConnection(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->recvTimeout = timeout;
	return 0;
}
int sceHttpSetResolveTimeOut(u32 id, u32 timeout) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = TemplateOrConnection(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->resolveTimeout = timeout;
	return 0;
}
int sceHttpSetResolveRetry(u32 id, int count) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = TemplateOrConnection(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->resolveRetry = count;
	return 0;
}

// The boolean options of templates and connections.
template <bool HttpSettings::*Field, bool Value>
int SetOption(u32 id) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = TemplateOrConnection(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->*Field = Value;
	return 0;
}
int sceHttpEnableCookie(u32 id) { return SetOption<&HttpSettings::cookie, true>(id); }
int sceHttpDisableCookie(u32 id) { return SetOption<&HttpSettings::cookie, false>(id); }
int sceHttpEnableKeepAlive(u32 id) { return SetOption<&HttpSettings::keepAlive, true>(id); }
int sceHttpDisableKeepAlive(u32 id) { return SetOption<&HttpSettings::keepAlive, false>(id); }
int sceHttpEnableCache(u32 id) { return SetOption<&HttpSettings::cache, true>(id); }
int sceHttpDisableCache(u32 id) { return SetOption<&HttpSettings::cache, false>(id); }
int sceHttpEnableAuth(u32 id) { return SetOption<&HttpSettings::auth, true>(id); }
int sceHttpDisableAuth(u32 id) { return SetOption<&HttpSettings::auth, false>(id); }
int sceHttpEnableRedirect(u32 id) { return SetOption<&HttpSettings::redirect, true>(id); }
int sceHttpDisableRedirect(u32 id) { return SetOption<&HttpSettings::redirect, false>(id); }

// Callbacks are stored nowhere: with no response they'd never be called.
int sceHttpSetCallback(u32 id, u32, u32) {
	HTTP_REQUIRE_INIT();
	if (!SettingsOf(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	return 0;
}
int sceHttpSetAuthInfoCB(u32 id, u32 cb) { return sceHttpSetCallback(id, cb, 0); }

int sceHttpInitCache(u32 size) {
	HTTP_REQUIRE_INIT();
	g_httpCacheInited = true;
	return 0;
}
int sceHttpEndCache() {
	HTTP_REQUIRE_INIT();
	g_httpCacheInited = false;
	return 0;
}

int sceHttpSetProxy(u32 id, u32 activate, u32 mode, u32 hostAddr, u32 port) {
	HTTP_REQUIRE_INIT();
	if (!TemplateOrConnection(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	return 0;
}
// No proxy configured.
int sceHttpGetProxy(u32 id, u32 activatePtr, u32 modePtr, u32 hostPtr, u32 len, u32 portPtr) {
	HTTP_REQUIRE_INIT();
	if (!TemplateOrConnection(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (mem::IsValidRange(activatePtr, 4)) GWrite32(activatePtr, 0);
	if (mem::IsValidRange(modePtr, 4)) GWrite32(modePtr, 0);
	if (len && mem::IsValidRange(hostPtr, 1)) GWrite8(hostPtr, 0);
	if (mem::IsValidRange(portPtr, 2)) GWrite16(portPtr, 0);
	return 0;
}

int sceHttpSetMallocFunction(u32, u32, u32) { return 0; }
int sceHttpLoadSystemCookie() { return g_httpInited ? 0 : (int)SCE_HTTP_ERROR_BEFORE_INIT; }
int sceHttpSaveSystemCookie() { return g_httpInited ? 0 : (int)SCE_HTTP_ERROR_BEFORE_INIT; }
int sceHttpAddCookie(const char *, const char *, u32) { return g_httpInited ? 0 : (int)SCE_HTTP_ERROR_BEFORE_INIT; }
int sceHttpNagle(u32 id) {
	HTTP_REQUIRE_INIT();
	if (!SettingsOf(id)) return (int)SCE_HTTP_ERROR_INVALID_ID;
	return 0;
}

// The browser (NetFront) sends through the cache: there is none, so it's a plain send.
int sceHttpSendRequestInCacheFirstMode(u32 id, u32 dataAddr, u32 dataSize) { return sceHttpSendRequest(id, dataAddr, dataSize); }
// Nothing is ever cached.
int sceHttpIsRequestInCache(u32 id, u32, u32) {
	HTTP_REQUIRE_INIT();
	return RequestOf(id) ? 0 : (int)SCE_HTTP_ERROR_INVALID_ID;
}
int sceHttpGetNetworkPspError(u32 id, u32 errAddr) {
	HTTP_REQUIRE_INIT();
	HttpRequest *r = RequestOf(id);
	if (!r) return (int)SCE_HTTP_ERROR_INVALID_ID;
	if (mem::IsValidRange(errAddr, 4)) GWrite32(errAddr, r->networkErrno ? 0x80010000u | (u32)r->networkErrno : 0);
	return 0;
}
int sceHttpChangeHttpVersion(u32 id, int version) {
	HTTP_REQUIRE_INIT();
	HttpSettings *s = SettingsOf(id);
	if (!s) return (int)SCE_HTTP_ERROR_INVALID_ID;
	s->httpVersion = version;
	return 0;
}
int sceHttpCloneTemplate(u32 id) {
	HTTP_REQUIRE_INIT();
	auto it = g_templates.find(id);
	if (it == g_templates.end()) return (int)SCE_HTTP_ERROR_INVALID_ID;
	u32 clone = FreeId(g_templates, 0);
	g_templates[clone] = it->second;
	return (int)clone;
}
// Options with nothing behind them here (caches, auth lists, cookie callbacks, buffer sizes).
int sceHttpAccepted() { return g_httpInited ? 0 : (int)SCE_HTTP_ERROR_BEFORE_INIT; }
// Cookies live in the host's HTTP session; none is handed out to the guest.
int sceHttpGetCookie(u32, u32, u32 lengthAddr, u32, u32) {
	HTTP_REQUIRE_INIT();
	if (mem::IsValidRange(lengthAddr, 4)) GWrite32(lengthAddr, 0);
	return (int)0x80431025;  // SCE_HTTP_ERROR_NOT_FOUND
}

int sceHttpsInit(int, int, int, int) {
	g_httpsInited = true;
	return 0;
}
int sceHttpsInitWithPath(int, int, int) {
	g_httpsInited = true;
	return 0;
}
int sceHttpsEnd() {
	g_httpsInited = false;
	return 0;
}
int sceHttpsLoadDefaultCert(int, int) { return 0; }
int sceHttpsOption(u32) { return 0; }
int sceHttpsGetSslError(u32 id, u32 errPtr, u32 detailPtr) {
	if (mem::IsValidRange(errPtr, 4)) GWrite32(errPtr, 0);
	if (mem::IsValidRange(detailPtr, 4)) GWrite32(detailPtr, 0);
	return 0;
}

// ================================================================ sceSsl

bool g_sslInited = false;
u32 g_sslMaxMem = 0, g_sslCurrentMem = 0;

int sceSslInit(int heapSize) {
	if (g_sslInited) return (int)SCE_SSL_ERROR_ALREADY_INIT;
	if (heapSize <= 0) return (int)SCE_SSL_ERROR_INVALID_PARAMETER;
	g_sslMaxMem = (u32)heapSize;
	g_sslCurrentMem = (u32)heapSize / 2;  // as JPCSP
	g_sslInited = true;
	return 0;
}
int sceSslEnd() {
	if (!g_sslInited) return (int)SCE_SSL_ERROR_NOT_INIT;
	g_sslInited = false;
	return 0;
}
int sceSslGetUsedMemoryMax(u32 ptr) {
	if (!g_sslInited) return (int)SCE_SSL_ERROR_NOT_INIT;
	if (mem::IsValidRange(ptr, 4) && !(ptr & 3)) GWrite32(ptr, g_sslMaxMem);
	return 0;
}
int sceSslGetUsedMemoryCurrent(u32 ptr) {
	if (!g_sslInited) return (int)SCE_SSL_ERROR_NOT_INIT;
	if (mem::IsValidRange(ptr, 4) && !(ptr & 3)) GWrite32(ptr, g_sslCurrentMem);
	return 0;
}

}  // namespace

namespace net {

void RegisterHttpLibraries() {
	static const hle::HleFunction sceHttp[] = {
		HLEFN(0xAB1ABE07, sceHttpInit),
		HLEFN(0xD1C8945E, sceHttpEnd),
		HLEFN(0xA6800C34, sceHttpInitCache),
		HLEFN(0x78B54C09, sceHttpEndCache),
		HLEFN(0x59E6D16F, sceHttpEnableCache),
		HLEFN(0xCCBD167A, sceHttpDisableCache),
		HLEFN(0xD70D4847, sceHttpGetProxy),
		HLEFN(0x4CC7D78F, sceHttpGetStatusCode),
		HLEFN(0xEDEEB999, sceHttpReadData),
		HLEFN(0xBB70706F, sceHttpSendRequest),
		HLEFN(0xA5512E01, sceHttpDeleteRequest),
		HLEFN(0x15540184, sceHttpDeleteHeader),
		HLEFN(0x5152773B, sceHttpDeleteConnection),
		HLEFN(0x8ACD1F73, sceHttpSetConnectTimeOut),
		HLEFN(0x9988172D, sceHttpSetSendTimeOut),
		HLEFN(0xF0F46C62, sceHttpSetProxy),
		HLEFN(0x0DAFA58F, sceHttpEnableCookie),
		HLEFN(0x78A0D3EC, sceHttpEnableKeepAlive),
		HLEFN(0x0B12ABFB, sceHttpDisableCookie),
		HLEFN(0xC7EF2559, sceHttpDisableKeepAlive),
		HLEFN(0xE4D21302, sceHttpsInit),
		HLEFN(0xF9D8EB63, sceHttpsEnd),
		HLEFN(0x47347B50, sceHttpCreateRequest),
		HLEFN(0x8EEFD953, sceHttpCreateConnection),
		HLEFN(0xD081EC8F, sceHttpGetNetworkErrno),
		HLEFN(0x3EABA285, sceHttpAddExtraHeader),
		HLEFN(0xC10B6BD9, sceHttpAbortRequest),
		HLEFN(0xFCF8C055, sceHttpDeleteTemplate),
		HLEFN(0xF49934F6, sceHttpSetMallocFunction),
		HLEFN(0x03D9526F, sceHttpSetResolveRetry),
		HLEFN(0x47940436, sceHttpSetResolveTimeOut),
		HLEFN(0x2A6C3296, sceHttpSetAuthInfoCB),
		HLEFN(0x0809C831, sceHttpEnableRedirect),
		HLEFN(0x9FC5F10D, sceHttpEnableAuth),
		HLEFN(0x1A0EBB69, sceHttpDisableRedirect),
		HLEFN(0xAE948FEE, sceHttpDisableAuth),
		HLEFN(0x76D1363B, sceHttpSaveSystemCookie),
		HLEFN(0x87797BDD, sceHttpsLoadDefaultCert),
		HLEFN(0xF1657B22, sceHttpLoadSystemCookie),
		HLEFN(0x9B1F1F36, sceHttpCreateTemplate),
		HLEFN(0xB509B09E, sceHttpCreateRequestWithURL),
		HLEFN(0xCDF8ECB9, sceHttpCreateConnectionWithURL),
		HLEFN(0x1F0FC3E3, sceHttpSetRecvTimeOut),
		HLEFN(0xDB266CCF, sceHttpGetAllHeader),
		HLEFN(0x0282A3BD, sceHttpGetContentLength),
		HLEFN(0x7774BF4C, sceHttpAddCookie),
		HLEFN(0x68AB0F86, sceHttpsInitWithPath),
		HLEFN(0xB3FAF831, sceHttpsOption),  // sceHttpsDisableOption
		HLEFN(0xBAC31BF1, sceHttpsOption),  // sceHttpsEnableOption
		HLEFN(0x2255551E, sceHttpGetNetworkPspError),
		HLEFN(0x9AFC98B2, sceHttpSendRequestInCacheFirstMode),
		HLEFN(0x54E7DF75, sceHttpIsRequestInCache),
		HLEFN(0xC6330B0D, sceHttpChangeHttpVersion),
		HLEFN(0x4E4A284A, sceHttpCloneTemplate),
		HLEFN(0x739C2D79, sceHttpAccepted),  // sceHttpInitExternalCache
		HLEFN(0x8046E250, sceHttpAccepted),  // sceHttpEnableExternalCache
		HLEFN(0xA461A167, sceHttpAccepted),  // sceHttpEndExternalCache
		HLEFN(0xB0257723, sceHttpAccepted),  // sceHttpFlushExternalCache
		HLEFN(0x1CEDB9D4, sceHttpAccepted),  // sceHttpFlushCache
		HLEFN(0x457D221D, sceHttpAccepted),  // sceHttpFlushCookie
		HLEFN(0x2C3C82CF, sceHttpAccepted),  // sceHttpFlushAuthList
		HLEFN(0x77EE5319, sceHttpAccepted),  // sceHttpLoadAuthList
		HLEFN(0xDD6E7857, sceHttpAccepted),  // sceHttpSaveAuthList
		HLEFN(0x951D310E, sceHttpAccepted),  // sceHttpDisableProxyAuth
		HLEFN(0xCDB0DC58, sceHttpAccepted),  // sceHttpEnableProxyAuth
		HLEFN(0x06488A1C, sceHttpAccepted),  // sceHttpSetCookieSendCallback
		HLEFN(0x3A67F306, sceHttpAccepted),  // sceHttpSetCookieRecvCallback
		HLEFN(0xC98CBBA7, sceHttpAccepted),  // sceHttpSetResHeaderMaxSize
		HLEFN(0x9668864C, sceHttpAccepted),  // sceHttpSetRecvBlockSize
		HLEFN(0xB0C34B1D, sceHttpAccepted),  // sceHttpSetCacheContentLengthMaxSize
		HLEFN(0xA909F2AE, sceHttpAccepted),  // sceHttp_A909F2AE (htmlviewer)
		HLEFN(0x96F16D3E, sceHttpGetCookie),
		HLEFN(0xAB1540D5, sceHttpsGetSslError),
		HLEFN(0xA4496DE5, sceHttpSetCallback),  // sceHttpSetRedirectCallback
		HLEFN(0x267618F4, sceHttpSetCallback),  // sceHttpSetAuthInfoCallback
		HLEFN(0x569A1481, sceHttpSetCallback),  // sceHttpsSetSslCallback
		HLEFN(0xCC920C12, sceHttpNagle),        // sceHttpEnableNagle
		HLEFN(0xD29163DA, sceHttpNagle),        // sceHttpDisableNagle
	};
	hle::RegisterModule("sceHttp", sceHttp);

	static const hle::HleFunction sceSsl[] = {
		HLEFN(0x957ECBE2, sceSslInit),
		HLEFN(0x191CDEFF, sceSslEnd),
		HLESTUB(0x5BFB6B61, "sceSslGetNotAfter"),
		HLESTUB(0x17A10DCC, "sceSslGetNotBefore"),
		HLESTUB(0x3DD5E023, "sceSslGetSubjectName"),
		HLESTUB(0x1B7C8191, "sceSslGetIssuerName"),
		HLESTUB(0xCC0919B0, "sceSslGetSerialNumber"),
		HLESTUB(0x058D21C0, "sceSslGetNameEntryCount"),
		HLESTUB(0xD6D097B4, "sceSslGetNameEntryInfo"),
		HLEFN(0xB99EDE6A, sceSslGetUsedMemoryMax),
		HLEFN(0x0EB43B06, sceSslGetUsedMemoryCurrent),
		HLESTUB(0xF57765D3, "sceSslGetKeyUsage"),
	};
	hle::RegisterModule("sceSsl", sceSsl);
}

}  // namespace net
