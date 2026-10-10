// SPDX-License-Identifier: GPL-2.0-or-later
// PSPC Network client pieces: the JSON reader, NP tickets (the layout server/pspc_network/ticket.py
// signs), and the DNS messages used to send games to revival servers.
#include <cstring>

#include "common/json.h"
#include "net/dns.h"
#include "net/np_client.h"
#include "net/np_ticket.h"
#include "testing.h"

TEST(json_reader) {
	std::string err;
	json::Value v = json::Value::Parse(
		"{\"a\": 1.5, \"s\": \"x\\\"y\\u00e9\\ud83d\\ude00\", \"t\": true, \"n\": null, \"l\": [1, {\"k\": \"v\"}], \"o\": {}}", &err);
	CHECK(v.IsObject());
	CHECK(err.empty());
	CHECK(v["a"].Number() == 1.5);
	CHECK(v["s"].String() == "x\"y\xc3\xa9\xf0\x9f\x98\x80");
	CHECK(v["t"].Bool());
	CHECK(v["n"].IsNull() && v.Has("n"));
	CHECK(v["l"].Array().size() == 2);
	CHECK(v["l"].Array()[1]["k"].String() == "v");
	CHECK(v["o"].Members().empty());
	CHECK(v["missing"]["deeper"].IsNull());
	CHECK(v["a"].String("def") == "def");
	CHECK(json::Value::Parse("{\"a\":}", &err).IsNull() && !err.empty());
	CHECK(json::Value::Parse("[1,2] x").IsNull());
	CHECK(json::Quote("a\"b\\c\n\x01") == "\"a\\\"b\\\\c\\n\\u0001\"");
}

TEST(np_ticket_layout) {
	np::TicketFields f;
	for (int i = 0; i < 20; i++) f.serial[i] = (u8)i;
	f.issuedMs = 1000;
	f.expiresMs = 2000;
	f.accountId = 0x1234567890ABCDEFull;
	f.onlineId = "Gabe_Logan";
	f.country = "gb";
	f.serviceId = "UP9000-NPUG80114_00";
	std::vector<u8> t = np::BuildLocalTicket(f);
	// Header: version 2.1, then the size of the rest (big-endian), then the body section.
	CHECK(t.size() == 224);  // as large as the server's tickets
	CHECK(t[0] == 0x21 && t[1] == 0x01 && t[2] == 0 && t[3] == 0);
	CHECK(((u32)t[4] << 24 | (u32)t[5] << 16 | (u32)t[6] << 8 | t[7]) == t.size() - 8);
	CHECK(t[8] == 0x30 && t[9] == 0x00);
	CHECK(t[12] == 0 && t[13] == 8 && t[14] == 0 && t[15] == 20);  // serial: binary, 20 bytes
	np::TicketFields g;
	std::vector<np::TicketParam> params;
	CHECK(np::ParseTicket(t, &g, &params));
	CHECK(g.onlineId == "Gabe_Logan");
	CHECK(g.accountId == 0x1234567890ABCDEFull);
	CHECK(g.issuedMs == 1000 && g.expiresMs == 2000);
	CHECK(g.country == "gb" && g.domain == "c9");
	CHECK(g.serviceId == "UP9000-NPUG80114_00");
	CHECK(g.cipher == "PSPL");
	CHECK(memcmp(g.serial, f.serial, 20) == 0);
	CHECK(params.size() == 12);
	CHECK(params[5].type == 4 && params[5].data.size() == 32);  // online ID: string[32]
	CHECK(params[10].type == 0 && params[11].type == 0);
	std::vector<u8> bad = t;
	bad[7] ^= 1;  // wrong size
	CHECK(!np::ParseTicket(bad, nullptr));
	CHECK(!np::ParseTicket(std::vector<u8>(5, 0), nullptr));
}

TEST(np_server_url) {
	CHECK(np::NormalizeServerUrl("np.example.com/") == "https://np.example.com");
	CHECK(np::NormalizeServerUrl(" http://127.0.0.1:8423 ") == "http://127.0.0.1:8423");
	CHECK(np::NormalizeServerUrl("https://x.y") == "https://x.y");
	CHECK(np::NormalizeServerUrl("").empty());
}

TEST(dns_messages) {
	// SFCO's lookup of its lobby server, as it sends it.
	const u8 sfco[] = {0x00, 0x00, 0x01, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x09, 'c', 'o', 'm', 'b', 'a', 't', 'o',
		'p', 's', 0x03, 'p', 's', 'p', 0x06, 'o', 'n', 'l', 'i', 'n', 'e', 0x04, 's', 'c', 'e', 'a', 0x03, 'c', 'o', 'm', 0x00, 0x00, 0x01, 0x00, 0x01};
	dns::Question q;
	CHECK(dns::ParseQuery(sfco, sizeof(sfco), &q));
	CHECK(q.name == "combatops.psp.online.scea.com");
	CHECK(q.qtype == dns::TYPE_A && q.qclass == dns::CLASS_IN && q.id == 0);
	const u32 addr = 0x0100007F;  // 127.0.0.1, network order
	std::vector<u8> answer = dns::BuildAnswer(std::vector<u8>(sfco, sfco + sizeof(sfco)), q, addr);
	u32 got = 0;
	CHECK(dns::ParseAnswer(answer.data(), answer.size(), 0, &got));
	CHECK(got == addr);
	CHECK(!dns::ParseAnswer(answer.data(), answer.size(), 1, &got));  // another query's
	// A query we build round-trips, and a response isn't taken for a query.
	std::vector<u8> mine = dns::BuildQuery(0x1234, "Gpcm.GameSpy.com");
	CHECK(dns::ParseQuery(mine.data(), mine.size(), &q));
	CHECK(q.name == "gpcm.gamespy.com" && q.id == 0x1234);
	CHECK(!dns::ParseQuery(answer.data(), answer.size(), &q));
	// A CNAME before the A record (as resolvers answer), with name compression.
	const u8 cname[] = {0x12, 0x34, 0x81, 0x80, 0, 1, 0, 2, 0, 0, 0, 0, 1, 'a', 0, 0, 1, 0, 1,
		0xC0, 0x0C, 0, 5, 0, 1, 0, 0, 0, 60, 0, 4, 1, 'b', 0xC0, 0x0E,
		0xC0, 0x1F, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 10, 0, 0, 7};
	CHECK(dns::ParseAnswer(cname, sizeof(cname), 0x1234, &got));
	CHECK(got == 0x0700000A);
}
