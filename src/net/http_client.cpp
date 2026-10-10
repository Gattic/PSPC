// SPDX-License-Identifier: GPL-2.0-or-later
// See http_client.h.
#include "net/http_client.h"

#include <windows.h>
#include <winhttp.h>

#include <cstdlib>

#pragma comment(lib, "winhttp.lib")

namespace hostnet {

namespace {

std::wstring Widen(const std::string &s) {
	if (s.empty()) return std::wstring();
	int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
	std::wstring w((size_t)n, L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
	return w;
}

std::string Narrow(const std::wstring &w) {
	if (w.empty()) return std::string();
	int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
	std::string s((size_t)n, '\0');
	WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
	return s;
}

std::wstring QueryString(HINTERNET r, DWORD what) {
	DWORD len = 0;
	WinHttpQueryHeaders(r, what, WINHTTP_HEADER_NAME_BY_INDEX, WINHTTP_NO_OUTPUT_BUFFER, &len, WINHTTP_NO_HEADER_INDEX);
	if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || len == 0) return std::wstring();
	std::wstring buf(len / sizeof(wchar_t) + 1, L'\0');
	if (!WinHttpQueryHeaders(r, what, WINHTTP_HEADER_NAME_BY_INDEX, buf.data(), &len, WINHTTP_NO_HEADER_INDEX)) return std::wstring();
	buf.resize(len / sizeof(wchar_t));
	return buf;
}

}  // namespace

HttpExchange::HttpExchange() = default;

HttpExchange::~HttpExchange() {
	std::lock_guard<std::mutex> lock(mu_);
	CloseHandles();
}

void HttpExchange::CloseHandles() {
	if (request_) WinHttpCloseHandle((HINTERNET)request_);
	if (connect_) WinHttpCloseHandle((HINTERNET)connect_);
	if (session_) WinHttpCloseHandle((HINTERNET)session_);
	request_ = connect_ = session_ = nullptr;
}

void HttpExchange::Cancel() {
	std::lock_guard<std::mutex> lock(mu_);
	cancelled_ = true;
	// Closing the handles makes a synchronous call blocked on them return (WinHTTP's way to cancel).
	CloseHandles();
}

u32 HttpExchange::Send(const HttpRequestSpec &spec) {
	HINTERNET session, connect, request;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (cancelled_) return ERROR_WINHTTP_OPERATION_CANCELLED;
		CloseHandles();
		status = 0;
		statusLine.clear();
		rawHeaders.clear();
		haveContentLength = false;
		contentLength = 0;
		finalUrl.clear();

		std::wstring url = Widen(spec.url);
		URL_COMPONENTS uc{};
		uc.dwStructSize = sizeof(uc);
		uc.dwSchemeLength = uc.dwHostNameLength = uc.dwUrlPathLength = uc.dwExtraInfoLength = (DWORD)-1;
		if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) return GetLastError();
		bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
		std::wstring host(uc.lpszHostName, uc.dwHostNameLength);
		std::wstring path(uc.lpszUrlPath, uc.dwUrlPathLength);
		if (uc.lpszExtraInfo) path.append(uc.lpszExtraInfo, uc.dwExtraInfoLength);
		if (path.empty()) path = L"/";

		std::wstring agent = Widen(spec.userAgent);
		session = WinHttpOpen(agent.empty() ? L"" : agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
		                      WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session)
			session = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
		if (!session) return GetLastError();
		session_ = session;
		// WinHTTP's defaults: infinite resolve, 60 s connect, 30 s send and receive. 0 keeps them.
		int resolve = spec.resolveTimeoutMs ? (int)spec.resolveTimeoutMs : 0;
		int conn = spec.connectTimeoutMs ? (int)spec.connectTimeoutMs : 60000;
		int send = spec.sendTimeoutMs ? (int)spec.sendTimeoutMs : 30000;
		int recv = spec.recvTimeoutMs ? (int)spec.recvTimeoutMs : 30000;
		WinHttpSetTimeouts(session, resolve, conn, send, recv);
		connect = WinHttpConnect(session, host.c_str(), uc.nPort, 0);
		if (!connect) return GetLastError();
		connect_ = connect;
		std::wstring method = Widen(spec.method);
		request = WinHttpOpenRequest(connect, method.c_str(), path.c_str(), spec.http10 ? L"HTTP/1.0" : L"HTTP/1.1",
		                             WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
		if (!request) return GetLastError();
		request_ = request;
		DWORD features = 0;
		if (!spec.followRedirects) features |= WINHTTP_DISABLE_REDIRECTS;
		if (!spec.cookies) features |= WINHTTP_DISABLE_COOKIES;
		if (features) WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &features, sizeof(features));
		std::wstring headers;
		for (auto &h : spec.headers) headers += Widen(h.first) + L": " + Widen(h.second) + L"\r\n";
		if (!headers.empty() &&
		    !WinHttpAddRequestHeaders(request, headers.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD | WINHTTP_ADDREQ_FLAG_REPLACE))
			return GetLastError();
	}
	// The network part runs without the lock, so Cancel can close the handles under it.
	void *data = spec.body.empty() ? WINHTTP_NO_REQUEST_DATA : (void *)spec.body.data();
	DWORD size = (DWORD)spec.body.size();
	if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, data, size, size, 0)) return GetLastError();
	if (!WinHttpReceiveResponse(request, nullptr)) return GetLastError();

	std::lock_guard<std::mutex> lock(mu_);
	if (cancelled_) return ERROR_WINHTTP_OPERATION_CANCELLED;
	DWORD code = 0, len = sizeof(code);
	if (WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
	                        &code, &len, WINHTTP_NO_HEADER_INDEX))
		status = (int)code;
	rawHeaders = Narrow(QueryString(request, WINHTTP_QUERY_RAW_HEADERS_CRLF));
	size_t eol = rawHeaders.find("\r\n");
	statusLine = rawHeaders.substr(0, eol);
	std::string cl = Narrow(QueryString(request, WINHTTP_QUERY_CONTENT_LENGTH));
	if (!cl.empty()) {
		haveContentLength = true;
		contentLength = strtoull(cl.c_str(), nullptr, 10);
	}
	DWORD urlLen = 0;
	WinHttpQueryOption(request, WINHTTP_OPTION_URL, nullptr, &urlLen);
	if (urlLen) {
		std::wstring u(urlLen / sizeof(wchar_t) + 1, L'\0');
		if (WinHttpQueryOption(request, WINHTTP_OPTION_URL, u.data(), &urlLen)) {
			u.resize(urlLen / sizeof(wchar_t));
			finalUrl = Narrow(u);
		}
	}
	return 0;
}

u32 HttpExchange::Read(u8 *buf, u32 size, u32 *got) {
	*got = 0;
	HINTERNET request;
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (cancelled_) return ERROR_WINHTTP_OPERATION_CANCELLED;
		request = (HINTERNET)request_;
	}
	if (!request) return ERROR_INVALID_HANDLE;
	DWORD n = 0;
	if (!WinHttpReadData(request, buf, size, &n)) return GetLastError();
	*got = n;
	return 0;
}

const char *HttpExchange::ErrorName(u32 err) {
	switch (err) {
	case 0: return "OK";
	case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "ERROR_WINHTTP_NAME_NOT_RESOLVED";
	case ERROR_WINHTTP_CANNOT_CONNECT: return "ERROR_WINHTTP_CANNOT_CONNECT";
	case ERROR_WINHTTP_CONNECTION_ERROR: return "ERROR_WINHTTP_CONNECTION_ERROR";
	case ERROR_WINHTTP_TIMEOUT: return "ERROR_WINHTTP_TIMEOUT";
	case ERROR_WINHTTP_OPERATION_CANCELLED: return "ERROR_WINHTTP_OPERATION_CANCELLED";
	case ERROR_WINHTTP_SECURE_FAILURE: return "ERROR_WINHTTP_SECURE_FAILURE";
	case ERROR_WINHTTP_INVALID_SERVER_RESPONSE: return "ERROR_WINHTTP_INVALID_SERVER_RESPONSE";
	case ERROR_WINHTTP_INVALID_URL: return "ERROR_WINHTTP_INVALID_URL";
	case ERROR_WINHTTP_UNRECOGNIZED_SCHEME: return "ERROR_WINHTTP_UNRECOGNIZED_SCHEME";
	case ERROR_WINHTTP_REDIRECT_FAILED: return "ERROR_WINHTTP_REDIRECT_FAILED";
	case ERROR_WINHTTP_SECURE_INVALID_CA: return "ERROR_WINHTTP_SECURE_INVALID_CA";
	case ERROR_WINHTTP_SECURE_CERT_CN_INVALID: return "ERROR_WINHTTP_SECURE_CERT_CN_INVALID";
	case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID: return "ERROR_WINHTTP_SECURE_CERT_DATE_INVALID";
	case ERROR_INVALID_HANDLE: return "ERROR_INVALID_HANDLE";
	default: return "ERROR_WINHTTP_?";
	}
}

}  // namespace hostnet
