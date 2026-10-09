// SPDX-License-Identifier: GPL-2.0-or-later
//
// PlayStation Network libraries, offline: sceNp, sceNpAuth, sceNpService. They initialize, the
// account-independent settings read fine, and anything that needs a signed-in PSN account fails
// with NOT_SIGNIN.
//
// NIDs, error codes and the content-rating / language defaults from PPSSPP (Core/HLE/sceNp.cpp,
// sceNp.h), Copyright (c) 2012- PPSSPP Project, GPL-2.0-or-later.
#include "common/log.h"
#include "runtime/hle/hle_wrap.h"
#include "runtime/hle/modules/net_internal.h"

namespace {

constexpr u32 SCE_NP_ERROR_ALREADY_INITIALIZED = 0x80550001;
constexpr u32 SCE_NP_ERROR_NOT_INITIALIZED = 0x80550002;
constexpr u32 SCE_NP_ERROR_INVALID_ARGUMENT = 0x80550003;
constexpr u32 SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED = 0x80550301;
constexpr u32 SCE_NP_AUTH_ERROR_NOT_INITIALIZED = 0x80550302;
constexpr u32 SCE_NP_AUTH_ERROR_EINVAL = 0x80550303;
constexpr u32 SCE_NP_AUTH_ERROR_ESRCH = 0x80550305;
constexpr u32 SCE_NP_MANAGER_ERROR_NOT_SIGNIN = 0x8055050B;
constexpr u32 SCE_NP_SERVICE_ERROR_ALREADY_INITIALIZED = 0x80550701;
constexpr u32 SCE_NP_SERVICE_ERROR_NOT_INITIALIZED = 0x80550702;

bool g_npInited = false, g_npAuthInited = false, g_npServiceInited = false;
struct NpMemStat {
	s32 memSize, maxMemSize, freeMemSize;
};
NpMemStat g_npAuthMem{}, g_npServiceMem{};

// ---------------------------------------------------------------- sceNp

int sceNpInit() {
	if (g_npInited) return (int)SCE_NP_ERROR_ALREADY_INITIALIZED;
	g_npInited = true;
	return 0;
}
int sceNpTerm() {
	g_npInited = false;
	return 0;
}
// Needs the PSN account: not signed in.
int sceNpNotSignedIn(u32 ptr) {
	if (!mem::IsValidAddress(ptr)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
}
int sceNpGetAccountRegion(u32 countryPtr, u32 regionPtr) {
	if (!mem::IsValidAddress(countryPtr) || !mem::IsValidAddress(regionPtr)) return (int)SCE_NP_ERROR_INVALID_ARGUMENT;
	return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
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

int sceNpAuthInit(u32 poolSize, u32 stackSize, u32 threadPrio) {
	if (g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_ALREADY_INITIALIZED;
	g_npAuthMem.memSize = (s32)poolSize - 0x20;
	g_npAuthMem.maxMemSize = 0x4050;
	g_npAuthMem.freeMemSize = g_npAuthMem.memSize;
	g_npAuthInited = true;
	return 0;
}
int sceNpAuthTerm() {
	g_npAuthInited = false;
	return 0;
}
int sceNpAuthGetMemoryStat(u32 statPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidRange(statPtr, sizeof(NpMemStat))) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	mem::WriteBytes(statPtr, &g_npAuthMem, sizeof(g_npAuthMem));
	return 0;
}
int sceNpAuthCreateStartRequest(u32 paramPtr) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	if (!mem::IsValidAddress(paramPtr)) return (int)SCE_NP_AUTH_ERROR_EINVAL;
	return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
}
// No request can exist.
int sceNpAuthNoRequest(int id) {
	if (!g_npAuthInited) return (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED;
	return (int)SCE_NP_AUTH_ERROR_ESRCH;
}
int sceNpAuthGetTicket(int id, u32, u32) { return sceNpAuthNoRequest(id); }
int sceNpAuthGetTicketParam(u32, int, int, u32) { return g_npAuthInited ? (int)SCE_NP_AUTH_ERROR_EINVAL : (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED; }
int sceNpAuthGetEntitlementIdList(u32, int, u32, int) { return g_npAuthInited ? (int)SCE_NP_AUTH_ERROR_EINVAL : (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED; }
int sceNpAuthGetEntitlementById(u32, int, u32, u32) { return g_npAuthInited ? (int)SCE_NP_AUTH_ERROR_EINVAL : (int)SCE_NP_AUTH_ERROR_NOT_INITIALIZED; }

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
// Friend lists, lookups and title storage all go through the signed-in account.
int sceNpServiceNotSignedIn() {
	if (!g_npServiceInited) return (int)SCE_NP_SERVICE_ERROR_NOT_INITIALIZED;
	return (int)SCE_NP_MANAGER_ERROR_NOT_SIGNIN;
}

}  // namespace

namespace net {

void RegisterNpLibraries() {
	static const hle::HleFunction sceNp[] = {
		HLEFN(0x857B47D3, sceNpInit),
		HLEFN(0x37E1E274, sceNpTerm),
		HLEFN(0x633B5F71, sceNpNotSignedIn),  // sceNpGetNpId
		HLEFN(0x4B5C71C8, sceNpNotSignedIn),  // sceNpGetOnlineId
		HLEFN(0x7E0864DF, sceNpNotSignedIn),  // sceNpGetUserProfile
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
		HLEFN(0x72BB0467, sceNpAuthNoRequest),  // sceNpAuthDestroyRequest
		HLEFN(0xD99455DD, sceNpAuthNoRequest),  // sceNpAuthAbortRequest
		HLEFN(0x5A3CB57A, sceNpAuthGetTicketParam),
		HLEFN(0x75FB0AE3, sceNpAuthGetEntitlementIdList),
	};
	hle::RegisterModule("sceNpAuth", sceNpAuth);

	static const hle::HleFunction sceNpService[] = {
		HLEFN(0x0F8F5821, sceNpServiceInit),
		HLEFN(0x00ACFAC3, sceNpServiceTerm),
		HLEFN(0x250488F9, sceNpServiceGetMemoryStat),
		HLEFN(0xBE22EEA3, sceNpServiceNotSignedIn),  // sceNpRosterCreateRequest
		HLEFN(0x66C64821, sceNpServiceNotSignedIn),  // sceNpRosterDeleteRequest
		HLEFN(0x58251346, sceNpServiceNotSignedIn),  // sceNpRosterGetFriendListEntryCount
		HLEFN(0x4E851B10, sceNpServiceNotSignedIn),  // sceNpRosterGetFriendListEntry
		HLEFN(0xA01443AA, sceNpServiceNotSignedIn),  // sceNpRosterGetBlockListEntryCount
		HLEFN(0x506C318D, sceNpServiceNotSignedIn),  // sceNpRosterGetBlockListEntry
		HLEFN(0x788F2B5E, sceNpServiceNotSignedIn),  // sceNpRosterAddFriendListEntry
		HLEFN(0x5F5E32AF, sceNpServiceNotSignedIn),  // sceNpRosterAbort
		HLEFN(0x5494274B, sceNpServiceNotSignedIn),  // sceNpLookupCreateTransactionCtx
		HLEFN(0xA670D3A3, sceNpServiceNotSignedIn),  // sceNpLookupDestroyTransactionCtx
		HLEFN(0x4B4E4E71, sceNpServiceNotSignedIn),  // sceNpLookupAbortTransaction
		HLEFN(0xC76F55ED, sceNpServiceNotSignedIn),  // sceNpLookupTitleSmallStorage
	};
	hle::RegisterModule("sceNpService", sceNpService);
}

}  // namespace net
