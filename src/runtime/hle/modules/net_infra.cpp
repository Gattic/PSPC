// SPDX-License-Identifier: GPL-2.0-or-later
//
// Online-service redirects: Sony's and publishers' servers are gone, so a game's host names are
// pointed at revival servers instead.
//
// The table is a list of games, each with the disc IDs it covers, host names mapped to addresses
// ("domains") and/or a revival DNS server that answers for all of the game's names ("dns"). It is
// read from, in order (the first entry naming the running game wins): infrastructure.json next to
// pspc.exe (the player's own), data/infrastructure.json (shipped), and PPSSPP's infra-dns.json in
// either place, whose format this is (games[].known_working_ids / other_ids / not_working_ids,
// domains, dns). pspc.ini infra_dns= is a revival DNS server for every game.
//
// A domain "*.example.com" covers every name under example.com (the longest such match wins); its
// target may be "*.other.net", which keeps the part "*" matched: "*.gamespy.com": "*.openspy.net"
// sends gpcm.gamespy.com to gpcm.openspy.net.
//
// Games resolve names three ways and all are covered: sceNetResolver, sceHttp, and DNS queries
// they send themselves over UDP (SCE-RT / Medius games do), which the socket layer hands to
// InfraAnswerDnsQuery. With a revival DNS server the access point also reports it as the DNS server.
#include <algorithm>
#include <cctype>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common/fileutil.h"
#include "common/json.h"
#include "common/log.h"
#include "net/dns.h"
#include "runtime/hle/modules/net_internal.h"
#include "runtime/media/disc.h"

namespace net {

namespace {

constexpr const char *LOG = "INFRA";

struct GameEntry {
	std::string name;
	std::map<std::string, std::string> domains;  // lowercase host -> IPv4 literal or host name
	std::string dns;                             // revival DNS server (IPv4 literal or host name)
};

std::mutex g_mu;
bool g_loaded = false;
bool g_haveEntry = false;
GameEntry g_entry;
u32 g_dnsServer = 0;  // network order; 0 = none
std::map<std::string, u32> g_cache;  // host -> address, for this run

std::string Lower(std::string s) {
	for (char &c : s) c = (char)tolower((unsigned char)c);
	return s;
}

bool ReadTable(const fs::path &file, const std::string &discId, GameEntry *out) {
	std::vector<u8> data;
	if (!ReadWholeFile(file, data)) return false;
	std::string error;
	const json::Value doc = json::Value::Parse(std::string(data.begin(), data.end()), &error);
	if (doc.IsNull()) {
		LOGW(LOG, "%s: not valid JSON (%s)", PathToUtf8(file).c_str(), error.c_str());
		return false;
	}
	for (const json::Value &g : doc["games"].Array()) {
		bool match = false;
		for (const char *key : {"ids", "known_working_ids", "other_ids", "not_working_ids"})
			for (const json::Value &id : g[key].Array())
				if (!discId.empty() && Lower(id.String()) == Lower(discId)) match = true;
		if (!match) continue;
		out->name = g["name"].String(discId);
		out->dns = g["dns"].String();
		for (const auto &kv : g["domains"].Members()) out->domains[Lower(kv.first)] = kv.second.String();
		LOGI(LOG, "%s: %s is %s (%zu host names%s%s)", PathToUtf8(file).c_str(), discId.c_str(), out->name.c_str(),
		     out->domains.size(), out->dns.empty() ? "" : ", DNS ", out->dns.c_str());
		return true;
	}
	return false;
}

// The target of a "*.suffix" domain covering host ("" if none does).
std::string WildcardTarget(const std::map<std::string, std::string> &domains, const std::string &host) {
	std::string target, prefix;
	size_t best = 0;
	for (const auto &kv : domains) {
		if (kv.first.size() < 3 || kv.first.compare(0, 2, "*.") != 0) continue;
		const std::string suffix = kv.first.substr(1);  // ".example.com"
		if (host.size() <= suffix.size() || suffix.size() <= best ||
		    host.compare(host.size() - suffix.size(), suffix.size(), suffix) != 0)
			continue;
		best = suffix.size();
		target = kv.second;
		prefix = host.substr(0, host.size() - suffix.size());
	}
	if (target.size() > 2 && target.compare(0, 2, "*.") == 0) target = prefix + target.substr(1);
	return target;
}

u32 ResolveAddress(const std::string &addr) {
	u32 a = 0;
	if (addr.empty()) return 0;
	if (hostnet::ParseIpv4(addr.c_str(), &a)) return a;
	return hostnet::ResolveIpv4(addr, &a) == 0 ? a : 0;
}

void LoadLocked() {
	if (g_loaded) return;
	g_loaded = true;
	const Config &cfg = CurrentConfig();
	const std::string discId = media::DiscId();
	std::vector<fs::path> files = {ExecutableDir() / "infrastructure.json"};
	if (!cfg.dataDir.empty()) files.push_back(fs::u8path(cfg.dataDir) / "infrastructure.json");
	files.push_back(ExecutableDir() / "infra-dns.json");
	if (!cfg.dataDir.empty()) files.push_back(fs::u8path(cfg.dataDir) / "infra-dns.json");
	for (const fs::path &f : files) {
		if (ReadTable(f, discId, &g_entry)) {
			g_haveEntry = true;
			break;
		}
	}
	const std::string dns = !cfg.infraDns.empty() ? cfg.infraDns : g_haveEntry ? g_entry.dns : std::string();
	if (!dns.empty()) {
		g_dnsServer = ResolveAddress(dns);
		if (g_dnsServer) LOGI(LOG, "Revival DNS server %s (%s)", dns.c_str(), hostnet::Ipv4ToString(g_dnsServer).c_str());
		else LOGW(LOG, "Revival DNS server %s: can't resolve it", dns.c_str());
	}
}

}  // namespace

u32 InfraDnsServer() {
	std::lock_guard<std::mutex> lock(g_mu);
	LoadLocked();
	return g_dnsServer;
}

bool InfraRedirect(const std::string &hostIn, u32 *addr) {
	const std::string host = Lower(hostIn);
	std::string target;
	u32 dnsServer;
	{
		std::lock_guard<std::mutex> lock(g_mu);
		LoadLocked();
		auto c = g_cache.find(host);
		if (c != g_cache.end()) {
			*addr = c->second;
			return c->second != 0;
		}
		auto it = g_entry.domains.find(host);
		target = it != g_entry.domains.end() ? it->second : WildcardTarget(g_entry.domains, host);
		dnsServer = g_dnsServer;
	}
	u32 a = 0;
	if (!target.empty()) {
		a = ResolveAddress(target);
		LOGI(LOG, "%s -> %s (%s)", host.c_str(), target.c_str(), a ? hostnet::Ipv4ToString(a).c_str() : "unresolvable");
	} else if (dnsServer) {
		if (!dns::Lookup(dnsServer, host, &a, 2000)) a = 0;
		LOGI(LOG, "%s -> %s (revival DNS)", host.c_str(), a ? hostnet::Ipv4ToString(a).c_str() : "not found");
	} else {
		return false;
	}
	std::lock_guard<std::mutex> lock(g_mu);
	g_cache[host] = a;
	*addr = a;
	return a != 0;
}

bool InfraHasRedirects() {
	std::lock_guard<std::mutex> lock(g_mu);
	LoadLocked();
	return !g_entry.domains.empty() || g_dnsServer != 0;
}

bool InfraAnswerDnsQuery(const u8 *query, size_t size, std::vector<u8> *response) {
	dns::Question q;
	if (!dns::ParseQuery(query, size, &q) || q.qtype != dns::TYPE_A || q.qclass != dns::CLASS_IN) return false;
	u32 a = 0;
	if (!InfraRedirect(q.name, &a)) return false;
	*response = dns::BuildAnswer(std::vector<u8>(query, query + size), q, a);
	return true;
}

}  // namespace net
