// SPDX-License-Identifier: GPL-2.0-or-later
#include "net/np_client.h"

#include <cstdlib>

#include "common/json.h"
#include "net/http_client.h"

namespace np {

namespace {

std::vector<u8> Base64Decode(const std::string &in) {
	auto val = [](char c) -> int {
		if (c >= 'A' && c <= 'Z') return c - 'A';
		if (c >= 'a' && c <= 'z') return c - 'a' + 26;
		if (c >= '0' && c <= '9') return c - '0' + 52;
		if (c == '+' || c == '-') return 62;
		if (c == '/' || c == '_') return 63;
		return -1;
	};
	std::vector<u8> out;
	u32 acc = 0;
	int bits = 0;
	for (char c : in) {
		const int v = val(c);
		if (v < 0) continue;  // '=' padding, whitespace
		acc = (acc << 6) | (u32)v;
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			out.push_back((u8)(acc >> bits));
		}
	}
	return out;
}

std::string UrlEncode(const std::string &s) {
	static const char *hex = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
			out.push_back((char)c);
		} else {
			out.push_back('%');
			out.push_back(hex[c >> 4]);
			out.push_back(hex[c & 15]);
		}
	}
	return out;
}

void ReadProfile(const json::Value &v, Profile *p) {
	if (!p) return;
	p->onlineId = v["online_id"].String();
	p->accountId = strtoull(v["account_id"].String("0").c_str(), nullptr, 10);
	p->country = v["country"].String("us");
	p->domain = v["domain"].String("c9");
}

}  // namespace

std::string NormalizeServerUrl(const std::string &server) {
	std::string s = server;
	while (!s.empty() && (s.back() == '/' || s.back() == ' ')) s.pop_back();
	while (!s.empty() && s.front() == ' ') s.erase(s.begin());
	if (s.empty()) return s;
	if (s.rfind("http://", 0) != 0 && s.rfind("https://", 0) != 0) s = "https://" + s;
	return s;
}

NetworkClient::NetworkClient(std::string server, std::string token)
	: server_(NormalizeServerUrl(server)), token_(std::move(token)) {}

Result NetworkClient::Call(const char *method, const std::string &path, const std::string &body, std::string *responseBody) {
	Result r;
	if (server_.empty()) {
		r.message = "No PSPC Network server is set.";
		return r;
	}
	hostnet::HttpRequestSpec spec;
	spec.method = method;
	spec.url = server_ + path;
	spec.userAgent = "PSPC/1 (PSPC Network client)";
	spec.followRedirects = false;
	spec.connectTimeoutMs = spec.resolveTimeoutMs = 5000;
	spec.sendTimeoutMs = spec.recvTimeoutMs = timeoutMs;
	spec.headers.push_back({"Accept", "application/json"});
	if (!token_.empty()) spec.headers.push_back({"Authorization", "Bearer " + token_});
	if (!body.empty()) {
		spec.headers.push_back({"Content-Type", "application/json"});
		spec.body.assign(body.begin(), body.end());
	}
	hostnet::HttpExchange ex;
	const u32 err = ex.Send(spec);
	if (err) {
		r.message = "Can't reach the PSPC Network server " + server_ + " (" + hostnet::HttpExchange::ErrorName(err) + ").";
		return r;
	}
	std::string text;
	u8 buf[4096];
	for (;;) {
		u32 got = 0;
		if (ex.Read(buf, sizeof(buf), &got) != 0 || got == 0) break;
		text.append((const char *)buf, got);
		if (text.size() > (1u << 20)) break;
	}
	r.httpStatus = ex.status;
	r.ok = ex.status >= 200 && ex.status < 300;
	if (!r.ok) {
		const json::Value v = json::Value::Parse(text);
		r.code = v["error"].String();
		r.message = v["message"].String("The PSPC Network server answered " + std::to_string(ex.status) + ".");
	}
	if (responseBody) *responseBody = std::move(text);
	return r;
}

Result NetworkClient::Info(std::string *name, bool *registration) {
	std::string text;
	Result r = Call("GET", "/v1/info", std::string(), &text);
	if (r) {
		const json::Value v = json::Value::Parse(text);
		if (v["api"].Number() < 1) {
			r.ok = false;
			r.message = server_ + " isn't a PSPC Network server.";
			return r;
		}
		if (name) *name = v["name"].String("PSPC Network");
		if (registration) *registration = v["registration"].Bool(true);
	}
	return r;
}

Result NetworkClient::Register(const std::string &onlineId, const std::string &password, const std::string &country,
                               Profile *profile, std::string *token) {
	std::string text;
	Result r = Call("POST", "/v1/accounts",
	                "{\"online_id\":" + json::Quote(onlineId) + ",\"password\":" + json::Quote(password) + ",\"country\":" +
	                    json::Quote(country) + "}",
	                &text);
	if (r) {
		const json::Value v = json::Value::Parse(text);
		ReadProfile(v, profile);
		token_ = v["token"].String();
		if (token) *token = token_;
	}
	return r;
}

Result NetworkClient::SignIn(const std::string &onlineId, const std::string &password, Profile *profile, std::string *token) {
	std::string text;
	Result r = Call("POST", "/v1/sessions", "{\"online_id\":" + json::Quote(onlineId) + ",\"password\":" + json::Quote(password) + "}",
	                &text);
	if (r) {
		const json::Value v = json::Value::Parse(text);
		ReadProfile(v, profile);
		token_ = v["token"].String();
		if (token) *token = token_;
	}
	return r;
}

Result NetworkClient::SignOut() { return Call("DELETE", "/v1/sessions/current", std::string(), nullptr); }

Result NetworkClient::Me(Profile *profile) {
	std::string text;
	Result r = Call("GET", "/v1/me", std::string(), &text);
	if (r) ReadProfile(json::Value::Parse(text), profile);
	return r;
}

Result NetworkClient::Ticket(const std::string &serviceId, std::vector<u8> *ticket) {
	std::string text;
	Result r = Call("POST", "/v1/tickets", "{\"service_id\":" + json::Quote(serviceId) + "}", &text);
	if (r && ticket) {
		*ticket = Base64Decode(json::Value::Parse(text)["ticket"].String());
		if (ticket->empty()) {
			r.ok = false;
			r.message = "The PSPC Network server sent no ticket.";
		}
	}
	return r;
}

Result NetworkClient::Friends(std::vector<Friend> *friends) {
	std::string text;
	Result r = Call("GET", "/v1/friends", std::string(), &text);
	if (r && friends) {
		friends->clear();
		const json::Value doc = json::Value::Parse(text);  // a named value: the loop refers into it
		for (const json::Value &f : doc["friends"].Array()) friends->push_back({f["online_id"].String(), f["status"].String()});
	}
	return r;
}

Result NetworkClient::AddFriend(const std::string &onlineId, Friend *result) {
	std::string text;
	Result r = Call("PUT", "/v1/friends/" + UrlEncode(onlineId), std::string(), &text);
	if (r && result) {
		const json::Value v = json::Value::Parse(text);
		*result = {v["online_id"].String(), v["status"].String()};
	}
	return r;
}

Result NetworkClient::RemoveFriend(const std::string &onlineId) {
	return Call("DELETE", "/v1/friends/" + UrlEncode(onlineId), std::string(), nullptr);
}

}  // namespace np
