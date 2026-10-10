// SPDX-License-Identifier: GPL-2.0-or-later
//
// One HTTP/HTTPS request and its response over WinHTTP, for sceHttp (net_http.cpp). Calls block:
// the runtime makes them on a worker thread (net::RunBlockingOnHost) while the guest thread sleeps.
// TLS, proxies (the system's settings), redirects and chunked bodies are WinHTTP's; certificates
// are checked against the Windows store.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "common/common.h"

namespace hostnet {

struct HttpRequestSpec {
	std::string method = "GET";  // GET, POST, HEAD
	std::string url;             // absolute http:// or https:// URL
	std::string userAgent;
	bool http10 = false;
	std::vector<std::pair<std::string, std::string>> headers;
	std::vector<u8> body;
	bool followRedirects = true;
	bool cookies = false;
	u32 resolveTimeoutMs = 0;  // 0 = WinHTTP's default
	u32 connectTimeoutMs = 0;
	u32 sendTimeoutMs = 0;
	u32 recvTimeoutMs = 0;
};

class HttpExchange {
public:
	HttpExchange();
	~HttpExchange();
	HttpExchange(const HttpExchange &) = delete;
	HttpExchange &operator=(const HttpExchange &) = delete;

	// Sends the request and reads the response's status and headers. Returns 0 or the Win32 /
	// WinHTTP error (ERROR_WINHTTP_NAME_NOT_RESOLVED, ..._CANNOT_CONNECT, ..._TIMEOUT,
	// ..._SECURE_FAILURE, ..._OPERATION_CANCELLED, ...).
	u32 Send(const HttpRequestSpec &spec);
	// Reads up to `size` bytes of the body (decoded from chunked transfer); *got = 0 at its end.
	u32 Read(u8 *buf, u32 size, u32 *got);
	// Makes a Send or Read in progress on another thread fail with ..._OPERATION_CANCELLED, and
	// every later call too. Thread-safe.
	void Cancel();

	// The response (valid after a successful Send).
	int status = 0;
	std::string statusLine;     // "HTTP/1.1 200 OK"
	std::string rawHeaders;     // status line, headers, blank line; CRLF line ends
	bool haveContentLength = false;
	u64 contentLength = 0;
	std::string finalUrl;       // after redirects

	// "ERROR_WINHTTP_CANNOT_CONNECT"-like name, for logs.
	static const char *ErrorName(u32 err);

private:
	void CloseHandles();
	std::mutex mu_;
	void *session_ = nullptr, *connect_ = nullptr, *request_ = nullptr;
	bool cancelled_ = false;
};

}  // namespace hostnet
