// SPDX-License-Identifier: GPL-2.0-or-later
//
// One streaming HTTP/HTTPS exchange over WinHTTP (Windows) or libcurl (Linux).
// Send and Read are serialized on host worker threads while the guest sleeps. Cancel may run
// concurrently. TLS uses the host trust store; Linux proxy configuration uses libcurl's environment.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <chrono>
#include <condition_variable>
#include <thread>
#endif

#include "common/common.h"

namespace hostnet {

enum class HttpFailure { Resolve, Connect, Connection, Timeout, Cancelled, BadResponse,
                         InvalidUrl, UnknownScheme, Secure, Other };

struct HttpRequestSpec {
	std::string method = "GET";  // GET, POST, HEAD
	std::string url;             // absolute http:// or https:// URL
	std::string userAgent;
	bool http10 = false;
	std::vector<std::pair<std::string, std::string>> headers;
	std::vector<u8> body;
	bool followRedirects = true;
	bool cookies = false;
	u32 resolveTimeoutMs = 0;  // 0 = backend default
	u32 connectTimeoutMs = 0;
	u32 sendTimeoutMs = 0;
	u32 recvTimeoutMs = 0;
	// A revival server standing in for the URL's host (net_infra.cpp): connect here instead,
	// keep the original Host header, and accept its certificate (it can't be the original's).
	std::string connectAddress;
};

class HttpExchange {
public:
	HttpExchange();
	~HttpExchange();
	HttpExchange(const HttpExchange &) = delete;
	HttpExchange &operator=(const HttpExchange &) = delete;

	// Sends the request and reads the response's status and headers. Returns 0 or an opaque
	// backend error; ClassifyError translates it without leaking host APIs into guest HLE.
	u32 Send(const HttpRequestSpec &spec);
	// Reads up to `size` bytes of the body (decoded from chunked transfer); *got = 0 at its end.
	u32 Read(u8 *buf, u32 size, u32 *got);
	// Makes a Send or Read in progress on another thread fail as Cancelled, and
	// every later call too. Thread-safe.
	void Cancel();

	// The response (valid after a successful Send).
	int status = 0;
	std::string statusLine;     // "HTTP/1.1 200 OK"
	std::string rawHeaders;     // status line, headers, blank line; CRLF line ends
	bool haveContentLength = false;
	u64 contentLength = 0;
	std::string finalUrl;       // after redirects

	static const char *ErrorName(u32 err);
	static HttpFailure ClassifyError(u32 err);

private:
#ifdef _WIN32
	void CloseHandles();
	u32 CompletionError(u32 error);
	void WatchReceiveDeadline();
	u32 BeginReceive();
	u32 EndReceive(u32 error);
	// Handles, cancellation and deadline state share mu_. The deadline thread
	// closes only the current request; it is stopped and joined before destruction.
	std::mutex mu_;
	std::condition_variable deadlineCv_;
	std::thread deadlineThread_;
	std::chrono::steady_clock::time_point receiveDeadline_;
	u64 receiveGeneration_ = 0;
	u64 armedGeneration_ = 0;
	u32 receiveTimeoutMs_ = 30000;
	bool deadlineStarted_ = false, deadlineStop_ = false, receiving_ = false, receiveTimedOut_ = false;
	void *session_ = nullptr, *connect_ = nullptr, *request_ = nullptr;
	bool cancelled_ = false;
#else
	struct State;
	std::unique_ptr<State> state_;
#endif
};

}  // namespace hostnet
