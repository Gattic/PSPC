// SPDX-License-Identifier: GPL-2.0-or-later
//
// PlayStation Network libraries: sceNp, sceNpAuth, sceNpService, answered by PSPC Network
// (docs/pspc-network.md) instead of Sony's servers.
//
// Signing in (the NP sign-in dialog, NpStartSignIn) checks the account's token with the configured
// PSPC Network server; with no server set it signs in locally under pspc.ini's np_online_id. Once
// signed in, the online ID, NP ID and profile are the account's, and sceNpAuth requests return NP
// tickets in Sony's layout: signed by the server, or made here in local mode. A request runs on a
// library thread, as the firmware's does, and the game's ticket callback is called from it.
//
// NIDs, error codes and the content-rating / language defaults from PPSSPP (Core/HLE/sceNp.cpp,
// sceNp.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

#include "common/log.h"
#include "net/np_client.h"
#include "net/np_ticket.h"
#include "runtime/core/cpu.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/kernel/scheduler.h"
#include "runtime/kernel/thread.h"

namespace {

constexpr const char *LOG = "NP";

constexpr u32 SCE_NP_ERROR_ALREADY_INITIALIZED = 0x80550001;
constexpr u32 SCE_NP_ERROR_NOT_INITIALIZED = 0x80550002;
constexpr u32 SCE_NP_ERROR_INVALID_ARGUMENT = 0x80550003;
constexpr u32 SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED = 0x80550301;
constexpr u32 SCE_NP_AUTH_ERROR_NOT_INITIALIZED = 0x80550302;
constexpr u32 SCE_NP_AUTH_ERROR_EINVAL = 0x80550303;
constexpr u32 SCE_NP_AUTH_ERROR_ENOMEM = 0x80550304;
constexpr u32 SCE_NP_AUTH_ERROR_ESRCH = 0x80550305;
constexpr u32 SCE_NP_AUTH_ERROR_EBUSY = 0x80550306;
constexpr u32 SCE_NP_AUTH_ERROR_ABORTED = 0x80550307;
constexpr u32 SCE_NP_AUTH_ERROR_SERVICE_DOWN = 0x80550401;
constexpr u32 SCE_NP_AUTH_ERROR_UNKNOWN = 0x80550480;
constexpr u32 SCE_NP_MANAGER_ERROR_INVALID_TICKET_SIZE = 0x80550508;
constexpr u32 SCE_NP_MANAGER_ERROR_NOT_SIGNIN = 0x8055050B;
constexpr u32 SCE_NP_SERVICE_ERROR_ALREADY_INITIALIZED = 0x80550701;
constexpr u32 SCE_NP_SERVICE_ERROR_NOT_INITIALIZED = 0x80550702;
constexpr u32 SCE_NP_SERVICE_ERROR_EINVAL = 0x80550703;
constexpr u32 SCE_NP_SERVICE_ERROR_ESRCH = 0x80550705;

constexpr const char *kDefaultOnlineId = "PSPCPlayer";
constexpr u64 kTicketLifetimeMs = 15 * 60 * 1000;
constexpr u64 kServerTimeoutUs = 20 * 1000000;
constexpr u64 kMinRoundTripUs = 30000;  // a ticket is never there sooner than a server could answer

// ---------------------------------------------------------------- sign-in (shared with the dialog)

std::mutex g_mu;
net::NpSigninState g_signIn = net::NpSigninState::Idle;
std::string g_signInMessage;
np::Profile g_profile;
bool g_localAccount = true;

bool SignedIn() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_signIn == net::NpSigninState::SignedIn;
}
np::Profile CurrentProfile() {
	std::lock_guard<std::mutex> lock(g_mu);
	return g_profile;
}

// An online ID for local mode: PSN's rules (3-16 letters, digits, - or _, a letter first).
std::string LocalOnlineId(const std::string &configured) {
	std::string id;
	for (char c : configured)
		if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_') id.push_back(c);
	if (id.size() > 16) id.resize(16);
	if (id.size() < 3 || !((id[0] >= 'A' && id[0] <= 'Z') || (id[0] >= 'a' && id[0] <= 'z'))) id = kDefaultOnlineId;
	return id;
}

// A stable account ID for a local online ID (FNV-1a of the lowercase name).
u64 LocalAccountId(const std::string &onlineId) {
	u64 h = 14695981039346656037ull;
	for (char c : onlineId) h = (h ^ (u8)((c >= 'A' && c <= 'Z') ? c + 32 : c)) * 1099511628211ull;
	return h | 1;
}

bool HostedMode() { return !np::NormalizeServerUrl(net::CurrentConfig().npServer).empty(); }

u64 NowMs() {
	return (u64)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------- sceNp

bool g_npInited = false, g_npAuthInited = false, g_npServiceInited = false;
struct NpMemStat {
	s32 memSize, maxMemSize, freeMemSize;
};
NpMemStat g_npAuthMem{}, g_npServiceMem{};
int g_authThreadPrio = 0x30, g_authStackSize = 0x4000;

int sceNpInit() {
	if (g_npInited) return (int)SCE_NP_ERROR_ALREADY_INITIALIZED;
	g_npInited = true;
	return 0;
}
int sceNpTerm() {
	g_npInited = false;
	return 0;
}

// SceNpOnlineId: char data[16], char term, char dummy[3].
void WriteOnlineId(u32 addr, const std::string &id) {
	u8 buf[20] = {};
	memcpy(buf, id.data(), std::min<size_t>(id.size(), 16));
	mem::WriteBytes(addr, buf, sizeof(buf));
}

int sceNpGetOnlineId(u32 idPtr) {
	if (!mem::IsValidRange(idPtr, 20)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	WriteOnlineId(idPtr, CurrentProfile().onlineId);
	return 0;
}
// SceNpId: SceNpOnlineId handle, u8 opt[8], u8 reserved[8].
int sceNpGetNpId(u32 idPtr) {
	if (!mem::IsValidRange(idPtr, 36)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	mem::Memset(idPtr, 0, 36);
	WriteOnlineId(idPtr, CurrentProfile().onlineId);
	return 0;
}
// SceNpUserInformation: SceNpId userId, SceNpAvatarUrl icon (char[127] + term), u8 reserved[52].
int sceNpGetUserProfile(u32 profilePtr) {
	if (!mem::IsValidRange(profilePtr, 36 + 128 + 52)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	mem::Memset(profilePtr, 0, 36 + 128 + 52);
	WriteOnlineId(profilePtr, CurrentProfile().onlineId);
	return 0;
}
// Two SceNpCountryCode (char data[2], term, padding): the account's country and region.
int sceNpGetAccountRegion(u32 countryPtr, u32 regionPtr) {
	if (!mem::IsValidRange(countryPtr, 4) || !mem::IsValidRange(regionPtr, 4)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	const np::Profile p = CurrentProfile();
	u8 c[4] = {}, r[4] = {};
	memcpy(c, p.country.data(), std::min<size_t>(p.country.size(), 2));
	memcpy(r, p.domain.data(), std::min<size_t>(p.domain.size(), 2));
	mem::WriteBytes(countryPtr, c, 4);
	mem::WriteBytes(regionPtr, r, 4);
	return 0;
}
// Parental control on, adult user (PPSSPP's defaults).
int sceNpGetContentRatingFlag(u32 parentalControlPtr, u32 userAgePtr) {
	if (!mem::IsValidRange(parentalControlPtr, 4) || !mem::IsValidRange(userAgePtr, 4)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	GWrite32(parentalControlPtr, 1);
	GWrite32(userAgePtr, 24);
	return 0;
}
int sceNpGetChatRestrictionFlag(u32 flagPtr) {
	if (!mem::IsValidRange(flagPtr, 4)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	GWrite32(flagPtr, 0);
	return 0;
}
int sceNpGetMyLanguages(u32 listPtr) {
	if (!mem::IsValidRange(listPtr, 12)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	GWrite32(listPtr, 1033);      // en-US
	GWrite32(listPtr + 4, 2057);  // en-GB
	GWrite32(listPtr + 8, 1036);  // fr-FR
	return 0;
}

// ---------------------------------------------------------------- sceNpAuth

struct AuthRequest {
	int id = 0;
	u32 callback = 0, callbackArg = 0;
	std::string serviceId;
	bool done = false, aborted = false;
	int result = 0;  // the ticket's size, or an error
	std::vector<u8> ticket;
};
std::map<int, std::shared_ptr<AuthRequest>> g_authRequests;
int g_nextAuthRequest = 1;

int sceNpAuthInit(u32 poolSize, u32 stackSize, u32 threadPrio) {
	if (g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED;
	g_npAuthMem.memSize = (s32)poolSize - 0x20;
	g_npAuthMem.maxMemSize = 0x4050;
	g_npAuthMem.freeMemSize = g_npAuthMem.memSize;
	g_authThreadPrio = threadPrio >= 8 && threadPrio < 120 ? (int)threadPrio : 0x30;
	g_authStackSize = stackSize >= 0x1000 ? (int)stackSize : 0x4000;
	g_npAuthInited = true;
	return 0;
}
int sceNpAuthTerm() {
	for (auto &r : g_authRequests) r.second->aborted = true;
	g_authRequests.clear();
	g_npAuthInited = false;
	return 0;
}
int sceNpAuthGetMemoryStat(u32 statPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statPtr, sizeof(NpMemStat))) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	mem::WriteBytes(statPtr, &g_npAuthMem, sizeof(g_npAuthMem));
	return 0;
}

// SceNpAuthRequestParameter: size, version (u16 major, minor), serviceId, cookie, cookieSize,
// entitlementId, consumedCount, ticketCb(requestId, result, arg), cbArg (from size 0x24).
int sceNpAuthCreateStartRequest(u32 paramPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(paramPtr, 4)) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	const u32 size = GRead32(paramPtr);
	if (size < 0x20 || !mem::IsValidRange(paramPtr, std::min<u32>(size, 0x24))) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	if (g_authRequests.size() >= 16) return (int)SCE_NP_AUTH_ERROR_ENOMEM;

	auto req = std::make_shared<AuthRequest>();
	req->id = g_nextAuthRequest++;
	if (const char *sid = mem::GetCString(GRead32(paramPtr + 8), 64)) req->serviceId = sid;
	if (req->serviceId.size() > 24) req->serviceId.resize(24);
	req->callback = GRead32(paramPtr + 0x1C);
	req->callbackArg = size >= 0x24 ? GRead32(paramPtr + 0x20) : 0;
	g_authRequests[req->id] = req;
	LOGI(LOG, "Ticket request %d for service \"%s\"", req->id, req->serviceId.c_str());

	const np::Profile profile = CurrentProfile();
	const bool hosted = HostedMode();
	const net::Config cfg = net::CurrentConfig();
	const SceUID tid = net::StartLibraryThread("SceNpAuthReq", g_authThreadPrio, g_authStackSize, [req, profile, hosted, cfg]() {
		// The answer comes after a round trip to the server, never before sceNpAuthCreateStartRequest
		// has returned the request's ID (games match the callback's ID against it).
		if (kernel::PspThread *self = sched::Current())
			sched::WaitUntil(kernel::WAITTYPE_DELAY, self->uid, sched::NowUs() + kMinRoundTripUs, false);
		std::vector<u8> ticket;
		int result = 0;
		if (hosted) {
			// The host work may outlive this wait (on a time-out): it writes into shared storage.
			struct Out {
				np::Result r;
				std::vector<u8> ticket;
			};
			auto out = std::make_shared<Out>();
			const std::string serviceId = req->serviceId;
			const bool finished = net::RunBlockingOnHost(
				[out, cfg, serviceId]() {
					np::NetworkClient client(cfg.npServer, cfg.npToken);
					out->r = client.Ticket(serviceId, &out->ticket);
				},
				sched::NowUs() + kServerTimeoutUs);
			const np::Result &r = out->r;
			if (finished) ticket = out->ticket;
			if (!finished) {
				result = (int)SCE_NP_AUTH_ERROR_SERVICE_DOWN;
				LOGW(LOG, "Ticket request %d: the PSPC Network server didn't answer", req->id);
			} else if (!r) {
				result = r.httpStatus == 0 ? (int)SCE_NP_AUTH_ERROR_SERVICE_DOWN : (int)SCE_NP_AUTH_ERROR_UNKNOWN;
				LOGW(LOG, "Ticket request %d failed: %s", req->id, r.message.c_str());
			}
		} else {
			np::TicketFields f;
			std::random_device rd;
			for (u8 &b : f.serial) b = (u8)rd();
			f.issuedMs = NowMs();
			f.expiresMs = f.issuedMs + kTicketLifetimeMs;
			f.accountId = profile.accountId;
			f.onlineId = profile.onlineId;
			f.country = profile.country;
			f.domain = profile.domain;
			f.serviceId = req->serviceId;
			ticket = np::BuildLocalTicket(f);
		}
		if (result == 0) {
			result = (int)ticket.size();
			LOGI(LOG, "Ticket request %d: %d-byte ticket for %s", req->id, result, profile.onlineId.c_str());
		}
		req->ticket = std::move(ticket);
		req->result = result;
		req->done = true;
		if (!req->aborted && req->callback)
			cpu::CallGuest(&sched::Current()->ctx, req->callback, {(u32)req->id, (u32)result, req->callbackArg});
	});
	if (tid < 0) {
		g_authRequests.erase(req->id);
		return (int)SCE_NP_AUTH_ERROR_ENOMEM;
	}
	return req->id;
}

std::shared_ptr<AuthRequest> FindRequest(int id) {
	auto it = g_authRequests.find(id);
	return it == g_authRequests.end() ? nullptr : it->second;
}

int sceNpAuthGetTicket(int id, u32 bufPtr, u32 length) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	auto req = FindRequest(id);
	if (!req) return (int)SCE_NP_AUTH_ERROR_ESRCH;
	if (req->aborted) return (int)SCE_NP_AUTH_ERROR_ABORTED;
	if (!req->done) return (int)SCE_NP_AUTH_ERROR_EBUSY;
	if (req->result < 0) return req->result;
	if (!mem::IsValidRange(bufPtr, (u32)req->ticket.size())) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	if (length < req->ticket.size()) return (int)SCE_NP_MANAGER_ERROR_INVALID_TICKET_SIZE;
	mem::WriteBytes(bufPtr, req->ticket.data(), (u32)req->ticket.size());
	return (int)req->ticket.size();
}
int sceNpAuthDestroyRequest(int id) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	auto req = FindRequest(id);
	if (!req) return (int)SCE_NP_AUTH_ERROR_ESRCH;
	req->aborted = true;
	g_authRequests.erase(id);
	return 0;
}
int sceNpAuthAbortRequest(int id) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	auto req = FindRequest(id);
	if (!req) return (int)SCE_NP_AUTH_ERROR_ESRCH;
	req->aborted = true;
	return 0;
}

// sceNpAuthGetTicketParam(ticket, size, paramId, SceNpTicketParam *out): one body field, ids in
// body order (0 serial ... 9 status, 10-11 the trailing nulls). Numbers come out as host integers
// (u32, or u64 for longs and dates), strings and binaries as their bytes; the union is 256 bytes.
int sceNpAuthGetTicketParam(u32 ticketPtr, u32 ticketSize, int paramId, u32 outPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	if (ticketSize == 0 || ticketSize > 0x10000 || !mem::IsValidRange(ticketPtr, ticketSize) || !mem::IsValidRange(outPtr, 256))
		return (int)SCE_NP_AUTH_ERROR_EINVAL;
	std::vector<u8> t(ticketSize);
	mem::ReadBytes(t.data(), ticketPtr, ticketSize);
	std::vector<np::TicketParam> params;
	if (!np::ParseTicket(t, nullptr, &params) || paramId < 0 || paramId >= (int)params.size())
		return (int)SCE_NP_AUTH_ERROR_EINVAL;
	const np::TicketParam &p = params[(size_t)paramId];
	u8 out[256] = {};
	auto be = [&](size_t n) {
		u64 v = 0;
		for (size_t i = 0; i < n && i < p.data.size(); i++) v = (v << 8) | p.data[i];
		return v;
	};
	if (p.type == 1) {
		const u32 v = (u32)be(4);
		memcpy(out, &v, 4);
	} else if (p.type == 2 || p.type == 7) {
		const u64 v = be(8);
		memcpy(out, &v, 8);
	} else {
		memcpy(out, p.data.data(), std::min<size_t>(p.data.size(), sizeof(out)));
	}
	mem::WriteBytes(outPtr, out, sizeof(out));
	return 0;
}
// PSPC Network accounts own no entitlements.
int sceNpAuthGetEntitlementIdList(u32 ticketPtr, u32 ticketSize, u32 listPtr, u32 maxCount) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	return 0;
}
int sceNpAuthGetEntitlementById(u32 ticketPtr, u32 ticketSize, u32 entitlementIdPtr, u32 outPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	return (int)SCE_NP_AUTH_ERROR_EINVAL;
}

// ---------------------------------------------------------------- sceNpService

int sceNpServiceInit(u32 poolSize, u32 stackSize, u32 threadPrio) {
	if (g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_ALREADY_INITIALIZED;
	g_npServiceMem.memSize = (s32)poolSize - 0x20;
	g_npServiceMem.maxMemSize = 0x4050;
	g_npServiceMem.freeMemSize = g_npServiceMem.memSize;
	g_npServiceInited = true;
	return 0;
}
int sceNpServiceTerm() {
	g_npServiceInited = false;
	return 0;
}
int sceNpServiceGetMemoryStat(u32 statPtr) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statPtr, sizeof(NpMemStat))) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	mem::WriteBytes(statPtr, &g_npServiceMem, sizeof(g_npServiceMem));
	return 0;
}

// Friend lists (roster): the account's friends on the PSPC Network server (mutual friends; none in
// local mode). From np_service.prx 4.01: sceNpRosterGetFriendListEntry(roster, maxEntries,
// startIndex, u32 *returned, u32 *total, entries, opt) fills 40-byte entries { u32 status;
// SceNpId npId; } (the reply's record type 0x1009) and returns *returned;
// GetFriendListEntryCount(roster, opt) returns the count. The status byte's meaning isn't known;
// entries carry 0.
std::map<int, bool> g_rosterRequests;
int g_nextRoster = 1;

// The signed-in account's mutual friends (blocks the calling guest thread on the server).
std::vector<std::string> FetchFriends() {
	std::vector<std::string> out;
	if (!HostedMode() || !SignedIn()) return out;
	const net::Config cfg = net::CurrentConfig();
	struct Out {
		np::Result r;
		std::vector<np::Friend> friends;
	};
	auto res = std::make_shared<Out>();
	if (!net::RunBlockingOnHost(
			[res, cfg]() {
				np::NetworkClient client(cfg.npServer, cfg.npToken);
				res->r = client.Friends(&res->friends);
			},
			sched::NowUs() + kServerTimeoutUs)) {
		LOGW(LOG, "Friend list: the PSPC Network server didn't answer");
		return out;
	}
	if (!res->r) {
		LOGW(LOG, "Friend list: %s", res->r.message.c_str());
		return out;
	}
	for (const np::Friend &f : res->friends)
		if (f.status == "friend") out.push_back(f.onlineId);
	return out;
}

int sceNpRosterCreateRequest(u32 paramPtr) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	if (!SignedIn()) return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
	const int id = g_nextRoster++;
	g_rosterRequests[id] = true;
	return id;
}
int sceNpRosterDeleteRequest(int id) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	return g_rosterRequests.erase(id) ? 0 : (int)SCE_NP_SERVICE_ERROR_ESRCH;
}
int sceNpRosterAbort(int id) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	return g_rosterRequests.count(id) ? 0 : (int)SCE_NP_SERVICE_ERROR_ESRCH;
}
int sceNpRosterGetFriendListEntry(int id, u32 maxEntries, u32 start, u32 returnedPtr, u32 totalPtr, u32 entriesPtr, u32 opt) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	if (!g_rosterRequests.count(id)) return (int)SCE_NP_SERVICE_ERROR_ESRCH;
	if (opt && (!mem::IsValidRange(opt, 4) || GRead32(opt) != 4)) return (int)SCE_NP_SERVICE_ERROR_EINVAL;
	const std::vector<std::string> friends = FetchFriends();
	maxEntries &= 0xFF;
	start &= 0xFF;
	u32 n = 0;
	for (u32 i = start; i < friends.size() && n < maxEntries; i++, n++) {
		const u32 e = entriesPtr + n * 40;
		if (!mem::IsValidRange(e, 40)) break;
		mem::Memset(e, 0, 40);  // status 0, then the SceNpId
		WriteOnlineId(e + 4, friends[i]);
	}
	if (returnedPtr && mem::IsValidRange(returnedPtr, 4)) GWrite32(returnedPtr, n);
	if (totalPtr && mem::IsValidRange(totalPtr, 4)) GWrite32(totalPtr, n);
	LOGI(LOG, "Friend list: %u of %zu friends", n, friends.size());
	return (int)n;
}
int sceNpRosterGetFriendListEntryCount(int id, u32 opt) {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	if (!g_rosterRequests.count(id)) return (int)SCE_NP_SERVICE_ERROR_ESRCH;
	if (opt && (!mem::IsValidRange(opt, 4) || GRead32(opt) != 4)) return (int)SCE_NP_SERVICE_ERROR_EINVAL;
	return (int)FetchFriends().size();
}
int sceNpServiceUnsupported() {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	return SignedIn() ? (int)SCE_NP_SERVICE_ERROR_EINVAL : (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
}

}  // namespace

namespace net {

void NpStartSignIn() {
	std::unique_lock<std::mutex> lock(g_mu);
	if (g_signIn == NpSigninState::SignedIn || g_signIn == NpSigninState::Working) return;
	const Config cfg = CurrentConfig();
	if (!Enabled()) {
		g_signIn = NpSigninState::Failed;
		g_signInMessage = "The WLAN switch is off (network=0 in pspc.ini).";
		return;
	}
	if (np::NormalizeServerUrl(cfg.npServer).empty()) {
		g_profile = np::Profile();
		g_profile.onlineId = LocalOnlineId(cfg.npOnlineId);
		g_profile.accountId = LocalAccountId(g_profile.onlineId);
		g_localAccount = true;
		g_signIn = NpSigninState::SignedIn;
		g_signInMessage.clear();
		LOGI(LOG, "Signed in locally as %s (no PSPC Network server set)", g_profile.onlineId.c_str());
		return;
	}
	if (cfg.npToken.empty()) {
		g_signIn = NpSigninState::Failed;
		g_signInMessage = "Sign in to PSPC Network first: the launcher's Network button.";
		LOGW(LOG, "Sign-in: no PSPC Network account (np_token) in pspc.ini");
		return;
	}
	g_signIn = NpSigninState::Working;
	g_signInMessage.clear();
	lock.unlock();
	std::thread([cfg]() {
		np::NetworkClient client(cfg.npServer, cfg.npToken);
		np::Profile profile;
		const np::Result r = client.Me(&profile);
		std::lock_guard<std::mutex> l(g_mu);
		if (r) {
			g_profile = profile;
			g_localAccount = false;
			g_signIn = NpSigninState::SignedIn;
			LOGI(LOG, "Signed in to PSPC Network (%s) as %s", client.Server().c_str(), profile.onlineId.c_str());
		} else {
			g_signIn = NpSigninState::Failed;
			g_signInMessage = r.httpStatus == 401 ? "The PSPC Network sign-in has expired: sign in again in the launcher."
			                                      : r.message;
			LOGW(LOG, "Sign-in failed: %s", r.message.c_str());
		}
	}).detach();
}

NpSigninState NpGetSignInState(std::string *message) {
	std::lock_guard<std::mutex> lock(g_mu);
	if (message) *message = g_signInMessage;
	return g_signIn;
}

void NpResetSignIn() {
	std::lock_guard<std::mutex> lock(g_mu);
	if (g_signIn == NpSigninState::Failed) {
		g_signIn = NpSigninState::Idle;
		g_signInMessage.clear();
	}
}

std::string NpOnlineId() { return CurrentProfile().onlineId; }

void RegisterNpLibraries() {
	static const hle::HleFunction sceNp[] = {
		HLEFN(0x857B47D3, sceNpInit),
		HLEFN(0x37E1E274, sceNpTerm),
		HLEFN(0x633B5F71, sceNpGetNpId),
		HLEFN(0x4B5C71C8, sceNpGetOnlineId),
		HLEFN(0x7E0864DF, sceNpGetUserProfile),
		HLEFN(0xA0BE3C4B, sceNpGetAccountRegion),
		HLEFN(0xBB069A87, sceNpGetContentRatingFlag),
		HLEFN(0x1D60AE4B, sceNpGetChatRestrictionFlag),
		HLEFN(0xCDCC21D3, sceNpGetMyLanguages),
	};
	hle::RegisterModule("sceNp", sceNp);

	static const hle::HleFunction sceNpAuth[] = {
		HLEFN(0xA1DE86F8, sceNpAuthInit),
		HLEFN(0x4EC1F667, sceNpAuthTerm),
		HLEFN(0xF4531ADC, sceNpAuthGetMemoryStat),
		HLEFN(0xCD86A656, sceNpAuthCreateStartRequest),
		HLEFN(0x3F1C1F70, sceNpAuthGetTicket),
		HLEFN(0x6900F084, sceNpAuthGetEntitlementById),
		HLEFN(0x72BB0467, sceNpAuthDestroyRequest),
		HLEFN(0xD99455DD, sceNpAuthAbortRequest),
		HLEFN(0x5A3CB57A, sceNpAuthGetTicketParam),
		HLEFN(0x75FB0AE3, sceNpAuthGetEntitlementIdList),
	};
	hle::RegisterModule("sceNpAuth", sceNpAuth);

	static const hle::HleFunction sceNpService[] = {
		HLEFN(0x0F8F5821, sceNpServiceInit),
		HLEFN(0x00ACFAC3, sceNpServiceTerm),
		HLEFN(0x250488F9, sceNpServiceGetMemoryStat),
		HLEFN(0xBE22EEA3, sceNpRosterCreateRequest),
		HLEFN(0x66C64821, sceNpRosterDeleteRequest),
		HLEFN(0x58251346, sceNpRosterGetFriendListEntryCount),
		HLEFN(0x4E851B10, sceNpRosterGetFriendListEntry),
		HLEFN(0xA01443AA, sceNpServiceUnsupported),  // sceNpRosterGetBlockListEntryCount
		HLEFN(0x506C318D, sceNpServiceUnsupported),  // sceNpRosterGetBlockListEntry
		HLEFN(0x788F2B5E, sceNpServiceUnsupported),  // sceNpRosterAddFriendListEntry
		HLEFN(0x5F5E32AF, sceNpRosterAbort),
		HLEFN(0x5494274B, sceNpServiceUnsupported),  // sceNpLookupCreateTransactionCtx
		HLEFN(0xA670D3A3, sceNpServiceUnsupported),  // sceNpLookupDestroyTransactionCtx
		HLEFN(0x4B4E4E71, sceNpServiceUnsupported),  // sceNpLookupAbortTransaction
		HLEFN(0xC76F55ED, sceNpServiceUnsupported),  // sceNpLookupTitleSmallStorage
	};
	hle::RegisterModule("sceNpService", sceNpService);
}

}  // namespace net
