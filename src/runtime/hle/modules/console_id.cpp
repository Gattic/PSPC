// SPDX-License-Identifier: GPL-2.0-or-later
// The per-installation console identity (see console_id.h).
#include "runtime/hle/modules/console_id.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>
#include <string>

#include "common/fileutil.h"
#include "common/log.h"
#include "common/sha1.h"
#include "runtime/io/io.h"

namespace console {

namespace {

constexpr const char *LOG = "IDSTORAGE";
constexpr const char *kFile = "flash1:/pspc_idstorage.txt";
// The organisation prefix of the MAC addresses PSPs ship with (Alps Electric, the WLAN module's
// maker); the rest is per installation.
constexpr u8 kMacPrefix[3] = {0x00, 0x16, 0xFE};

bool g_loaded = false;
Identity g_id{};

bool ParseMac(const std::string &s, u8 out[6]) {
	unsigned v[6];
	if (sscanf(s.c_str(), "%2x:%2x:%2x:%2x:%2x:%2x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) return false;
	for (int i = 0; i < 6; i++) out[i] = (u8)v[i];
	return true;
}

void Generate(Identity &id, const std::string &salt) {
	// Random where the host has a source of it; otherwise derived from the host path and the time.
	u8 bytes[16];
	try {
		std::random_device rd;
		for (int i = 0; i < 16; i += 4) {
			const u32 r = rd();
			memcpy(bytes + i, &r, 4);
		}
	} catch (...) {
		const auto now = std::chrono::system_clock::now().time_since_epoch().count();
		const std::string seed = salt + std::to_string(now);
		const auto h = Sha1::Hash(seed.data(), seed.size());
		memcpy(bytes, h.data(), 16);
	}
	memcpy(id.mac, kMacPrefix, 3);
	memcpy(id.mac + 3, bytes, 3);
	memcpy(&id.fuseId, bytes + 8, 8);
	id.fuseId &= 0x0000FFFFFFFFFFFFull;  // fuse IDs are 48 bits
	if (id.fuseId == 0) id.fuseId = 1;
}

void Load() {
	g_loaded = true;
	const std::string host = io::HostPathFor(kFile);
	std::vector<u8> data;
	bool haveMac = false, haveFuse = false;
	if (!host.empty() && ReadWholeFile(fs::u8path(host), data)) {
		std::istringstream in(std::string(data.begin(), data.end()));
		std::string line;
		while (std::getline(in, line)) {
			while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
			if (line.rfind("mac=", 0) == 0) haveMac = ParseMac(line.substr(4), g_id.mac);
			if (line.rfind("fuse=", 0) == 0) {
				g_id.fuseId = strtoull(line.c_str() + 5, nullptr, 16);
				haveFuse = g_id.fuseId != 0;
			}
		}
	}
	if (haveMac && haveFuse) return;
	Identity fresh{};
	Generate(fresh, host);
	if (!haveMac) memcpy(g_id.mac, fresh.mac, 6);
	if (!haveFuse) g_id.fuseId = fresh.fuseId;
	char text[256];
	snprintf(text, sizeof(text),
	         "# PSPC console identity: what a PSP keeps in its ID storage and fuses. Delete this file\n"
	         "# to make a new one (savedata made with the old one then reads as copied from another PSP).\n"
	         "mac=%02X:%02X:%02X:%02X:%02X:%02X\nfuse=%012llX\n",
	         g_id.mac[0], g_id.mac[1], g_id.mac[2], g_id.mac[3], g_id.mac[4], g_id.mac[5],
	         (unsigned long long)g_id.fuseId);
	if (host.empty() || !WriteTextFile(fs::u8path(host), text))
		LOGW(LOG, "Cannot write %s; the console identity lasts for this run only", kFile);
	else
		LOGI(LOG, "New console identity in %s", kFile);
}

}  // namespace

const Identity &Get() {
	if (!g_loaded) Load();
	return g_id;
}

int IdStorageLookup(u16 key, u32 offset, u8 *buf, u32 len) {
	// Leaves are 512 bytes. The ones the 4.01 XMB reads: 0x0044 the WLAN MAC address (System
	// Information), 0x0046 the sound reduction default (impose), 0x0047 the default parental level,
	// 0x0054 the body colour of a PSP-2000/3000 (theme defaults); the last three only when it
	// rebuilds the registry. A PSPC console has no sound reduction, no parental lock and the
	// standard colour scheme.
	u8 leaf[512] = {};
	switch (key) {
	case 0x0044: memcpy(leaf, Get().mac, 6); break;
	case 0x0046:
	case 0x0047:
	case 0x0054: break;
	default: return (int)0x80000025;
	}
	if (offset >= sizeof(leaf) || len > sizeof(leaf) - offset) return (int)0x80000104;
	memcpy(buf, leaf + offset, len);
	return 0;
}

}  // namespace console
