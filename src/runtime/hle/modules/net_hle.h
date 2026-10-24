// SPDX-License-Identifier: GPL-2.0-or-later
//
// What the network libraries (net*.cpp) offer the rest of the runtime: the per-run configuration
// (pspc.ini network= / adhoc_server=) and the connection state and actions the Netconf dialog
// drives (src/runtime/dialogs/simple_dialogs.cpp).
#pragma once

#include <string>

#include "common/common.h"

namespace net {

struct Config {
	bool enabled = true;                  // false: the WLAN switch reads off (PSPC's offline mode)
	std::string adhocServer = "localhost";  // host[:port] of the ad hoc server
	std::string npServer, npOnlineId, npToken;  // PSPC Network (net_np.cpp)
	std::string infraDns;                       // online-service redirects (net_infra.cpp)
	int gamePort = 0;                           // bind the game's own UDP port to this port (0: as asked)
	std::string dataDir;                        // data/: infrastructure.json
};
// Called once per run, before the program boots (runtime.cpp).
void Configure(const Config &config);
bool Enabled();

// ---------------------------------------------------------------- PSPC Network sign-in (net_np.cpp)

// The PSN sign-in the NP sign-in dialog performs: against the PSPC Network server (np_server, with
// the launcher's token), or locally as np_online_id when no server is set. Staying signed in for
// the rest of the run, as the PSP's NP manager does.
enum class NpSigninState { Idle, Working, SignedIn, Failed };
void NpStartSignIn();  // no-op while working or signed in
NpSigninState NpGetSignInState(std::string *message = nullptr);
void NpResetSignIn();  // after a failure: the next NpStartSignIn tries again
std::string NpOnlineId();

// ---------------------------------------------------------------- infrastructure (sceNetApctl)

enum ApctlState : int {
	APCTL_STATE_DISCONNECTED = 0,
	APCTL_STATE_SCANNING = 1,
	APCTL_STATE_JOINING = 2,
	APCTL_STATE_GETTING_IP = 3,
	APCTL_STATE_GOT_IP = 4,
	APCTL_STATE_EAP_AUTH = 5,
	APCTL_STATE_KEY_EXCHANGE = 6,
};
int ApctlGetState();
// sceNetApctlConnect / sceNetApctlDisconnect, also usable without sceNetApctlInit (the dialog).
int ApctlConnect(int confId);
int ApctlDisconnect();
// The connection being made or held: profile name, SSID and (once GOT_IP) the address.
std::string ApctlProfileName();
std::string ApctlSsid();
std::string ApctlIpAddress();

// ---------------------------------------------------------------- ad hoc (sceNetAdhocctl)

enum AdhocctlState : int {
	ADHOCCTL_STATE_DISCONNECTED = 0,
	ADHOCCTL_STATE_CONNECTED = 1,
	ADHOCCTL_STATE_SCANNING = 2,
	ADHOCCTL_STATE_GAMEMODE = 3,
	ADHOCCTL_STATE_DISCOVER = 4,
	ADHOCCTL_STATE_WOL = 5,
};
int AdhocctlGetState();
// sceNetAdhocctlCreate / Connect / Join with a guest group name (8 bytes, not NUL-terminated) or
// scan info (SceNetAdhocctlScanInfo) address. They need sceNetAdhocctlInit, as on hardware.
int AdhocctlCreate(u32 groupNameAddr);
int AdhocctlConnect(u32 groupNameAddr);
int AdhocctlJoin(u32 scanInfoAddr);
int AdhocctlDisconnect();

}  // namespace net
