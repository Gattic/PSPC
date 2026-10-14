// SPDX-License-Identifier: GPL-2.0-or-later
// libcurl's multi interface keeps Send/Read synchronous without buffering a whole response.
#include "net/http_client.h"
#include "common/fileutil.h"
#include "common/host.h"

#include <curl/curl.h>
#include <algorithm>
#include <atomic>
#include <cstdio>

namespace hostnet {

struct HttpExchange::State {
	HttpExchange &owner;
	CURL *easy = nullptr;
	CURLM *multi = nullptr;
	curl_slist *headers = nullptr, *connectTo = nullptr;
	HttpRequestSpec spec;
	std::atomic<bool> cancelled{false};
	bool added = false, done = false, ready = false, paused = false, location = false;
	CURLcode error = CURLE_OK;
	std::vector<u8> buffer;
	size_t pos = 0;

	explicit State(HttpExchange &exchange) : owner(exchange) {}
	~State() { Close(); }
	void Close() {
		if (added) curl_multi_remove_handle(multi, easy);
		if (easy) curl_easy_cleanup(easy);
		if (multi) curl_multi_cleanup(multi);
		curl_slist_free_all(headers);
		curl_slist_free_all(connectTo);
		easy = nullptr; multi = nullptr; headers = connectTo = nullptr;
		added = done = ready = paused = location = false;
		error = CURLE_OK; buffer.clear(); pos = 0;
	}
	template <typename T> void Set(CURLoption option, T value) {
		if (error == CURLE_OK) error = curl_easy_setopt(easy, option, value);
	}
	bool Redirecting() const {
		return spec.followRedirects && location && owner.status >= 300 && owner.status < 400;
	}
	static size_t Header(char *data, size_t size, size_t count, void *opaque) {
		auto &s = *static_cast<State *>(opaque);
		size_t n = size * count;
		std::string line(data, n);
		if (line.starts_with("HTTP/")) {
			s.owner.rawHeaders.clear();
			s.owner.statusLine = line.substr(0, line.find_first_of("\r\n"));
			if (sscanf(line.c_str(), "HTTP/%*s %d", &s.owner.status) != 1) return 0;
			s.ready = s.location = false;
		} else if (s.ready) {
			return n;  // Trailers are not part of the response-header snapshot returned by Send.
		}
		if (s.owner.rawHeaders.size() + n > 128 * 1024) return 0;
		s.owner.rawHeaders += line;
		if (ToLower(line).starts_with("location:")) s.location = true;
		if (line == "\r\n" && s.owner.status >= 200) s.ready = !s.Redirecting();
		return n;
	}
	static size_t Body(char *data, size_t size, size_t count, void *opaque) {
		auto &s = *static_cast<State *>(opaque);
		size_t n = size * count;
		if (s.Redirecting()) return n;
		// Accept one curl chunk, then apply backpressure until the guest consumes it.
		if (!s.buffer.empty()) { s.paused = true; return CURL_WRITEFUNC_PAUSE; }
		s.buffer.assign(data, data + n);
		s.pos = 0;
		return n;
	}
	CURLcode Pump(bool sending) {
		int phase = -1;
		u64 started = hostsys::MonotonicUs();
		for (;;) {
			if (cancelled) return CURLE_ABORTED_BY_CALLBACK;
			if ((sending && ready) || (!sending && !buffer.empty())) return CURLE_OK;
			if (done) return error;
			int running = 0;
			if (curl_multi_perform(multi, &running) != CURLM_OK) return CURLE_RECV_ERROR;
			int remaining;
			while (auto *message = curl_multi_info_read(multi, &remaining)) {
				if (message->msg == CURLMSG_DONE) { done = true; error = message->data.result; }
			}
			if ((sending && ready) || (!sending && !buffer.empty()) || done) continue;
			u32 timeout = spec.recvTimeoutMs ? spec.recvTimeoutMs : 30000;
			int current = 3;
			if (sending) {
				curl_off_t dns = 0, connect = 0, uploaded = 0;
				long requestBytes = 0;
				curl_easy_getinfo(easy, CURLINFO_NAMELOOKUP_TIME_T, &dns);
				curl_easy_getinfo(easy, CURLINFO_CONNECT_TIME_T, &connect);
				curl_easy_getinfo(easy, CURLINFO_SIZE_UPLOAD_T, &uploaded);
				curl_easy_getinfo(easy, CURLINFO_REQUEST_SIZE, &requestBytes);
				if (!dns) { current = 0; timeout = spec.resolveTimeoutMs; }
				else if (!connect) { current = 1; timeout = spec.connectTimeoutMs ? spec.connectTimeoutMs : 60000; }
				else if (!requestBytes || uploaded < (curl_off_t)spec.body.size()) {
					current = 2; timeout = spec.sendTimeoutMs ? spec.sendTimeoutMs : 30000;
				}
			}
			u64 now = hostsys::MonotonicUs();
			if (phase != current) { phase = current; started = now; }
			if (timeout && now - started >= (u64)timeout * 1000) {
				done = true; return error = CURLE_OPERATION_TIMEDOUT;
			}
			// Cancel only touches an atomic; no curl handle is closed from a competing thread.
			int wait = timeout ? (int)std::min<u64>(100, ((u64)timeout * 1000 - (now - started) + 999) / 1000) : 100;
			if (curl_multi_poll(multi, nullptr, 0, wait, nullptr) != CURLM_OK) return CURLE_RECV_ERROR;
		}
	}
};

HttpExchange::HttpExchange() : state_(std::make_unique<State>(*this)) {}
HttpExchange::~HttpExchange() = default;
void HttpExchange::Cancel() { state_->cancelled = true; }

u32 HttpExchange::Send(const HttpRequestSpec &spec) {
	static const CURLcode initialized = curl_global_init(CURL_GLOBAL_DEFAULT);
	if (initialized != CURLE_OK) return initialized;
	auto &s = *state_;
	if (s.cancelled) return CURLE_ABORTED_BY_CALLBACK;
	s.Close();
	s.spec = spec;
	status = 0; statusLine.clear(); rawHeaders.clear(); finalUrl.clear();
	haveContentLength = false; contentLength = 0;
	s.easy = curl_easy_init();
	s.multi = curl_multi_init();
	if (!s.easy || !s.multi) return CURLE_OUT_OF_MEMORY;
	s.Set(CURLOPT_URL, spec.url.c_str());
	s.Set(CURLOPT_PROTOCOLS_STR, "http,https");
	s.Set(CURLOPT_REDIR_PROTOCOLS_STR, "http,https");
	s.Set(CURLOPT_NOSIGNAL, 1L);
	s.Set(CURLOPT_SUPPRESS_CONNECT_HEADERS, 1L);
	s.Set(CURLOPT_HTTP_VERSION, spec.http10 ? CURL_HTTP_VERSION_1_0 : CURL_HTTP_VERSION_1_1);
	s.Set(CURLOPT_FOLLOWLOCATION, spec.followRedirects ? 1L : 0L);
	s.Set(CURLOPT_MAXREDIRS, 10L);
	s.Set(CURLOPT_CONNECTTIMEOUT_MS, (long)(spec.connectTimeoutMs ? spec.connectTimeoutMs : 60000));
	s.Set(CURLOPT_USERAGENT, spec.userAgent.c_str());
	s.Set(CURLOPT_HEADERFUNCTION, &State::Header);
	s.Set(CURLOPT_HEADERDATA, &s);
	s.Set(CURLOPT_WRITEFUNCTION, &State::Body);
	s.Set(CURLOPT_WRITEDATA, &s);
	if (spec.cookies) s.Set(CURLOPT_COOKIEFILE, "");
	if (spec.method == "HEAD") s.Set(CURLOPT_NOBODY, 1L);
	else if (spec.method == "POST") {
		s.Set(CURLOPT_POST, 1L);
		s.Set(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)s.spec.body.size());
		s.Set(CURLOPT_POSTFIELDS, s.spec.body.empty() ? "" : (const char *)s.spec.body.data());
	} else if (spec.method == "PUT" || spec.method == "DELETE") {
		if (!s.spec.body.empty()) {
			s.Set(CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)s.spec.body.size());
			s.Set(CURLOPT_POSTFIELDS, (const char *)s.spec.body.data());
		}
		s.Set(CURLOPT_CUSTOMREQUEST, s.spec.method.c_str());
	} else if (spec.method != "GET") return CURLE_BAD_FUNCTION_ARGUMENT;
	for (const auto &[name, value] : spec.headers) {
		auto *list = curl_slist_append(s.headers, (name + ": " + value).c_str());
		if (!list) return CURLE_OUT_OF_MEMORY;
		s.headers = list;
	}
	s.Set(CURLOPT_HTTPHEADER, s.headers);
	if (!spec.connectAddress.empty()) {
		// Revival routing is explicit in the request. Retain the URL's Host/SNI, as on Windows.
		std::unique_ptr<CURLU, decltype(&curl_url_cleanup)> url(curl_url(), curl_url_cleanup);
		if (!url) return CURLE_OUT_OF_MEMORY;
		if (curl_url_set(url.get(), CURLUPART_URL, spec.url.c_str(), 0) != CURLUE_OK) return CURLE_URL_MALFORMAT;
		char *host = nullptr, *port = nullptr;
		CURLUcode h = curl_url_get(url.get(), CURLUPART_HOST, &host, 0);
		CURLUcode p = curl_url_get(url.get(), CURLUPART_PORT, &port, CURLU_DEFAULT_PORT);
		if (h == CURLUE_OK && p == CURLUE_OK)
			s.connectTo = curl_slist_append(nullptr, (std::string(host) + ":" + port + ":" + spec.connectAddress + ":").c_str());
		curl_free(host); curl_free(port);
		if (h != CURLUE_OK || p != CURLUE_OK) return CURLE_URL_MALFORMAT;
		if (!s.connectTo) return CURLE_OUT_OF_MEMORY;
		s.Set(CURLOPT_CONNECT_TO, s.connectTo);
		s.Set(CURLOPT_SSL_VERIFYPEER, 0L);
		s.Set(CURLOPT_SSL_VERIFYHOST, 0L);
	}
	if (s.error != CURLE_OK) return s.error;
	if (curl_multi_add_handle(s.multi, s.easy) != CURLM_OK) return CURLE_FAILED_INIT;
	s.added = true;
	CURLcode result = s.Pump(true);
	if (result != CURLE_OK) return result;
	char *url = nullptr;
	curl_easy_getinfo(s.easy, CURLINFO_EFFECTIVE_URL, &url);
	if (url) finalUrl = url;
	curl_off_t length = -1;
	curl_easy_getinfo(s.easy, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
	haveContentLength = length >= 0;
	if (haveContentLength) contentLength = (u64)length;
	return status ? CURLE_OK : CURLE_GOT_NOTHING;
}

u32 HttpExchange::Read(u8 *buf, u32 size, u32 *got) {
	*got = 0;
	auto &s = *state_;
	if (s.cancelled) return CURLE_ABORTED_BY_CALLBACK;
	if (!s.added || !s.ready) return CURLE_BAD_FUNCTION_ARGUMENT;
	if (!size) return CURLE_OK;
	if (s.buffer.empty() && s.paused) {
		s.paused = false;
		CURLcode result = curl_easy_pause(s.easy, CURLPAUSE_CONT);
		if (result != CURLE_OK) return result;
	}
	CURLcode result = s.Pump(false);
	if (result != CURLE_OK) return result;
	*got = (u32)std::min<size_t>(size, s.buffer.size() - s.pos);
	if (*got) memcpy(buf, s.buffer.data() + s.pos, *got);
	s.pos += *got;
	if (s.pos == s.buffer.size()) { s.buffer.clear(); s.pos = 0; }
	return CURLE_OK;
}

const char *HttpExchange::ErrorName(u32 err) { return curl_easy_strerror((CURLcode)err); }
HttpFailure HttpExchange::ClassifyError(u32 err) {
	switch ((CURLcode)err) {
	case CURLE_COULDNT_RESOLVE_HOST: case CURLE_COULDNT_RESOLVE_PROXY: return HttpFailure::Resolve;
	case CURLE_COULDNT_CONNECT: return HttpFailure::Connect;
	case CURLE_SEND_ERROR: case CURLE_RECV_ERROR: case CURLE_PARTIAL_FILE: return HttpFailure::Connection;
	case CURLE_OPERATION_TIMEDOUT: return HttpFailure::Timeout;
	case CURLE_ABORTED_BY_CALLBACK: return HttpFailure::Cancelled;
	case CURLE_GOT_NOTHING: case CURLE_WEIRD_SERVER_REPLY: case CURLE_TOO_MANY_REDIRECTS:
	case CURLE_WRITE_ERROR: return HttpFailure::BadResponse;
	case CURLE_URL_MALFORMAT: return HttpFailure::InvalidUrl;
	case CURLE_UNSUPPORTED_PROTOCOL: return HttpFailure::UnknownScheme;
	case CURLE_SSL_CONNECT_ERROR: case CURLE_PEER_FAILED_VERIFICATION: case CURLE_SSL_CERTPROBLEM:
	case CURLE_SSL_CACERT_BADFILE: case CURLE_SSL_ISSUER_ERROR: return HttpFailure::Secure;
	default: return HttpFailure::Other;
	}
}
}  // namespace hostnet
