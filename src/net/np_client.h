// SPDX-License-Identifier: GPL-2.0-or-later
//
// Client for a PSPC Network server (server/pspc_network): the stand-in for the PlayStation
// Network. Accounts, sign-in sessions (a token kept in pspc.ini), NP tickets the server signs,
// friend lists. JSON over HTTP(S) through WinHTTP; calls block (the runtime makes them on a worker
// thread, the launcher on its UI thread with a short timeout).
#pragma once

#include <string>
#include <vector>

#include "common/common.h"

namespace np {

struct Profile {
	std::string onlineId;
	u64 accountId = 0;
	std::string country = "us";
	std::string domain = "c9";
};

struct Friend {
	std::string onlineId;
	std::string status;  // "friend", "sent" (waiting for them), "received" (waiting for us)
};

struct Result {
	bool ok = false;
	int httpStatus = 0;    // 0: the server couldn't be reached
	std::string code;      // the server's error code ("bad_credentials", "online_id_taken"...)
	std::string message;   // for people
	explicit operator bool() const { return ok; }
};

// "np.example.com" -> "https://np.example.com"; keeps an explicit http:// or https://; drops a
// trailing '/'. Empty stays empty.
std::string NormalizeServerUrl(const std::string &server);

class NetworkClient {
public:
	NetworkClient(std::string server, std::string token = std::string());

	Result Info(std::string *name, bool *registration);
	Result Register(const std::string &onlineId, const std::string &password, const std::string &country, Profile *profile,
		std::string *token);
	Result SignIn(const std::string &onlineId, const std::string &password, Profile *profile, std::string *token);
	Result SignOut();
	Result Me(Profile *profile);
	Result Ticket(const std::string &serviceId, std::vector<u8> *ticket);
	Result Friends(std::vector<Friend> *friends);
	Result AddFriend(const std::string &onlineId, Friend *result);
	Result RemoveFriend(const std::string &onlineId);

	const std::string &Server() const { return server_; }
	void SetToken(std::string token) { token_ = std::move(token); }
	u32 timeoutMs = 10000;

private:
	Result Call(const char *method, const std::string &path, const std::string &body, std::string *responseBody);

	std::string server_, token_;
};

}  // namespace np
