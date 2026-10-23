# SPDX-License-Identifier: GPL-2.0-or-later
"""What keeps a public GameSpy server up and its players apart (docs/gamespy-server-security.md):
bounded messages, filters and heartbeats, secure randomness, injection into other players' streams,
sign-ins that can't be forged, and the limits on connections, requests and what a stranger may store.

Strangers are simulated by connecting from an address in access.TUNNEL_NET (Linux: any 127.x.y.z
address is loopback); the server's own machine (127.0.0.1) is trusted and not limited, as in use."""

import asyncio
import ipaddress
import logging
import os
import shutil
import socket
import stat
import struct
import sys
import tempfile
import threading
import time
import types
import unittest
import urllib.request
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_gamespy import access, crypt, gp, gstats, kv, leaderboards, natneg, qr, sb, sqlfilter  # noqa: E402
from pspc_gamespy.server import Config, GameSpyServer  # noqa: E402
from pspc_gamespy.store import Store, StoreError  # noqa: E402
from test_gamespy import GAMENAME, _free_port, heartbeat  # noqa: E402

STRANGER = '127.64.0.7'


def stranger_can_connect():
    s = socket.socket()
    try:
        s.bind((STRANGER, 0))
        return True
    except OSError:
        return False
    finally:
        s.close()


class Conn:
    """A TCP connection that starts from a chosen local address."""

    def __init__(self, port, source='127.0.0.1'):
        self.s = socket.socket()
        self.s.settimeout(5)
        self.s.bind((source, 0))
        self.s.connect(('127.0.0.1', port))
        self.buf = b''

    def close(self):
        self.s.close()

    def send(self, data):
        self.s.sendall(data.encode('latin-1') if isinstance(data, str) else data)

    def message(self):
        while b'\\final\\' not in self.buf:
            data = self.s.recv(65536)
            if not data:
                raise ConnectionError('closed')
            self.buf += data
        msg, _, self.buf = self.buf.partition(b'\\final\\')
        return kv.parse(msg.decode('latin-1'))

    def closed_by_server(self, wait=2.0):
        """Whether the server closes the connection (what arrives meanwhile is read and dropped)."""
        self.s.settimeout(wait)
        try:
            while True:
                if not self.s.recv(65536):
                    return True
        except socket.timeout:
            return False
        except OSError:
            return True


class UnitTests(unittest.TestCase):
    def test_a_value_cannot_carry_another_message(self):
        evil = 'hi\\final\\\\bm\\4\\f\\100000007\\msg\\I have authorized your request'
        wire = kv.build(('bm', 1), ('f', 5), ('msg', evil))
        messages = list(kv.Splitter().feed(wire))
        self.assertEqual(len(messages), 1)
        self.assertEqual(messages[0]['bm'], '1')

    def test_a_message_that_never_ends_is_refused(self):
        splitter = kv.Splitter()
        with self.assertRaises(kv.TooLong):
            for _ in range(100):
                list(splitter.feed('\\a\\' + 'x' * 1000))

    def test_filters_are_bounded(self):
        started = time.monotonic()
        self.assertFalse(sqlfilter._like('a' * 500, '%a' * 30 + '%b'))          # exponential as a regular expression
        self.assertTrue(sqlfilter._like('Hello World', 'hello%w_rld'))
        self.assertFalse(sqlfilter._like('abcd', 'a%c'))
        self.assertFalse(sqlfilter.Filter('(' * 200 + 'a = 1' + ')' * 200).ok)
        self.assertFalse(sqlfilter.Filter('not ' * 200 + 'a').ok)
        self.assertFalse(sqlfilter.Filter('a = 1 ' + 'or a = 1 ' * 300).ok)
        self.assertTrue(sqlfilter.Filter("gamever = 10901 and levelsettings >= 1073741824").ok)
        self.assertLess(time.monotonic() - started, 1.0)

    def test_a_heartbeat_costs_work_in_proportion_to_its_size(self):
        started = time.monotonic()
        payload = b'k\0v\0\0' + struct.pack('>H', 65535) + b'a\0' * 30000 + b'\0'
        keys, players, teams = qr.parse_heartbeat(payload)
        self.assertEqual(keys, {'k': 'v'})
        self.assertEqual((players, teams), ([], []))
        keys, players, _ = qr.parse_heartbeat(
            b'a\0b\0\0' + struct.pack('>H', 64) + b'player_\0score_\0\0' + b''.join(b'P%d\0%d\0' % (i, i) for i in range(64)))
        self.assertEqual((len(players), players[63]['player_']), (64, 'P63'))
        self.assertLess(time.monotonic() - started, 1.0)

    def test_rate_limiter(self):
        limiter = access.RateLimiter(3, 0.001)
        self.assertEqual([limiter.allow(STRANGER) for _ in range(5)], [True, True, True, False, False])
        self.assertTrue(limiter.allow('127.0.0.1'))             # the server's own machine is never limited
        limiter.spend('203.0.113.9', 10)
        self.assertFalse(limiter.available('203.0.113.9'))

    def test_rate_limiter_forgets_quiet_keys(self):
        limiter = access.RateLimiter(2, 1000.0, max_keys=10)
        for i in range(100):
            limiter.allow('203.0.113.%d' % i)
        self.assertLessEqual(len(limiter.buckets), 10)

    def test_connection_limit(self):
        limit = access.ConnectionLimit('test', 2, 3)
        self.assertEqual([limit.enter('203.0.113.1') for _ in range(3)], [True, True, False])
        self.assertTrue(limit.enter('203.0.113.2'))
        self.assertFalse(limit.enter('203.0.113.3'))            # three in all
        limit.leave('203.0.113.1')
        self.assertTrue(limit.enter('203.0.113.3'))

    def test_the_proxys_word_is_taken_only_from_the_servers_machine(self):
        headers = {'cf-connecting-ip': '203.0.113.9', 'x-forwarded-for': '1.2.3.4, 203.0.113.9'}
        self.assertEqual(access.client_ip('127.0.0.1', headers, True), '203.0.113.9')
        self.assertEqual(access.client_ip('127.0.0.1', {'x-forwarded-for': '6.6.6.6, 203.0.113.10'}, True), '203.0.113.10')
        self.assertEqual(access.client_ip('127.0.0.1', {'cf-connecting-ip': 'nonsense'}, True), '127.0.0.1')
        self.assertEqual(access.client_ip('127.0.0.1', headers, False), '127.0.0.1')                 # no proxy configured
        self.assertEqual(access.client_ip('198.51.100.7', headers, True), '198.51.100.7')            # not from this machine

    def test_the_shortcut_for_addresses_that_are_not_loopback_changes_no_answer(self):
        """is_local and is_tunnel skip the parse for addresses outside 127/8: same answers as parsing every one."""
        def reference(ip):
            try:
                a = ipaddress.ip_address(ip)
            except ValueError:
                return False, False
            return a.is_loopback and a not in access.TUNNEL_NET, a in access.TUNNEL_NET
        cases = ['127.0.0.1', '127.0.0.2', '127.64.0.1', '127.64.255.255', '127.63.255.255', '127.65.0.0', '127.255.255.255', '128.0.0.1', '126.255.255.255',
                 '0.0.0.0', '10.0.0.1', '192.168.1.10', '198.51.100.20', '203.0.113.9', '::1', '::', '::ffff:127.0.0.1', '::ffff:127.64.0.1',
                 '::ffff:203.0.113.9', 'fe80::1', '2606:4700::1111', '', ' ', 'all', '127', '127.', '127.0.0', '127.0.0.1 ', ' 127.0.0.1', '0127.0.0.1',
                 '127.0.0.1.5', '127.0.0.256', 'localhost', '1270.0.0.1', '1.127.0.0.1', '\x00', '127.0.0.1\n', 'ＬＯＣＡＬ']
        rnd = __import__('random').Random(5)
        cases += ['%d.%d.%d.%d' % tuple(rnd.randrange(256) for _ in range(4)) for _ in range(500)]
        cases += ['127.%d.%d.%d' % tuple(rnd.randrange(256) for _ in range(3)) for _ in range(500)]
        for ip in cases:
            self.assertEqual((access.is_local(ip), access.is_tunnel(ip)), reference(ip), repr(ip))

    def test_who_is_local(self):
        self.assertTrue(access.is_local('127.0.0.1'))
        self.assertFalse(access.is_local(STRANGER))
        self.assertTrue(access.is_tunnel(STRANGER))
        self.assertFalse(access.is_local('203.0.113.5'))
        self.assertEqual(access.safe('a\r\nb\x1b[31m'), 'a\\r\\nb\\x1b[31m')

    def test_reach_gives_remote_players_the_public_address(self):
        reach = qr.Reach('127.77.0.1')
        self.assertEqual(reach.ip_for('127.0.0.1', STRANGER), '127.77.0.1')        # a tunnelled player
        self.assertEqual(reach.ip_for('127.0.0.1', '203.0.113.5'), '127.77.0.1')   # on the internet
        self.assertEqual(reach.ip_for('127.0.0.1', '127.0.0.1'), '127.0.0.1')      # on the server's machine
        self.assertEqual(reach.ip_for('203.0.113.6', STRANGER), '203.0.113.6')
        self.assertEqual(qr.Reach('127.0.0.1').ip_for('127.0.0.1', STRANGER), '127.0.0.1')

    def test_database_is_private_and_bounded(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        path = os.path.join(tmp, 'db.sqlite')
        store = Store(path)
        self.addCleanup(store.close)
        if os.name == 'posix':
            self.assertEqual(stat.S_IMODE(os.stat(path).st_mode), 0o600)
        userid, a = store.new_user('a@example.com', 'x', 'A')
        _, b = store.new_user('b@example.com', 'x', 'B')
        for i in range(250):
            store.queue_message(a, b, 1, 'm%d' % i)
        messages = store.take_messages(b)
        self.assertEqual((len(messages), messages[-1]['text']), (200, 'm249'))       # the newest are kept
        for i in range(100):
            store.add_buddy_request(a, 1000 + i, '')
        with self.assertRaises(StoreError):
            store.add_buddy_request(a, 9999, '')
        store.add_buddy_request(a, 1000, 'again')                                    # an existing one may change
        for i in range(32):
            store.set_persist(a, 1, i, b'x')
        with self.assertRaises(StoreError):
            store.set_persist(a, 1, 99, b'x')
        store.set_persist(a, 1, 0, b'y')
        for i in range(15):
            store.new_profile(userid, 'P%d' % i)
        with self.assertRaises(StoreError):
            store.new_profile(userid, 'Too many')

    def test_a_database_in_memory_leaves_no_file_behind(self):
        here = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, here, True)
        old = os.getcwd()
        os.chdir(here)
        self.addCleanup(os.chdir, old)
        for name in (':memory:', ''):
            Store(name).close()
        self.assertEqual(os.listdir(here), [])

    def test_old_rows_are_pruned(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        store = Store(os.path.join(tmp, 'db.sqlite'))
        self.addCleanup(store.close)
        old = store.new_game(GAMENAME)
        recent = store.new_game(GAMENAME)
        store.x('UPDATE games SET started = ? WHERE game_id = ?', (int(time.time()) - 100 * 86400, old))
        self.assertEqual(store.prune(), 1)
        self.assertEqual([r['game_id'] for r in store.q('SELECT game_id FROM games')], [recent])

    def test_text_from_strangers_cannot_write_lines_into_the_log(self):
        server = natneg.NatNegServer(qr.Reach())
        server.connection_made(type('T', (), {'sendto': lambda self, packet, addr: None})())
        evil = b'swbfespsp\nFAKE LOG LINE: admin logged in\x1b[31m'
        report = natneg.MAGIC + bytes([3, natneg.REPORT]) + struct.pack('>I', 7) + bytes(11) + evil + b'\0'
        with self.assertLogs('gamespy', level='INFO') as logs:
            server.datagram_received(report, ('198.51.100.1', 5000))
        text = '\n'.join(logs.output)
        self.assertNotIn('\nFAKE LOG LINE', text)
        self.assertIn('FAKE LOG LINE', text)                                # said, on the same line, escaped
        self.assertNotIn('\x1b', text)

    # ---- a heartbeat the server will not list must cost next to nothing (a regression of the "ignored" log line)

    @staticmethod
    def hostile_heartbeat(gamename='swsspsp', size=60000):
        """The most expensive heartbeat to parse in full: 96 columns and 128 rows, padded to `size` bytes."""
        body = bytes([qr.HEARTBEAT]) + b'abcd' + b'gamename\0' + gamename.encode() + b'\0\0'
        body += struct.pack('>H', 128) + b''.join(b'n%d\0' % i for i in range(96)) + b'\0' + b'v\0' * 128 * 96
        return (body + bytes(size))[:size]

    @staticmethod
    def master_that_drops_everyone():
        return qr.MasterServer(qr.Registry(access.Limits()), can_host=lambda ip: access.is_local(ip))

    def test_a_dropped_heartbeat_is_not_parsed(self):
        master = self.master_that_drops_everyone()
        packet = self.hostile_heartbeat()
        with mock.patch.object(qr, 'parse_heartbeat', wraps=qr.parse_heartbeat) as parse:
            for i in range(50):
                master.datagram_received(packet, ('198.51.100.%d' % i, 5000))     # a new source each time: nothing rate limits it
        self.assertEqual(parse.call_count, 0)

    def test_dropped_heartbeats_from_spoofed_sources_are_cheap(self):
        master = self.master_that_drops_everyone()
        packet = self.hostile_heartbeat()
        started = time.perf_counter()
        for i in range(400):
            master.datagram_received(packet, ('198.%d.%d.%d' % (i >> 16, (i >> 8) & 255, i & 255), 5000))
        per_packet = (time.perf_counter() - started) / 400
        self.assertLess(per_packet, 0.002, '%.2f ms a packet: 11 ms was the regression, 0.02 ms the fix' % (per_packet * 1000))

    def test_a_flood_of_dropped_heartbeats_cannot_flood_the_log(self):
        master = self.master_that_drops_everyone()
        packet = self.hostile_heartbeat(size=400)
        with self.assertLogs('gamespy', level='INFO') as logs:
            for i in range(3000):                                               # 3,000 different spoofed sources, none seen before
                master.datagram_received(packet, ('203.%d.%d.%d' % (i >> 16, (i >> 8) & 255, i & 255), 5000))
        lines = [l for l in logs.output if 'ignored' in l]
        self.assertLessEqual(len(lines), 12, '%d lines for 3,000 packets' % len(lines))
        self.assertGreaterEqual(len(lines), 1)
        self.assertEqual(master.not_listed.count, 3000)                           # counted, though not each one said

    def test_a_player_who_tries_to_host_is_told_once_in_the_log(self):
        master = self.master_that_drops_everyone()
        packet = self.hostile_heartbeat(size=400)
        with self.assertLogs('gamespy', level='INFO') as logs:
            for _ in range(20):
                master.datagram_received(packet, ('198.51.100.7', 3658))
        lines = [l for l in logs.output if 'ignored' in l]
        self.assertEqual(len(lines), 1)
        self.assertIn('198.51.100.7:3658', lines[0])

    def test_the_game_of_a_dropped_heartbeat_is_read_without_parsing_it(self):
        read = qr.heartbeat_game
        self.assertEqual(read(b'gamename\0swsspsp\0hostname\0x\0\0'), 'swsspsp')
        self.assertEqual(read(b'hostname\0x\0mapname\0y\0gamename\0swbfespsp\0\0'), 'swbfespsp')      # not the first key
        self.assertEqual(read(b'gamename\0first\0gamename\0second\0\0'), 'first')                    # the first wins, as in parse_heartbeat
        self.assertEqual(read(b'hostname\0gamename\0\0'), '')                                          # a value that is called gamename is not the key
        self.assertEqual(read(b'gamename\0swsspsp'), '')                                                # cut off: no end of value
        self.assertEqual(read(b'\0gamename\0swsspsp\0\0'), '')                                          # the keys end at an empty name
        self.assertEqual(read(b''), '')
        self.assertEqual(read(bytes(5000)), '')
        far = b''.join(b'k%d\0v\0' % i for i in range(300)) + b'gamename\0swsspsp\0\0'
        self.assertEqual(read(far), '')                                                                   # beyond what is looked at
        for junk in (b'\xff' * 3000, b'gamename\0' * 400, os.urandom(2000)):
            self.assertIsInstance(read(junk), str)

    def test_the_game_is_read_the_same_way_by_the_parser_and_the_shortcut(self):
        for keys in ({'gamename': 'swsspsp', 'hostname': 'a'}, {'hostname': 'a', 'gamename': 'swbfespsp', 'mapname': 'b'}, {'hostname': 'a'}):
            payload = heartbeat(b'abcd', keys)[5:]
            self.assertEqual(qr.heartbeat_game(payload), qr.parse_heartbeat(payload)[0].get('gamename', ''))

    def test_challenges_and_session_keys_do_not_come_from_the_random_module(self):
        """random is a Mersenne Twister: its output predicts what follows. What a client has to guess (challenges, session
        keys, the QR2 challenge, the login ticket that is the stats server's bearer token) comes from secrets."""
        import random
        trap = mock.Mock(side_effect=AssertionError('random used for something a client has to guess'))
        with mock.patch.multiple(random, choice=trap, randint=trap, randrange=trap, random=trap, getrandbits=trap, choices=trap):
            self.assertEqual(len(gp._challenge()), 10)
            self.assertEqual(len(gp._challenge(22)), 22)
            self.assertEqual(len(qr.make_challenge(('203.0.113.9', 5000))), 8 + 8 + 4)
            session = gstats.Session(types.SimpleNamespace(store=None, limits=access.Limits(), presence=None, on_round_end=None), None,
                                     types.SimpleNamespace(get_extra_info=lambda name: ('203.0.113.9', 5000)))
            self.assertEqual(len(session.challenge), 10)
            self.assertGreater(session.sesskey, 0)

    def test_a_connection_reports_only_so_many_games(self):
        store = Store(':memory:')
        self.addCleanup(store.close)
        session = gstats.Session(types.SimpleNamespace(store=store, limits=access.Limits(), presence=None, on_round_end=None), None,
                                 types.SimpleNamespace(get_extra_info=lambda name: ('203.0.113.9', 5000)))
        session.game = gstats.games.find(GAMENAME)
        for key in range(1, 60):
            session.on_newgame(kv.parse('\\newgame\\\\sesskey\\%d' % key), None)
        self.assertEqual(store.one('SELECT COUNT(*) FROM games')[0], gstats.MAX_GAMES)
        session.on_updgame(kv.parse('\\updgame\\\\sesskey\\999\\done\\1\\gamedata\\x'), None)       # a new one past the cap is not made either
        self.assertEqual(store.one('SELECT COUNT(*) FROM games')[0], gstats.MAX_GAMES)

    def test_the_rankings_are_one_query_until_the_stats_change(self):
        store = Store(':memory:')
        self.addCleanup(store.close)
        board = leaderboards.Leaderboard(store, GAMENAME)
        _, profileid = store.new_user('r@example.com', 'x', 'Ranked')
        with mock.patch.object(store, 'q', wraps=store.q) as q:
            for sortcol in (leaderboards.OVERALL,) * 5:
                board.ranked(sortcol)
            self.assertEqual(q.call_count, 1)                                   # five page requests, one query
            board.ranked(leaderboards.KD)
            self.assertEqual(q.call_count, 2)                                   # another column, another ranking
            store.add_stats(profileid, GAMENAME, {'games': 1, 'points': 5})     # the stats changed: the next request looks again
            board.ranked(leaderboards.OVERALL)
            self.assertEqual(q.call_count, 3)

    def test_a_nat_negotiation_is_between_two_players_and_there_are_only_so_many(self):
        limits = access.Limits()
        limits.max_natneg_sessions = 3
        server = natneg.NatNegServer(qr.Reach(), limits)
        server.connection_made(types.SimpleNamespace(sendto=lambda packet, addr: None))

        def init(cookie, index):
            return (natneg.MAGIC + bytes([3, natneg.INIT]) + struct.pack('>I', cookie) + bytes([1, index, 1]) + bytes([192, 168, 1, 5]) +
                    struct.pack('>H', 3658) + b'swsspsp\0')
        for index in (0, 1, 2, 3):                                             # four clients say they are in negotiation 7
            server.datagram_received(init(7, index), ('198.51.100.%d' % (index + 1), 4000))
        self.assertEqual(sorted(server.sessions[7].clients), [0, 1])
        for cookie in range(100, 120):                                         # and many negotiations are started
            server.datagram_received(init(cookie, 0), ('198.51.100.50', 4000 + cookie))
        self.assertEqual(len(server.sessions), 3)

    def test_a_client_that_is_not_reading_is_dropped_past_a_megabyte_of_backlog(self):
        class Transport:
            def __init__(self, size):
                self.size, self.aborted = size, False

            def get_write_buffer_size(self):
                return self.size

            def abort(self):
                self.aborted = True
        for size, dropped in ((0, False), (1 << 20, False), ((1 << 20) + 1, True)):
            transport = Transport(size)
            self.assertEqual(access.drop_if_backlogged(types.SimpleNamespace(transport=transport)), dropped, size)
            self.assertEqual(transport.aborted, dropped)
        self.assertFalse(access.drop_if_backlogged(types.SimpleNamespace(transport=None)))

    def test_natneg_says_who_joined_and_who_was_waited_for(self):
        server = natneg.NatNegServer(qr.Reach())
        server.connection_made(type('T', (), {'sendto': lambda self, packet, addr: None})())
        init = natneg.MAGIC + bytes([3, natneg.INIT]) + struct.pack('>I', 0xABCD) + bytes([1, 0, 1]) + bytes([192, 168, 1, 5]) + \
            struct.pack('>H', 3658) + b'swsspsp\0'
        with self.assertLogs('gamespy', level='INFO') as logs:
            server.datagram_received(init, ('198.51.100.4', 41000))
            server.sessions[0xABCD].created -= natneg.PARTNER_WAIT + 1
            server.tick()
        text = '\n'.join(logs.output)
        self.assertIn('client 0 joined from 198.51.100.4:41000', text)
        # its init used the negotiation socket (port type 1) but said to use the game's own (use_game_port): a state to see
        self.assertIn('the partner never came (here: client 0 at 198.51.100.4:41000 (port types sent: [1]; its address for the game is unknown))', text)

    def test_natneg_ignores_short_packets_and_does_not_amplify(self):
        sent = []

        class Transport:
            def sendto(self, packet, addr):
                sent.append(packet)

        server = natneg.NatNegServer(qr.Reach())
        server.connection_made(Transport())
        header = natneg.MAGIC + bytes([3, natneg.INIT]) + struct.pack('>I', 7)
        server.datagram_received(header + b'\x01', ('198.51.100.1', 5000))                # too short to be an init
        server.datagram_received(natneg.MAGIC + bytes([3, natneg.REPORT]) + struct.pack('>I', 7) + b'\0\0\0', ('198.51.100.1', 5000))
        self.assertEqual(len(sent), 1)
        self.assertLessEqual(len(sent[0]), 12 + 3)                                      # the answer isn't bigger than the report

    def test_the_browser_asks_for_a_bounded_number_of_keys(self):
        fields = b'\\' + b'\\'.join(b'k%d' % i for i in range(1500))
        body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0' + b'ABCDEFGH' +
                b'\0' + fields + b'\0' + struct.pack('>I', 0))
        self.assertLessEqual(len(sb.ListRequest(body).fields), sb.MAX_FIELDS)

    def test_numbers_from_clients_fit_the_database(self):
        message = kv.parse('\\getprofile\\\\profileid\\' + '9' * 40 + '\\id\\1')
        self.assertLess(abs(message.int('profileid')), 2 ** 63)
        self.assertEqual(kv.parse('\\a\\-' + '9' * 40).int('a'), 0)

    def test_a_remote_address_cannot_fill_the_list_of_hosts_that_the_servers_own_machine_needs(self):
        limits = access.Limits()
        limits.max_player_hosts = 2
        registry = qr.Registry(limits)
        registry.hosts[('203.0.113.1', 1)] = object()
        registry.hosts[('203.0.113.2', 1)] = object()
        self.assertFalse(registry.has_room(('203.0.113.3', 1)))
        self.assertTrue(registry.has_room(('127.0.0.1', 5000)))

    # ---- players may host, a few at a time

    class Wire:
        """The master server's UDP socket: what it sends is kept."""

        def __init__(self):
            self.sent = []

        def sendto(self, data, addr):
            self.sent.append((data, addr))

    @staticmethod
    def open_master(max_player_hosts=5):
        limits = access.Limits()
        limits.max_player_hosts = max_player_hosts
        master = qr.MasterServer(qr.Registry(limits), can_host=lambda ip: not access.is_tunnel(ip))
        master.connection_made(UnitTests.Wire())
        return master

    @staticmethod
    def host_a_game(master, ip, port=3658, answer=True):
        """A host's heartbeat and, unless it stays silent, its answer to the challenge; whether the game is listed."""
        addr, instance = (ip, port), b'abcd'
        master.datagram_received(heartbeat(instance, {'gamename': GAMENAME, 'hostname': 'Game at %s' % ip}, statechanged=3), addr)
        if answer and addr in master.registry.hosts:
            challenge = next(d for d, a in reversed(master.transport.sent) if a == addr and d[2] == qr.CHALLENGE)
            master.datagram_received(bytes([qr.CHALLENGE]) + instance + challenge[7:], addr)
        host = master.registry.hosts.get(addr)
        return bool(host and host.validated)

    def test_players_can_host_up_to_the_limit_and_the_servers_own_games_take_none_of_the_places(self):
        master = self.open_master(max_player_hosts=5)
        self.assertTrue(all(self.host_a_game(master, '127.0.0.1', 40000 + i) for i in range(3)))      # the dedicated games
        self.assertEqual([self.host_a_game(master, '198.51.100.%d' % i) for i in range(1, 8)],
                         [True] * 5 + [False] * 2)                                  # five players, then no room
        self.assertTrue(self.host_a_game(master, '127.0.0.1', 40010))               # the machine's own is never refused
        self.assertEqual(len(master.registry.listed()), 9)
        exiting = heartbeat(b'abcd', {'gamename': GAMENAME}, statechanged=qr.STATE_EXITING)
        master.datagram_received(exiting, ('198.51.100.2', 3658))                   # a player closes their game
        self.assertTrue(self.host_a_game(master, '198.51.100.9'))                   # and the place goes to the next
        self.assertFalse(self.host_a_game(master, '198.51.100.10'))

    def test_one_address_cannot_take_the_places_of_others(self):
        master = self.open_master(max_player_hosts=5)
        self.assertEqual([self.host_a_game(master, '198.51.100.1', 3658 + i) for i in range(4)], [True, True, False, False])
        self.assertTrue(self.host_a_game(master, '198.51.100.2'))

    def test_a_full_list_does_not_parse_the_heartbeats_it_refuses(self):
        master = self.open_master(max_player_hosts=5)
        for i in range(5):
            self.host_a_game(master, '198.51.100.%d' % i)
        packet = self.hostile_heartbeat()
        with mock.patch.object(qr, 'parse_heartbeat', wraps=qr.parse_heartbeat) as parse, \
                self.assertLogs('gamespy', level='INFO') as logs:
            for i in range(200):
                master.datagram_received(packet, ('203.0.%d.%d' % (i >> 8, i & 255), 5000))
        self.assertEqual(parse.call_count, 0)
        said = [l for l in logs.output if 'not listed' in l]
        self.assertTrue(1 <= len(said) <= 12, '%d lines for 200 packets' % len(said))
        self.assertEqual(len(master.registry.hosts), 5)

    def test_forged_heartbeats_are_parsed_only_so_fast(self):
        master = self.open_master(max_player_hosts=512)
        packet = self.hostile_heartbeat(size=400)
        with mock.patch.object(qr, 'parse_heartbeat', wraps=qr.parse_heartbeat) as parse:
            for i in range(300):                                                    # 300 sources, none with a game listed
                master.datagram_received(packet, ('203.0.%d.%d' % (i >> 8, i & 255), 5000))
        self.assertLessEqual(parse.call_count, 12)                                  # the burst of ten, and what refilled meanwhile
        self.assertGreaterEqual(parse.call_count, 10)
        master.datagram_received(packet, ('127.0.0.1', 3658))                       # the server's own machine is not held to it
        self.assertIn(('127.0.0.1', 3658), master.registry.hosts)

    def test_the_games_of_the_servers_machine_are_listed_as_dedicated_and_players_are_not(self):
        master = self.open_master()
        for ip, port, level in (('127.0.0.1', 3658, '553648130'), ('127.0.0.1', 3659, ''), ('127.0.0.1', 3660, 'junk'),
                                ('127.0.0.1', 3661, str(qr.DEDICATED_LEVEL + 2)), ('198.51.100.1', 3658, '553648130')):
            master.datagram_received(heartbeat(b'abcd', {'gamename': GAMENAME, 'levelsettings': level}, statechanged=3), (ip, port))
        level = {addr: int(host.keys['levelsettings']) for addr, host in master.registry.hosts.items()}
        self.assertEqual(level[('127.0.0.1', 3658)], 553648130 | qr.DEDICATED_LEVEL)     # what the game said is kept
        self.assertEqual(level[('127.0.0.1', 3659)], qr.DEDICATED_LEVEL)
        self.assertEqual(level[('127.0.0.1', 3660)], qr.DEDICATED_LEVEL)
        self.assertEqual(level[('127.0.0.1', 3661)], qr.DEDICATED_LEVEL + 2)             # a game that says it is dedicated stays so
        self.assertEqual(level[('198.51.100.1', 3658)], 553648130)                       # a player's game is theirs to describe
        dedicated, player_hosted = sqlfilter.Filter('levelsettings >= 1073741824'), sqlfilter.Filter('levelsettings < 1073741824')
        self.assertEqual(sorted(a for a, h in master.registry.hosts.items() if dedicated.matches(h.keys)),
                         [('127.0.0.1', 3658), ('127.0.0.1', 3659), ('127.0.0.1', 3660), ('127.0.0.1', 3661)])
        self.assertEqual([a for a, h in master.registry.hosts.items() if player_hosted.matches(h.keys)], [('198.51.100.1', 3658)])

    def test_a_host_that_never_answers_gives_up_its_place(self):
        master = self.open_master(max_player_hosts=2)
        self.host_a_game(master, '198.51.100.1', answer=False)
        self.host_a_game(master, '198.51.100.2')
        self.host_a_game(master, '198.51.100.3', answer=False)                      # refused: the silent one holds the second place
        self.assertNotIn(('198.51.100.3', 3658), master.registry.hosts)
        for host in master.registry.hosts.values():
            host.born -= qr.PENDING_TIMEOUT + 1
        master.registry.expire()
        self.assertEqual(list(master.registry.hosts), [('198.51.100.2', 3658)])      # the one that answered stays
        self.assertTrue(self.host_a_game(master, '198.51.100.3'))

    def test_only_the_servers_machine_may_host_when_asked(self):
        config = Config(local_hosts_only=True, tunnel=False, limits=access.Limits(), bind='127.0.0.1', public_host='127.0.0.1')
        server = GameSpyServer(config, Store(':memory:'))
        self.addCleanup(server.store.close)
        self.assertTrue(server.master.can_host('127.0.0.1'))
        self.assertFalse(server.master.can_host('203.0.113.9'))
        self.assertFalse(server.master.can_host(STRANGER))

    def test_a_hosted_game_says_which_udp_ports_to_forward(self):
        host = qr.Host(('127.0.0.1', 40001), b'abcd')
        self.assertEqual(host.summary()['udp_ports'], [40001])              # the port the list gives players
        host.keys = {'localport': '3658', 'hostname': 'x'}
        self.assertEqual(host.summary()['udp_ports'], [3658, 40001])
        host.keys = {'localport': '40001'}
        self.assertEqual(host.summary()['udp_ports'], [40001])
        host.keys = {'localport': 'not a port'}
        self.assertEqual(host.summary()['udp_ports'], [40001])
        host.keys = {'localport': '99999'}
        self.assertEqual(host.summary()['udp_ports'], [40001])

    def test_a_server_for_the_internet_must_say_where_it_is(self):
        from pspc_gamespy import __main__ as cli
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        # It must refuse before it serves: asyncio.run is where serving starts. (Without this, a missing check would only show
        # when the ports are free, as a test that starts a real server and never ends; where they are taken it fails to bind.)
        with mock.patch.object(cli.asyncio, 'run') as serving:
            self.assertEqual(cli.main(['--db', os.path.join(tmp, 'x.sqlite'), 'serve', '--local-hosts-only']), 1)
            with mock.patch('socket.getaddrinfo', side_effect=socket.gaierror('no such name')):
                self.assertEqual(cli.main(['--db', os.path.join(tmp, 'x.sqlite'), 'serve', '--public-host', 'no.such.name']), 1)
            serving.assert_not_called()

    def test_the_limit_of_player_hosted_games_is_an_option(self):
        from pspc_gamespy import __main__ as cli
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, True)
        serve = ['--db', os.path.join(tmp, 'x.sqlite'), 'serve', '--public-host', '203.0.113.5']
        for extra, want in (([], access.Limits().max_player_hosts), (['--max-player-hosts', '5'], 5), (['--max-player-hosts', '0'], 0)):
            with mock.patch.object(cli.asyncio, 'run'), mock.patch.object(cli, 'GameSpyServer') as server:
                cli.main(serve + extra)
            self.assertEqual(server.call_args[0][0].limits.max_player_hosts, want, extra)
        with mock.patch.object(cli.asyncio, 'run'), mock.patch('sys.stderr'), self.assertRaises(SystemExit):
            cli.main(serve + ['--max-player-hosts', '-1'])

    def test_the_public_host_may_be_a_name(self):
        looked_up = []

        def getaddrinfo(host, port, family=0, type=0, *args):
            looked_up.append(host)
            if host == 'swss.example.org':
                return [(socket.AF_INET, socket.SOCK_STREAM, 6, '', ('198.51.100.7', 0))]
            raise socket.gaierror('no such name')

        with mock.patch('socket.getaddrinfo', getaddrinfo):
            reach = qr.Reach('swss.example.org')
            self.assertEqual(reach.ip_for('127.0.0.1', '203.0.113.5'), '198.51.100.7')   # what players are told: an address
            self.assertEqual(reach.ip_for('127.0.0.1', '127.0.0.1'), '127.0.0.1')
            self.assertEqual(qr.Reach('203.0.113.1').ip_for('127.0.0.1', '203.0.113.5'), '203.0.113.1')   # a literal needs no lookup
            self.assertEqual(looked_up, ['swss.example.org'])
            with self.assertRaises(OSError):
                qr.Reach('no.such.name')
            reach.public_host = 'moved.example.org'                 # its address changed, and the name can't be looked up now
            reach.refresh()
            self.assertEqual(reach.ip_for('127.0.0.1', '203.0.113.5'), '198.51.100.7')     # the last good address is kept


@unittest.skipUnless(stranger_can_connect(), 'needs 127.x.y.z addresses to connect from')
class ServerTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        logging.getLogger('gamespy').setLevel(logging.ERROR)
        cls.tmp = tempfile.mkdtemp()
        cls.store = Store(os.path.join(cls.tmp, 'gamespy.sqlite'))
        cls.limits = access.Limits()
        cls.limits.failed_logins = access.RateLimiter(3, 0.0001)
        cls.limits.new_accounts = access.RateLimiter(2, 0.0001)
        cls.limits.web = access.RateLimiter(5, 0.0001)
        cls.limits.connections['Presence'] = access.ConnectionLimit('Presence', 4, 100)
        cls.limits.hosts_per_ip = 2
        cls.async_errors = []
        asyncio_log = logging.getLogger('asyncio')
        asyncio_log.addHandler(type('H', (logging.Handler,), {'emit': lambda self, r: cls.async_errors.append(r.getMessage())})())
        cls.config = Config(public_host='127.77.0.1', bind='127.0.0.1', gpcm_port=_free_port(), gpsp_port=_free_port(),
                            qr_port=_free_port(socket.SOCK_DGRAM), natneg_port=_free_port(socket.SOCK_DGRAM),
                            sb_port=_free_port(), gstats_port=_free_port(), http_port=_free_port(), limits=cls.limits)
        cls.server = GameSpyServer(cls.config, cls.store)
        cls.loop = asyncio.new_event_loop()
        cls.thread = threading.Thread(target=cls.loop.run_forever, daemon=True)
        cls.thread.start()
        asyncio.run_coroutine_threadsafe(cls.server.start(), cls.loop).result(10)

    @classmethod
    def tearDownClass(cls):
        async def stop():
            cls.server.close()
            for s in cls.server._servers:
                await s.wait_closed()
            await asyncio.sleep(0.1)
        asyncio.run_coroutine_threadsafe(stop(), cls.loop).result(10)
        cls.loop.call_soon_threadsafe(cls.loop.stop)
        cls.thread.join(5)
        cls.loop.close()
        cls.store.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def conn(self, port, source='127.0.0.1'):
        c = Conn(port, source)
        self.addCleanup(c.close)
        return c

    def gp_login(self, nick, source='127.0.0.1', password='secret', create=True):
        """Logs in (creating the account if asked); returns (connection, reply)."""
        c = self.conn(self.config.gpcm_port, source)
        server_chal = c.server_chal = c.message()['challenge']
        email = '%s@example.com' % nick.lower()
        if create:
            c.send(kv.build(('newuser', ''), ('email', email), ('nick', nick),
                            ('passwordenc', crypt.encode_password(password)), ('namespaceid', 0), ('id', 1)))
            self.assertEqual(c.message().command, 'nur')
        user = '%s@%s' % (nick, email)
        pw = crypt.md5hex(password)
        c.send(kv.build(('login', ''), ('challenge', 'C' * 32), ('user', user),
                        ('response', crypt.login_hash(pw, user, 'C' * 32, server_chal)), ('port', 3658), ('id', 1)))
        return c, c.message()

    def on_loop(self, fn):
        async def call():
            return fn()
        return asyncio.run_coroutine_threadsafe(call(), self.loop).result(5)

    # ---- Presence

    def test_guessing_passwords_is_slowed_down(self):
        self.gp_login('Gwen', create=True)                                  # the account (from the server's own machine)
        source = '127.64.0.11'
        for _ in range(3):
            c, reply = self.gp_login('Gwen', source=source, password='wrong', create=False)
            self.assertEqual((reply.command, reply['err']), ('error', '260'))
        c, reply = self.gp_login('Gwen', source=source, create=False)       # the right password, from the same address
        self.assertEqual((reply.command, reply['err']), ('error', '5'))
        c, reply = self.gp_login('Gwen', source='127.64.0.12', create=False)  # another address isn't affected
        self.assertEqual(reply.command, 'lc')

    def test_search_manager_password_checks_count_too(self):
        self.gp_login('Gus', create=True)
        source = '127.64.0.13'
        results = []
        for password in ('a', 'b', 'c', 'd', 'secret'):
            c = self.conn(self.config.gpsp_port, source)
            c.send(kv.build(('check', ''), ('email', 'gus@example.com'), ('nick', 'Gus'), ('passenc', crypt.encode_password(password))))
            results.append(c.message()['cur'])
        self.assertEqual(results[-1], '3587')                               # refused although right: it kept guessing
        c = self.conn(self.config.gpsp_port)
        c.send(kv.build(('check', ''), ('email', 'gus@example.com'), ('nick', 'Gus'), ('passenc', crypt.encode_password('secret'))))
        self.assertEqual(c.message()['cur'], '0')

    def test_accounts_are_made_a_few_at_a_time(self):
        source = '127.64.0.14'
        replies = []
        for i in range(3):
            c = self.conn(self.config.gpcm_port, source)
            c.message()
            c.send(kv.build(('newuser', ''), ('email', 'spam%d@example.com' % i), ('nick', 'Spam%d' % i),
                            ('passwordenc', crypt.encode_password('x')), ('id', 1)))
            replies.append(c.message())
        self.assertEqual([r.command for r in replies], ['nur', 'nur', 'error'])
        self.assertEqual(replies[2]['err'], '5')

    def test_names_and_addresses_are_checked(self):
        c = self.conn(self.config.gpcm_port)
        c.message()
        for email, nick in (('a@example.com', 'x' * 31), ('a@example.com', 'bad\nnick'), ('a@example.com', 'bad\x1bnick'),
                            ('no-at-sign', 'Nick'), ('a b@example.com', 'Nick'), ('', 'Nick')):
            c.send(kv.build(('newuser', ''), ('email', email), ('nick', nick), ('passwordenc', crypt.encode_password('x')), ('id', 1)))
            self.assertEqual(c.message().command, 'error', (email, nick))

    def test_a_player_cannot_write_into_anothers_stream(self):
        mallory, m = self.gp_login('Mallory')
        victim, v = self.gp_login('Vic')
        wire = kv.build(('bm', 1), ('t', v['profileid']), ('msg', 'hi\\final\\\\bm\\4\\f\\%s\\msg\\forged' % m['profileid']), final=True)
        mallory.send(wire)
        got = victim.message()
        self.assertEqual((got['bm'], got['f']), ('1', m['profileid']))
        self.assertNotIn(b'\\final\\', victim.buf)                          # nothing more arrives: no second, forged message
        time.sleep(0.2)
        victim.s.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            victim.s.recv(100)

    def test_messages_go_to_real_profiles_who_have_not_blocked_the_sender(self):
        alice, a = self.gp_login('Alma')
        bob, b = self.gp_login('Bert')
        bob.send(kv.build(('addblock', ''), ('sesskey', b['sesskey']), ('profileid', a['profileid'])))
        time.sleep(0.2)
        alice.send(kv.build(('bm', 1), ('t', b['profileid']), ('msg', 'blocked')))
        alice.send(kv.build(('bm', 1), ('t', 424242), ('msg', 'nobody')))
        alice.send(kv.build(('bm', 1), ('t', a['profileid']), ('msg', 'to myself')))
        time.sleep(0.3)
        self.assertEqual(self.store.q('SELECT * FROM messages WHERE toid IN (?, 424242)', (int(b['profileid']),)), [])
        bob.s.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            bob.s.recv(100)

    def test_a_connection_that_never_finishes_a_message_is_closed(self):
        c = self.conn(self.config.gpcm_port, '127.64.0.15')
        c.message()
        c.s.settimeout(5)
        try:
            for _ in range(200):
                c.send(b'\\x\\' + b'y' * 1000)
        except OSError:
            pass
        self.assertTrue(c.closed_by_server())

    def test_connections_from_one_address_are_limited(self):
        source = '127.64.0.16'
        kept = []
        for _ in range(4):
            c = self.conn(self.config.gpcm_port, source)
            c.message()                                                     # the server's challenge: it let us in
            kept.append(c)
        extra = self.conn(self.config.gpcm_port, source)
        self.assertTrue(extra.closed_by_server())                           # the fifth is closed at once
        other = self.conn(self.config.gpcm_port, '127.64.0.17')
        self.assertEqual(other.message().command, 'lc')                     # other addresses are unaffected

    def test_silent_connections_are_closed(self):
        old = gp.LOGIN_TIMEOUT
        gp.LOGIN_TIMEOUT = 0.5
        self.addCleanup(setattr, gp, 'LOGIN_TIMEOUT', old)
        c = self.conn(self.config.gpcm_port, '127.64.0.18')
        c.message()
        self.assertTrue(c.closed_by_server(3))
        local = self.conn(self.config.gpcm_port)                            # the server's own machine may stay silent
        local.message()
        self.assertFalse(local.closed_by_server(1.5))

    # ---- stats

    def stats_connection(self, source):
        st = self.conn(self.config.gstats_port, source)
        rx, tx = gstats.Xor(), gstats.Xor()

        def message():
            while b'\\final\\' not in st.buf:
                st.buf += rx.apply(st.s.recv(65536))
            i = st.buf.index(b'\\final\\')
            raw, st.buf = st.buf[:i], st.buf[i + 7:]
            return gstats.parse_message(raw)

        def send(data):
            st.send(tx.apply((data.encode('latin-1') if isinstance(data, str) else data) + b'\\final\\'))

        chal = message()[0]['challenge']
        send(kv.build(('auth', ''), ('gamename', GAMENAME), ('response', gstats.auth_response(chal, 'wLfbMH')), ('id', 1), final=False))
        self.assertEqual(message()[0]['lc'], '2')
        return send, message

    def test_stats_sign_in_needs_the_player_to_be_logged_in_from_the_same_address(self):
        victim, v = self.gp_login('Vera', source='127.64.0.20')
        pid = v['profileid']
        send, message = self.stats_connection('127.64.0.21')                # someone else
        send(kv.build(('authp', ''), ('pid', pid), ('resp', 'x' * 32), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], '-1')
        send, message = self.stats_connection('127.64.0.20')                # the player's own address
        send(kv.build(('authp', ''), ('pid', pid), ('resp', 'x' * 32), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], pid)
        send(kv.build(('authp', ''), ('authtoken', 'guess'), ('lid', 1), final=False))
        self.assertEqual(message()[0]['pauthr'], '-1')                      # a login ticket that was never given
        send, message = self.stats_connection('127.64.0.22')
        send(kv.build(('authp', ''), ('authtoken', v['lt']), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], pid)                       # the login ticket works from anywhere

    def test_records_are_bounded(self):
        _, v = self.gp_login('Rhea', source='127.64.0.23')
        pid = v['profileid']
        send, message = self.stats_connection('127.64.0.23')
        send(kv.build(('authp', ''), ('pid', pid), ('resp', 'x' * 32), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], pid)
        big = b'z' * (gstats.MAX_RECORD + 1)
        send(kv.build(('setpd', ''), ('pid', pid), ('ptype', 1), ('dindex', 0), ('kv', 0), ('lid', 1), ('length', len(big)),
                      final=False).encode() + b'\\data\\' + big)
        self.assertEqual(message()[0]['setpdr'], '0')
        ok = b'z' * 100
        send(kv.build(('setpd', ''), ('pid', pid), ('ptype', 1), ('dindex', 0), ('kv', 0), ('lid', 2), ('length', len(ok)),
                      final=False).encode() + b'\\data\\' + ok)
        self.assertEqual(message()[0]['setpdr'], '1')

    # ---- hosting

    def udp(self, source):
        s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        s.bind((source, 0))
        s.settimeout(1)
        self.addCleanup(s.close)
        return s

    def register(self, udp, instance, name):
        master = ('127.0.0.1', self.config.qr_port)
        keys = {'gamename': GAMENAME, 'hostname': name, 'gamemode': 'openplaying'}
        udp.sendto(heartbeat(instance, keys, statechanged=3), master)
        try:
            challenge = udp.recv(256)
        except socket.timeout:
            return False
        udp.sendto(bytes([qr.CHALLENGE]) + instance + challenge[7:], master)
        try:
            return udp.recv(64)[2] == qr.CLIENT_REGISTERED
        except socket.timeout:
            return False

    def test_the_server_says_which_udp_port_a_hosted_game_needs_forwarded(self):
        host = self.udp('127.0.0.1')
        keys = {'gamename': GAMENAME, 'hostname': 'Ports', 'gamemode': 'openplaying', 'localport': 3658}
        master = ('127.0.0.1', self.config.qr_port)
        host.sendto(heartbeat(b'\x07\x07\x07\x07', keys, statechanged=3), master)
        challenge = host.recv(256)
        with self.assertLogs('gamespy', level='INFO') as logs:
            host.sendto(bytes([qr.CHALLENGE]) + b'\x07\x07\x07\x07' + challenge[7:], master)
            host.recv(64)
        source = host.getsockname()[1]
        expected = ', '.join(map(str, sorted({3658, source})))
        self.assertTrue(any('UDP ports players use: ' + expected in line for line in logs.output), logs.output)
        status = self.on_loop(self.server.status)
        self.assertIn(sorted({3658, source}), [g['udp_ports'] for g in status['games'] if g['name'] == 'Ports'])

    def test_refusals_are_in_the_log(self):
        """A player whose login or account is refused sees only the game's error message; the log has to say why."""
        self.gp_login('Wanda', source='127.64.0.50')
        with self.assertLogs('gamespy', level='INFO') as logs:
            c, reply = self.gp_login('Wanda', source='127.64.0.51', password='wrong', create=False)
            self.assertEqual(reply['err'], '260')
            d = self.conn(self.config.gpcm_port, '127.64.0.52')
            d.message()
            d.send(kv.build(('newuser', ''), ('email', 'no-at-sign'), ('nick', 'Bad'), ('passwordenc', crypt.encode_password('x')), ('id', 1)))
            self.assertEqual(d.message().command, 'error')
            time.sleep(0.2)
        text = '\n'.join(logs.output)
        self.assertIn("127.64.0.51", text)
        self.assertIn('login refused (260: The password is incorrect.)', text)
        self.assertIn('new account Bad refused (513', text)

    def test_a_heartbeat_from_an_address_that_may_not_host_is_logged_once(self):
        udp = self.udp(STRANGER)
        keys = {'gamename': GAMENAME, 'hostname': 'Not allowed', 'gamemode': 'openplaying'}
        with self.assertLogs('gamespy', level='INFO') as logs:
            for _ in range(3):
                udp.sendto(heartbeat(b'\x09\x09\x09\x09', keys, statechanged=3), ('127.0.0.1', self.config.qr_port))
            time.sleep(0.4)
        ignored = [l for l in logs.output if 'ignored: only this machine may list games' in l]
        self.assertEqual(len(ignored), 1, logs.output)
        # noise for a game this server doesn't serve is not worth a line
        with self.assertNoLogs('gamespy', level='INFO'):
            udp.sendto(heartbeat(b'\x08\x08\x08\x08', dict(keys, gamename='nosuchgame'), statechanged=3), ('127.0.0.1', self.config.qr_port))
            time.sleep(0.3)

    def test_text_from_clients_never_puts_control_characters_in_the_log(self):
        """Players choose names, filters, game names, paths and host names; a line break in one would let them write log
        lines of their own, and an escape sequence would reach the administrator's terminal."""
        evil = 'EVIL\nFAKE 2026-01-01 00:00:00 gpcm: admin logged in\r\x1b[31m\x00\x07'
        source = iter('127.64.0.%d' % i for i in range(70, 120))

        def tcp(port):
            c = self.conn(port, next(source))
            return c
        with self.assertLogs('gamespy', level='INFO') as logs:
            c = tcp(self.config.gpcm_port)                                       # Presence: an account, an unknown command
            c.message()
            c.send(kv.build(('newuser', ''), ('email', evil + '@x.org'), ('nick', evil), ('passwordenc', 'abc'), ('id', 1)))
            c.message()
            c = tcp(self.config.gpcm_port)
            c.message()
            c.send('\\' + evil.replace('\\', '') + '\\1\\final\\')
            c = tcp(self.config.gpsp_port)                                       # the search manager
            c.send('\\' + evil.replace('\\', '') + '\\1\\final\\')
            c = tcp(self.config.sb_port)                                         # the server browser: a game name, then a filter
            body = bytes([1, 3]) + struct.pack('>I', 0) + evil.encode() + b'\0' + evil.encode() + b'\0ABCDEFGH\0\\a\0' + struct.pack('>I', 0)
            c.send(struct.pack('>H', len(body) + 3) + b'\0' + body)
            c = tcp(self.config.sb_port)
            body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0ABCDEFGH' +
                    (evil + "' or ((").encode() + b'\0\\a\0' + struct.pack('>I', 0))
            c.send(struct.pack('>H', len(body) + 3) + b'\0' + body)
            st = tcp(self.config.gstats_port)                                    # the stats server: a game it does not know
            tx = gstats.Xor()
            st.s.recv(4096)
            st.send(tx.apply(kv.build(('auth', ''), ('gamename', evil), ('response', evil), ('id', 1), final=False).encode() + b'\\final\\'))
            udp = self.udp('127.0.0.1')                                          # the master server: "available?" and a heartbeat
            udp.sendto(bytes([qr.AVAILABLE]) + b'abcd' + evil.encode() + b'\0', ('127.0.0.1', self.config.qr_port))
            udp.sendto(heartbeat(b'abcd', {'gamename': evil, 'hostname': evil}, statechanged=3), ('127.0.0.1', self.config.qr_port))
            for method, target, host in (('GET', '/' + evil.replace('\n', '%0A').replace('\r', '%0D').replace('\x1b', '%1B').replace(' ', '%20'), 'x'),
                                         ('G\x1bET', '/nothing', evil.replace('\n', ' ').replace('\r', ' '))):      # the web server
                c = tcp(self.config.http_port)
                c.send('%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n' % (method, target, host))
                c.closed_by_server()
            time.sleep(0.6)
        messages = [record.getMessage() for record in logs.records]
        said = [m for m in messages if 'EVIL' in m]
        self.assertGreaterEqual(len(said), 6, messages)                          # the hostile text did reach the log calls
        for m in messages:
            for char in '\n\r\x1b\x00\x07':
                self.assertNotIn(char, m, repr(m))

    def search_request(self, source, *pairs):
        c = self.conn(self.config.gpsp_port, source)
        c.send(kv.build(*pairs))
        return c.message()

    def test_a_refused_account_says_why_in_the_log(self):
        """A game that shows 'Account Creation Failed' with no reason leaves the server's log as the only place the reason can be."""
        self.gp_login('Ivy', create=True)
        mine = [('passenc', crypt.encode_password('x')), ('productid', 11878), ('gamename', 'swsspsp'), ('namespaceid', 1), ('id', 1)]
        with self.assertLogs('gamespy', level='INFO') as logs:
            invalid = self.search_request('127.64.0.21', ('newuser', ''), ('email', 'no-at-sign'), ('nick', 'Ivy2'), *mine)
            taken = self.search_request('127.64.0.22', ('newuser', ''), ('email', 'ivy@example.com'), ('nick', 'Ivy'), *mine)
            made = self.search_request('127.64.0.23', ('newuser', ''), ('email', 'ivy3@example.com'), ('nick', 'Ivy3'), *mine)
            self.search_request('127.64.0.24', ('valid', ''), ('email', 'ivy@example.com'), ('partnerid', 0))
        lines = [r.getMessage() for r in logs.records]
        self.assertNotEqual(invalid['nur'], '0')
        self.assertNotEqual(taken['nur'], '0')
        self.assertEqual(made['nur'], '0')
        self.assertTrue(any('new account Ivy2 refused (%s: ' % invalid['nur'] in m and '127.64.0.21' in m for m in lines), lines)
        self.assertTrue(any('new account Ivy refused (%s: ' % taken['nur'] in m and '127.64.0.22' in m for m in lines), lines)
        self.assertTrue(any('new account Ivy3' in m and 'refused' not in m for m in lines), lines)
        self.assertTrue(any('valid answered vr=' in m and '127.64.0.24' in m for m in lines), lines)
        self.assertFalse(any('ivy@example.com' in m for m in lines), lines)       # what a search answers is not for the log

    def test_a_connection_that_says_nothing_is_logged_but_rarely(self):
        """A game that connects, reads the greeting and goes away (or sends what can't be read) is otherwise invisible."""
        with self.assertLogs('gamespy', level='INFO') as logs:
            self.conn(self.config.gpcm_port, '127.0.0.1').close()                # this machine's own: not worth a line
            for _ in range(4):                                                   # one address, over and over: one line
                c = self.conn(self.config.gpcm_port, '127.64.0.31')
                c.message()
                c.close()
                time.sleep(0.15)
            c = self.conn(self.config.gpcm_port, '127.64.0.32')                  # something that is not a message
            c.send(b'\x00\x01\x02 not a message')
            c.message()
            c.close()
            self.conn(self.config.gpsp_port, '127.64.0.33').close()              # the same for the search manager
            c = self.conn(self.config.gpsp_port, '127.64.0.34')                  # a request that never ends: its keys, not its values
            c.send('\\newuser\\\\email\\someone@example.com\\nick\\Someone\\passwordenc\\hunter2hunter2')
            c.close()
            time.sleep(0.4)
        lines = [r.getMessage() for r in logs.records if 'without a complete message' in r.getMessage()]
        unfinished = [m for m in lines if '127.64.0.34' in m]
        self.assertEqual(len(unfinished), 1, lines)
        self.assertIn('keys newuser, email, nick, passwordenc', unfinished[0])
        for secret in ('someone', 'Someone', 'hunter2'):
            self.assertNotIn(secret, unfinished[0])
        self.assertEqual(len([m for m in lines if '127.64.0.31' in m]), 1, lines)
        self.assertEqual(len([m for m in lines if '127.64.0.33' in m and m.startswith('gpsp')]), 1, lines)
        self.assertFalse(any("'127.0.0.1'" in m for m in lines), lines)

    def test_rare_log_counts_everything_and_allows_little(self):
        rare = access.RareLog(per_address_seconds=3600, burst=3, per_second=0.0001)
        said = [rare.allow('10.0.0.%d' % (i % 50)) for i in range(1000)]
        self.assertEqual(rare.count, 1000)
        self.assertEqual(said.count(True), 3)                                    # the burst, whoever asks
        again = access.RareLog(per_address_seconds=3600, burst=100, per_second=0.0001)
        self.assertEqual([again.allow('10.0.0.1') for _ in range(5)], [True, False, False, False, False])   # one address: once

    def test_who_may_host(self):
        self.assertFalse(self.register(self.udp(STRANGER), b'\x01\x01\x01\x01', 'Tunnelled'))   # tunnelled players can't
        self.assertTrue(self.register(self.udp('127.0.0.1'), b'\x02\x02\x02\x02', 'Dedicated'))  # the server's machine can
        self.assertIn('Dedicated', [h.name() for h in self.server.registry.listed()])
        self.assertNotIn('Tunnelled', [h.name() for h in self.server.registry.listed()])

    def test_hosts_from_one_address_are_limited(self):
        registry = self.server.registry
        before = len(registry.hosts)
        for i in range(2):
            registry.hosts[('203.0.113.50', 4000 + i)] = object()
        self.addCleanup(lambda: [registry.hosts.pop(('203.0.113.50', 4000 + i), None) for i in range(2)])
        self.assertFalse(registry.has_room(('203.0.113.50', 4002)))
        self.assertTrue(registry.has_room(('203.0.113.51', 4000)))
        self.assertTrue(registry.has_room(('127.0.0.1', 4000)))             # the server's own machine runs as many as it likes
        self.assertEqual(len(registry.hosts), before + 2)

    def test_messages_to_hosts_need_a_list_request_and_are_small(self):
        host = self.udp('127.0.0.1')
        self.assertTrue(self.register(host, b'\x03\x03\x03\x03', 'Message target'))
        port = host.getsockname()[1]
        target = socket.inet_aton('127.0.0.1') + struct.pack('>H', port)
        sb_conn = self.conn(self.config.sb_port, '127.64.0.24')
        sb_conn.send(struct.pack('>H', 9 + 5) + b'\x02' + target + b'hello')           # before asking for a list
        host.settimeout(0.5)
        with self.assertRaises(socket.timeout):
            host.recv(512)
        body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0' + b'ABCDEFGH' + b'\0\0' +
                struct.pack('>I', 0))
        sb_conn.send(struct.pack('>H', len(body) + 3) + b'\0' + body)
        sb_conn.s.settimeout(2)
        sb_conn.s.recv(65536)
        big = b'x' * (sb.MAX_MESSAGE + 1)
        sb_conn.send(struct.pack('>H', 9 + len(big)) + b'\x02' + target + big)
        with self.assertRaises(socket.timeout):
            host.recv(512)
        sb_conn.send(struct.pack('>H', 9 + 5) + b'\x02' + target + b'hello')
        host.settimeout(2)
        self.assertEqual(host.recv(512)[11:], b'hello')

    # ---- web

    def test_web_requests_are_limited(self):
        base = 'http://127.0.0.1:%d/motd/motd.asp' % self.config.http_port
        self.assertEqual(urllib.request.urlopen(base, timeout=5).status, 200)   # the server's own machine isn't limited
        c = self.conn(self.config.http_port, '127.64.0.25')
        statuses = []
        for _ in range(7):
            c.send('GET /motd/motd.asp HTTP/1.1\r\nHost: x\r\nConnection: keep-alive\r\n\r\n')
            data = c.s.recv(65536)
            statuses.append(int(data.split(b' ', 2)[1]))
            if b'Connection: close' in data:
                break
        self.assertEqual(statuses[:5], [200] * 5)
        self.assertEqual(statuses[5], 429)

    def test_a_chunked_request_gets_no_answer(self):
        """With Transfer-Encoding the length of the body is not Content-Length's, and a proxy and this server would disagree
        about where the request ends: the next bytes would be read as a request of their own. It is refused, unanswered."""
        c = self.conn(self.config.http_port, '127.64.0.60')
        c.send('POST /motd/motd.asp HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n'
               'GET /motd/motd.asp HTTP/1.1\r\nHost: smuggled\r\n\r\n')
        received = b''
        c.s.settimeout(3)
        try:
            while True:
                data = c.s.recv(4096)
                if not data:
                    break
                received += data
        except socket.timeout:
            self.fail('the connection was left open')
        self.assertEqual(received, b'')

    def test_oversized_web_requests_are_dropped(self):
        for request in (b'GET /' + b'a' * 10000 + b' HTTP/1.1\r\n\r\n',
                        b'GET / HTTP/1.1\r\n' + b''.join(b'X-%d: y\r\n' % i for i in range(200)) + b'\r\n',
                        b'POST / HTTP/1.1\r\nContent-Length: 99999999\r\n\r\n',
                        b'POST / HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n'):
            c = self.conn(self.config.http_port)
            c.send(request)
            self.assertTrue(c.closed_by_server(), request[:30])

    # ---- found in the second review

    def test_huge_numbers_do_not_break_a_connection(self):
        c, reply = self.gp_login('Hugo', source='127.64.0.40')
        huge = '9' * 60
        c.send(kv.build(('getprofile', ''), ('profileid', huge), ('id', 1)))
        c.send(kv.build(('addbuddy', ''), ('newprofileid', huge), ('id', 2)))
        c.send(kv.build(('bm', 1), ('t', huge), ('msg', 'x')))
        c.send(kv.build(('ka', '')))
        seen = []
        while True:
            m = c.message()
            seen.append(m.command)
            if m.command == 'ka':
                break
        self.assertIn('error', seen)                                        # the profile that doesn't exist, answered
        send, message = self.stats_connection('127.64.0.40')
        send(kv.build(('authp', ''), ('pid', reply['profileid']), ('resp', 'x' * 32), ('lid', 0), final=False))
        self.assertEqual(message()[0]['pauthr'], reply['profileid'])
        send(kv.build(('getpd', ''), ('pid', huge), ('ptype', 2), ('dindex', huge), ('lid', 7), final=False))
        m, _ = message()
        self.assertEqual((m.command, m['lid']), ('getpdr', '7'))
        send(kv.build(('setpd', ''), ('pid', reply['profileid']), ('ptype', 1), ('dindex', huge), ('kv', 0), ('lid', 8),
                      ('length', 1), final=False).encode() + b'\\data\\x')
        self.assertEqual(message()[0]['lid'], '8')
        self.assertEqual(self.async_errors, [])

    def test_a_player_cannot_flood_presence(self):
        c, reply = self.gp_login('Flo', source='127.64.0.41')
        try:
            for _ in range(400):
                c.send(kv.build(('bm', 1), ('t', 1), ('msg', 'x')))
        except OSError:
            pass
        self.assertTrue(c.closed_by_server())

    def test_one_connection_logs_in_once(self):
        self.gp_login('Bianca', source='127.64.0.44')                       # a second account to log in to
        c, reply = self.gp_login('Lena', source='127.64.0.42')
        user = 'Bianca@bianca@example.com'
        c.send(kv.build(('login', ''), ('challenge', 'C' * 32), ('user', user),
                        ('response', crypt.login_hash(crypt.md5hex('secret'), user, 'C' * 32, c.server_chal)), ('id', 1)))
        self.assertEqual(c.message().command, 'error')                      # right password, but this connection is Lena's
        self.assertTrue(c.closed_by_server())
        time.sleep(0.2)
        online = self.on_loop(lambda: [s.profile['nick'] for s in self.server.presence.online()])
        self.assertNotIn('Lena', online)                                    # not left "online" on a connection that is gone

    def test_the_browser_is_asked_for_lists_at_a_human_pace(self):
        sb_conn = self.conn(self.config.sb_port, '127.64.0.43')
        body = (bytes([1, 3]) + struct.pack('>I', 0) + GAMENAME.encode() + b'\0' + GAMENAME.encode() + b'\0' + b'ABCDEFGH' + b'\0\\hostname\0' +
                struct.pack('>I', 0))
        request = struct.pack('>H', len(body) + 3) + b'\0' + body
        try:
            for _ in range(200):
                sb_conn.send(request)
        except OSError:
            pass
        self.assertTrue(sb_conn.closed_by_server())


if __name__ == '__main__':
    unittest.main()
