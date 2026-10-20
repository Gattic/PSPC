# SPDX-License-Identifier: GPL-2.0-or-later
"""deploy/swss/session_report.py reads the server's log: stages reached per client and what looks wrong."""

import importlib.util
import io
import os
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location('session_report', os.path.join(HERE, '..', 'deploy', 'swss', 'session_report.py'))
session_report = importlib.util.module_from_spec(spec)
spec.loader.exec_module(session_report)

GOOD_VISIT = """\
2026-10-20 12:00:01 qr: swsspsp hosts SWSS Conquest on 127.0.0.1:47754, UDP ports players use: 3658, 47754 (map Bespin, 0/24 players)
2026-10-20 12:01:00 gpcm: new account Alice (a@example.com), profile 100000001, from 203.0.113.9
2026-10-20 12:01:01 gpcm: Alice logged in (profile 100000001) from 203.0.113.9
2026-10-20 12:01:02 qr: 203.0.113.9:40000 asks whether swsspsp is available
2026-10-20 12:01:03 sb 203.0.113.9:40001: swsspsp list, 1 of 1 hosted games (filter: gamever = 10901)
2026-10-20 12:01:04 natneg 0000abcd: negotiation started from 203.0.113.9:40002
2026-10-20 12:01:04 natneg 0000abcd: client 1 joined from 203.0.113.9:40002
2026-10-20 12:01:04 natneg 0000abcd: client 0 joined from 127.0.0.1:47754
2026-10-20 12:01:04 natneg 0000abcd: client 1 (203.0.113.9:40002) is told to connect to 198.51.100.20:3658
2026-10-20 12:01:04 natneg 0000abcd: client 0 (127.0.0.1:47754) is told to connect to 203.0.113.9:40002
2026-10-20 12:01:05 gstats 203.0.113.9: Alice signed in to swsspsp stats
2026-10-20 12:01:06 web 203.0.113.9: GET /motd/motd.asp -> 200, 47 bytes
"""

FAILED_VISIT = """\
2026-10-20 12:10:00 gpcm ('198.51.100.4', 50000): login refused (260: The password is incorrect.)
2026-10-20 12:10:30 gpcm ('198.51.100.4', 50001): new account Bob refused (513: The nick or e-mail address is invalid.)
2026-10-20 12:11:00 gpcm: Carol logged in (profile 100000002) from 198.51.100.5
2026-10-20 12:11:01 sb 198.51.100.5:41000: swsspsp list, 0 of 0 hosted games (filter: gamever = 10901)
2026-10-20 12:11:02 gstats 198.51.100.6: sign-in as profile 100000002 refused: not logged in to Presence from this address
2026-10-20 12:11:10 qr: heartbeat from 198.51.100.5:3658 ignored: only this machine may list games on this server
2026-10-20 12:11:40 natneg 0000beef: the partner never came (here: client 1 at 198.51.100.5:41002 (port types sent: [0, 1]))
2026-10-20 12:11:41 natneg 0000beef: client 1 reports failure (swsspsp)
2026-10-20 12:12:00 gpcm ('198.51.100.5', 50009): a message was not handled; closed
"""


def run(text, only=None):
    events, warnings = session_report.parse(text.splitlines())
    out = io.StringIO()
    session_report.report(events, warnings, only, out)
    return out.getvalue()


class SessionReportTests(unittest.TestCase):
    def test_a_complete_visit_has_every_stage_and_no_problem(self):
        report = run(GOOD_VISIT)
        self.assertIn('=== 203.0.113.9: 8 events', report)
        self.assertIn('stages: [x] login [x] game available? [x] server list [x] stats sign-in [x] web', report)
        self.assertNotIn('  ! ', report)
        self.assertNotIn('  ? ', report)
        self.assertIn('=== 127.0.0.1', report)                           # the dedicated game, listed
        self.assertIn('games listed in this log: 12:00:01', report)
        host_block = next(b for b in report.split('=== ') if b.startswith('127.0.0.1'))
        self.assertNotIn('stages:', host_block.split('\n\n')[0])         # a host is not checked against a player's stages

    def test_failures_are_named(self):
        report = run(FAILED_VISIT)
        for expected in ('a login was refused', 'an account was refused', 'the server list was empty every time',
                         'the stats sign-in was refused', 'tried to list a game and the server ignored it',
                         'NAT negotiation waited 30 seconds', 'a game reported that NAT negotiation failed',
                         'a connection was ended by the server'):
            self.assertIn(expected, report)
        self.assertIn('warnings the server logged (1)', report)
        self.assertIn('=== 198.51.100.4: 2 events', report)               # grouped by address, whatever form the log writes it in
        self.assertIn('=== 198.51.100.6: 1 events', report)

    def test_one_client_only(self):
        report = run(FAILED_VISIT, only='198.51.100.4')
        self.assertIn('198.51.100.4', report)
        self.assertNotIn('=== 198.51.100.5', report)

    def test_a_player_who_never_reached_the_stats_server_is_pointed_out(self):
        report = run("2026-10-20 12:00:00 gpcm: A logged in (profile 1) from 192.0.2.1\n")
        self.assertIn('never reached the stats server', report)

    def test_lines_that_are_not_about_a_client_are_skipped(self):
        events, _ = session_report.parse(['2026-10-20 12:00:00 Presence (gpcm) listening on 0.0.0.0:29900',
                                          'systemd: Started swss-server', 'garbage'])
        self.assertEqual(events, [])


if __name__ == '__main__':
    unittest.main()
