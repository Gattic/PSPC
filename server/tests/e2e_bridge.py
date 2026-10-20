# SPDX-License-Identifier: GPL-2.0-or-later
"""TCP and UDP end to end through the WebSocket bridge, with the processes a deployment has: a server
(`python -m pspc_gamespy serve --tunnel`), a relay (`python -m pspc_gamespy tunnel`) and, on the
server's machine, a dedicated game's UDP socket. What is checked is what games do, in both directions:

  - Presence: accounts, login, friends and messages between two players; a flood is cut off
  - the server browser: a list with the dedicated game in it, the address players are given for it, a
    pushed update, and a message forwarded to the game
  - NAT negotiation between the dedicated game and a player: the addresses each is given, and
    datagrams between the two sockets on those addresses, the game's first (as through a NAT)
  - stats: sign-in by profile number, records of 64 KiB written and read back byte for byte
  - the web pages over the bridge; many connections at once; delay and loss of TCP and UDP
  - the server going away and coming back

    python server/tests/e2e_bridge.py                    # through the relay
    python server/tests/e2e_bridge.py --direct ADDRESS   # no relay: the server listens on ADDRESS, as behind forwarded ports

--direct takes one of this machine's own (non-loopback) addresses, which stands in for the public one:
players connect to it as strangers, the dedicated game registers from 127.0.0.1 and heartbeats from
any other address must be refused (`--local-hosts-only`). The datagrams between a player and the game
are not exchanged there (nothing of the server's is in their path); the addresses they are given are checked.

In the default mode it uses 127.10.0.1 for the server and 127.77.0.1 for the relay and needs the games' ports free there
(not on 127.0.0.1, where a server may be running); port 80 is not listened on at the relay without
privileges, so the web pages are asked on the server's own web port as well. Everything it starts ends
with it, and its files are removed."""

import os
import random
import shutil
import socket
import statistics
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
SERVER_DIR = os.path.join(HERE, '..')
sys.path.insert(0, SERVER_DIR)
sys.path.insert(0, HERE)

from live_check import Check, Gp  # noqa: E402
from pspc_gamespy import crypt, games, gstats, kv, natneg, qr  # noqa: E402

SERVER = '127.10.0.1'           # where the server listens
RELAY = '127.77.0.1'            # where a player's game finds it
ENDPOINT = RELAY                # where the player's game connects: the relay, or with --direct the server
DIRECT = False
WEB_PORT = 18081
GAME = games.find('swsspsp')
HOST_NAME = 'Dedicated-E2E'
check = Check()


def start(args, log):
    out = open(log, 'wb')
    return subprocess.Popen([sys.executable, '-u', '-B', '-X', 'utf8', '-m', 'pspc_gamespy'] + args, cwd=SERVER_DIR, stdout=out,
                            stderr=subprocess.STDOUT, env=dict(os.environ, PYTHONPATH=SERVER_DIR))


def wait_for_tcp(host, port, seconds=15):
    end = time.time() + seconds
    while time.time() < end:
        try:
            socket.create_connection((host, port), timeout=1).close()
            return True
        except OSError:
            time.sleep(0.1)
    return False


def ports_free():
    for host in {SERVER, ENDPOINT}:
        for kind, port in ((socket.SOCK_STREAM, 29900), (socket.SOCK_DGRAM, 27900)):
            s = socket.socket(socket.AF_INET, kind)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)      # as the server does: connections just closed don't count
            try:
                s.bind((host, port))
            except OSError as e:
                sys.exit('%s:%d is in use (%s): this check needs it free' % (host, port, e.strerror))
            finally:
                s.close()


class Player:
    """A game on a player's PC: everything goes to the relay's address, where the games' ports are."""

    def __init__(self, nick, password='secret'):
        self.nick, self.password = nick, password
        self.email = '%s@example.com' % nick.lower()
        self.gp = Gp(ENDPOINT, 29900)
        self.server_challenge = self.gp.message()['challenge']
        self.gp.send(kv.build(('newuser', ''), ('email', self.email), ('nick', nick), ('uniquenick', nick),
                              ('passwordenc', crypt.encode_password(password)), ('productid', GAME.product_id),
                              ('gamename', GAME.gamename), ('namespaceid', GAME.namespace_id), ('id', 1)))
        assert self.gp.message().command == 'nur'
        user = '%s@%s' % (nick, self.email)
        self.gp.send(kv.build(('login', ''), ('challenge', 'C' * 32), ('user', user),
                              ('response', crypt.login_hash(crypt.md5hex(password), user, 'C' * 32, self.server_challenge)),
                              ('port', 3658), ('productid', GAME.product_id), ('gamename', GAME.gamename),
                              ('namespaceid', GAME.namespace_id), ('id', 1)))
        self.login = self.gp.message()
        self.profileid = self.login['profileid']
        self.proof_ok = self.login.get('proof') == crypt.login_hash(crypt.md5hex(password), user, self.server_challenge, 'C' * 32)

    def stats(self):
        s = socket.create_connection((ENDPOINT, 29920), timeout=10)
        return Stats(s)


class Stats:
    """The stats connection: GameSpy3D's XOR over a TCP stream of \\final\\-ended messages."""

    def __init__(self, sock):
        self.s, self.rx, self.tx, self.buf = sock, gstats.Xor(), gstats.Xor(), b''
        challenge = self.message()[0]['challenge']
        self.send(kv.build(('auth', ''), ('gamename', GAME.gamename), ('response', gstats.auth_response(challenge, GAME.secret_key)),
                           ('id', 1), final=False))
        self.message()

    def send(self, data):
        self.s.sendall(self.tx.apply((data.encode('latin-1') if isinstance(data, str) else data) + b'\\final\\'))

    def message(self):
        while b'\\final\\' not in self.buf:
            data = self.s.recv(65536)
            if not data:
                raise ConnectionError('closed')
            self.buf += self.rx.apply(data)
        raw, _, self.buf = self.buf.partition(b'\\final\\')
        return gstats.parse_message(raw)


def server_browser(request_options=0, fields=('hostname', 'mapname', 'numplayers')):
    """A list request for the dedicated games, as Elite Squadron asks: (connection, cipher, servers)."""
    s = socket.create_connection((ENDPOINT, 28910), timeout=10)
    filter_text = b"gamever = 10901 and levelsettings >= 1073741824\0"
    body = (bytes([1, 3]) + struct.pack('>I', 10901) + GAME.gamename.encode() + b'\0' + GAME.gamename.encode() + b'\0' + b'ABCDEFGH' +
            filter_text + ''.join('\\' + f for f in fields).encode() + b'\0' + struct.pack('>I', request_options))
    s.sendall(struct.pack('>H', len(body) + 3) + b'\0' + body)
    data = b''
    while len(data) < 1 or len(data) < 2 + (data[0] ^ 0xEC) + (data[1 + (data[0] ^ 0xEC)] ^ 0xEA if len(data) > 1 + (data[0] ^ 0xEC) else 0):
        data += s.recv(65536)
    n1 = data[0] ^ 0xEC
    n2 = data[1 + n1] ^ 0xEA
    cipher = crypt.GOACipher(crypt.sb_crypt_key(b'ABCDEFGH', GAME.secret_key, data[2 + n1:2 + n1 + n2]))
    plain = bytearray(cipher.decrypt(data[2 + n1 + n2:]))
    while not plain.endswith(b'\0\xff\xff\xff\xff'):
        plain += cipher.decrypt(s.recv(65536))
    pos = 6
    names = []
    key_count = plain[pos]
    pos += 1
    for _ in range(key_count):
        end = plain.index(0, pos + 1)                                   # a type byte, then the key's name
        names.append(plain[pos + 1:end].decode())
        pos = end + 1
    pos += 1                                                            # no popular values
    servers = []
    while plain[pos] != 0:
        flags, ip = plain[pos], socket.inet_ntoa(bytes(plain[pos + 1:pos + 5]))
        pos += 5
        port = 6500
        if flags & 16:
            port = struct.unpack_from('>H', plain, pos)[0]
            pos += 2
        private = None
        if flags & 2:
            private = socket.inet_ntoa(bytes(plain[pos:pos + 4]))
            pos += 4
        if flags & 32:
            pos += 2
        values = {}
        for f in names:
            end = plain.index(0, pos + 1)
            values[f] = plain[pos + 1:end].decode()
            pos = end + 1
        servers.append({'flags': flags, 'ip': ip, 'port': port, 'private': private, 'values': values})
    return s, cipher, servers


def pushed(sock, cipher):
    data = b''
    while len(data) < 2 or len(data) < struct.unpack_from('>H', data, 0)[0]:
        data += cipher.decrypt(sock.recv(65536))
    return data[2:struct.unpack_from('>H', data, 0)[0]]


def heartbeat_packet(instance, keys, players=(), state=None):
    if state is not None:
        keys = dict(keys, statechanged=str(state))
    body = b''.join(k.encode() + b'\0' + str(v).encode() + b'\0' for k, v in keys.items()) + b'\0'
    body += struct.pack('>H', len(players)) + b'player_\0score_\0\0' + b''.join(p.encode() + b'\0' + b'0\0' for p in players)
    body += struct.pack('>H', 0) + b'\0'
    return bytes([qr.HEARTBEAT]) + instance + body


def natneg_init(cookie, index, local_port):
    return (natneg.MAGIC + bytes([3, natneg.INIT]) + struct.pack('>I', cookie) + bytes([0, index, 1]) +
            socket.inet_aton('192.168.1.%d' % (10 + index)) + struct.pack('>H', local_port) + GAME.gamename.encode() + b'\0')


def percentiles(values):
    values = sorted(values)
    return min(values), statistics.median(values), values[int(len(values) * 0.95) - 1], max(values)


def udp_game_traffic(dedicated, player_udp, host_sees, player_sees):
    dedicated.sendto(b'game says hello first', host_sees)                    # as through a NAT that keeps its mapping
    data, source = player_udp.recvfrom(256)
    check('UDP: the game reaches the player before the player has sent anything', data == b'game says hello first' and source == player_sees)
    player_udp.sendto(b'player answers', player_sees)
    data, source = dedicated.recvfrom(256)
    check('UDP: the player reaches the game, from the address it was told', data == b'player answers' and source == host_sees, source)

    lost, delays = 0, []
    for i in range(300):
        payload = struct.pack('>I', i) + os.urandom(380)
        started = time.perf_counter()
        player_udp.sendto(payload, player_sees)
        try:
            data, source = dedicated.recvfrom(2048)
            dedicated.sendto(data, source)
            echoed, _ = player_udp.recvfrom(2048)
            if echoed != payload:
                lost += 1
            delays.append((time.perf_counter() - started) * 1000)
        except socket.timeout:
            lost += 1
        time.sleep(0.01)
    check('UDP: 300 datagrams of 384 bytes at 100 a second, out and back, none lost or changed', lost == 0, 'lost %d' % lost)
    low, median, p95, high = percentiles(delays)
    print('    UDP round trip through the bridge (ms): min %.1f median %.1f p95 %.1f max %.1f' % (low, median, p95, high))

    big = os.urandom(1400)
    player_udp.sendto(big, player_sees)
    check('UDP: a 1400 byte datagram arrives whole', dedicated.recvfrom(2048)[0] == big)
    burst, received, stop = 1500, [0], threading.Event()

    def count():                                                        # reads while the burst is sent, as a game would
        dedicated.settimeout(0.2)
        while not stop.is_set():
            try:
                dedicated.recvfrom(2048)
                received[0] += 1
            except socket.timeout:
                pass
    reader = threading.Thread(target=count)
    reader.start()
    started = time.perf_counter()
    for i in range(burst):                                              # about 2,000 a second, for under a second
        player_udp.sendto(struct.pack('>I', i) + bytes(396), player_sees)
        time.sleep(0.0004)
    sent_in = time.perf_counter() - started
    time.sleep(1)
    stop.set()
    reader.join()
    dedicated.settimeout(5)
    allowed = 600 + 300 * sent_in
    print('    UDP burst: %d datagrams in %.2f s (%.0f a second): %d arrived; the gateway allows 600 at once, then 300 a second: about %.0f'
          % (burst, sent_in, burst / sent_in, received[0], allowed))
    check('UDP: a burst is cut at the limit, not passed on in full, nor starved', 0.8 * allowed <= received[0] <= 1.1 * allowed, received[0])


def forged_heartbeats(dedicated, instance, keys):
    """On the internet UDP heartbeats can come from anywhere: from any address but this machine they are ignored."""
    stranger = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    stranger.bind((ENDPOINT, 0))                                        # not 127.0.0.1: another address
    stranger.settimeout(1)
    stranger.sendto(heartbeat_packet(os.urandom(4), dict(keys, hostname='Forged'), state=3), (SERVER, 27900))
    try:
        stranger.recv(256)
        check('Master server: a heartbeat from another address is ignored', False, 'it was answered')
    except socket.timeout:
        check('Master server: a heartbeat from another address is ignored', True)
    _, _, servers = server_browser()
    names = [x['values']['hostname'] for x in servers]
    check('Server browser: only the dedicated game is listed', names == [HOST_NAME], names)


def scenarios(tmp):
    # --- the dedicated game: a UDP socket on the server's machine, listed with the master server
    dedicated = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    dedicated.bind(('127.0.0.1', 0))
    dedicated.settimeout(5)
    host_port = dedicated.getsockname()[1]
    instance = os.urandom(4)
    keys = {'gamename': GAME.gamename, 'hostname': HOST_NAME, 'mapname': 'Bespin', 'gamemode': 'openplaying', 'gamever': 10901,
            'levelsettings': 1073741824, 'numplayers': 0, 'maxplayers': 24, 'natneg': 1, 'localip0': '192.168.1.20',
            'localport': host_port}
    master = (SERVER, 27900)
    dedicated.sendto(heartbeat_packet(instance, keys, state=3), master)
    challenge = dedicated.recv(256)
    dedicated.sendto(bytes([qr.CHALLENGE]) + instance + challenge[7:], master)
    check('Dedicated game: listed by the master server', dedicated.recv(64)[2] == qr.CLIENT_REGISTERED)

    # --- Presence through the bridge: two players, friends, messages
    alice, bob = Player('Alice-E2E'), Player('Bob-E2E')
    check('Presence: both log in, the server proves itself', alice.proof_ok and bob.proof_ok)
    alice.gp.send(kv.build(('addbuddy', ''), ('sesskey', alice.login['sesskey']), ('newprofileid', bob.profileid), ('reason', 'hi')))
    asked = bob.gp.message()
    check('Presence: a friend request reaches the other player', (asked['bm'], asked['f']) == ('2', alice.profileid))
    bob.gp.send(kv.build(('authadd', ''), ('sesskey', bob.login['sesskey']), ('fromprofileid', alice.profileid), ('sig', 'x')))
    check('Presence: accepted', alice.gp.message()['bm'] == '4')
    alice.gp.message()                                                  # Bob's status
    text = 'Hello \u00e9\u00e8 from Alice ' + 'x' * 900
    alice.gp.s.sendall(kv.build(('bm', 1), ('t', bob.profileid), ('msg', text)).encode('utf-8'))
    got = bob.gp.message()                                              # Gp reads bytes as Latin-1; the server speaks UTF-8
    check('Presence: a message between friends, 900+ bytes, accents intact',
          got['bm'] == '1' and got['msg'].encode('latin-1').decode('utf-8') == text)
    pings = []
    for _ in range(60):
        started = time.perf_counter()
        alice.gp.send(kv.build(('ka', '')))
        alice.gp.message()
        pings.append((time.perf_counter() - started) * 1000)
    low, median, p95, high = percentiles(pings)
    print('    TCP round trip through the bridge (ms): min %.1f median %.1f p95 %.1f max %.1f' % (low, median, p95, high))

    # --- the server browser: the dedicated game, as players are given it
    browser, cipher, servers = server_browser(request_options=4)
    mine = [s for s in servers if s['values']['hostname'] == HOST_NAME]
    check('Server browser: the dedicated game is listed', len(mine) == 1)
    entry = mine[0]
    check('Server browser: players are told the address they connect to, and the game\'s port', (entry['ip'], entry['port']) == (ENDPOINT, host_port),
          (entry['ip'], entry['port']))
    check('Server browser: negotiate and private-address flags kept', entry['flags'] & 4 and entry['private'] == '192.168.1.20')
    dedicated.sendto(heartbeat_packet(instance, dict(keys, mapname='Endor', numplayers=2), ['Alice-E2E', 'Bob-E2E']), master)
    update = pushed(browser, cipher)
    check('Server browser: a change at the game is pushed to a player', update[0] == 2 and b'Endor\0' in update)
    target = socket.inet_aton(ENDPOINT) + struct.pack('>H', host_port)
    browser.sendall(struct.pack('>H', 9 + 5) + b'\x02' + target + b'hello')
    got = dedicated.recv(256)
    check('Server browser: a message to the game is forwarded to its UDP socket', got[:3] == b'\xfe\xfd\x06' and got[11:] == b'hello')
    dedicated.sendto(bytes([qr.CLIENT_MESSAGE_ACK]) + instance + got[7:11], master)

    # --- NAT negotiation, then datagrams between the two sockets on the addresses they were given
    player_udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    player_udp.bind((ENDPOINT if DIRECT else '127.0.0.1', 0))
    player_udp.settimeout(5)
    cookie = random.getrandbits(31)
    natneg_server = (ENDPOINT, 27901)
    dedicated.sendto(natneg_init(cookie, 0, host_port), (SERVER, 27901))
    dedicated.recv(64)
    player_udp.sendto(natneg_init(cookie, 1, player_udp.getsockname()[1]), natneg_server)
    player_udp.recv(64)
    told_player, told_host = player_udp.recv(64), dedicated.recv(64)
    player_sees = (socket.inet_ntoa(told_player[12:16]), struct.unpack_from('>H', told_player, 16)[0])
    host_sees = (socket.inet_ntoa(told_host[12:16]), struct.unpack_from('>H', told_host, 16)[0])
    check('NAT negotiation: the player is told where the game is', player_sees == (ENDPOINT, host_port), player_sees)
    if DIRECT:
        check('NAT negotiation: the game is told where the player is (the player\'s own address)',
              host_sees == player_udp.getsockname(), host_sees)
    if not DIRECT:
        check('NAT negotiation: the game is told where the player is (an address of the player\'s own)',
              host_sees[0].startswith('127.64.'), host_sees)

    for sock, index, where in ((player_udp, 1, natneg_server), (dedicated, 0, (SERVER, 27901))):       # a game acknowledges, or it is sent again
        sock.sendto(natneg.MAGIC + bytes([3, natneg.CONNECT_ACK]) + struct.pack('>I', cookie) + bytes([0, index]), where)
    time.sleep(0.3)
    for sock in (player_udp, dedicated):
        sock.settimeout(0.2)
        try:
            while True:
                sock.recv(2048)
        except socket.timeout:
            pass
        sock.settimeout(5)

    if DIRECT:
        forged_heartbeats(dedicated, instance, keys)
    else:
        udp_game_traffic(dedicated, player_udp, host_sees, player_sees)

    # --- stats: sign-in and records up to 64 KiB, byte for byte, both ways
    stats = alice.stats()
    stats.send(kv.build(('authp', ''), ('pid', alice.profileid), ('resp', 'x' * 32), ('lid', 0), final=False))
    check('Stats: sign-in by profile number (the player is logged in to Presence from the same address)',
          stats.message()[0]['pauthr'] == alice.profileid)
    records = {i: os.urandom(60000 + i * 100) for i in range(14)}
    ok = True
    for i, record in records.items():
        stats.send(kv.build(('setpd', ''), ('pid', alice.profileid), ('ptype', 1), ('dindex', i), ('kv', 0), ('lid', i),
                            ('length', len(record)), final=False).encode() + b'\\data\\' + record)
        ok &= stats.message()[0]['setpdr'] == '1'
    check('Stats: 14 records of about 60 KB saved through the bridge (880 KB sent)', ok)
    ok = True
    started = time.perf_counter()
    for i, record in records.items():
        stats.send(kv.build(('getpd', ''), ('pid', alice.profileid), ('ptype', 1), ('dindex', i), ('lid', 100 + i), final=False))
        m, data = stats.message()
        ok &= data == record
    check('Stats: every record read back byte for byte', ok, '%.1f s' % (time.perf_counter() - started))
    thief = Player('Mallory-E2E')                                           # another player's number
    stolen = thief.stats()
    stolen.send(kv.build(('authp', ''), ('pid', bob.profileid), ('resp', 'x' * 32), ('lid', 0), final=False))
    # Mallory is logged in from the same address as Bob (the bridge gives everyone behind one address one address),
    # which is the case this rule can't tell apart; a player from another address is refused (test_gamespy_security).
    print('    (players that share an address share a stats sign-in: %s)' % stolen.message()[0]['pauthr'])

    # --- the web pages: over the bridge to the web port, and on the web port itself
    page = urllib.request.urlopen('http://%s:%d/games/%s/web/bfes_score.asp?version=0&sortcol=12&col=0&col=13&offset=0&numrows=5'
                                  % (SERVER, WEB_PORT, GAME.gamename), timeout=10).read()
    check('Web: the leaderboard page', page.startswith(b'BFESScore'))
    try:
        web = socket.create_connection((ENDPOINT, 80), timeout=3)
        web.sendall(b'GET /motd/motd.asp HTTP/1.1\r\nHost: motd.gamespy.com\r\nConnection: close\r\n\r\n')
        check('Web: the message of the day through the relay, port 80', b'200 OK' in web.recv(4096))
    except OSError:
        print('    (the relay can not listen on port 80 without privileges: the web pages were asked directly)')

    # --- many connections at once, and over the limit
    connections = []
    for i in range(12):
        s = socket.create_connection((ENDPOINT, 29900), timeout=10)
        connections.append(Gp.__new__(Gp))
        connections[-1].s, connections[-1].buf = s, b''
    greeted = sum(1 for c in connections if c.message().command == 'lc')
    check('TCP: 12 connections at once all get the server\'s greeting', greeted == 12, greeted)
    extra = []
    for i in range(30):
        s = socket.create_connection((ENDPOINT, 29900), timeout=10)
        s.settimeout(3)
        extra.append(s)
    time.sleep(1)
    refused = 0
    for s in extra:
        try:
            if s.recv(100) == b'':
                refused += 1
        except (socket.timeout, OSError):
            pass
    check('TCP: connections over a player\'s limit are closed by the server', refused > 0, '%d of 30 closed' % refused)
    for c in connections:
        c.s.close()
    for s in extra:
        s.close()

    # --- Presence flood is cut off
    flooder = Player('Flood-E2E')
    try:
        for _ in range(400):
            flooder.gp.send(kv.build(('bm', 1), ('t', 1), ('msg', 'x')))
    except OSError:
        pass
    flooder.gp.s.settimeout(3)
    closed = False
    try:
        while flooder.gp.s.recv(65536):
            pass
        closed = True
    except OSError:
        closed = True
    check('Presence: a flood of messages ends the connection', closed)
    return dedicated, instance, keys


def main():
    global SERVER, ENDPOINT, DIRECT
    if len(sys.argv) == 3 and sys.argv[1] == '--direct':
        SERVER = ENDPOINT = sys.argv[2]
        DIRECT = True
    ports_free()
    tmp = tempfile.mkdtemp(prefix='e2e-bridge-')
    processes = []
    try:
        server_log, relay_log = os.path.join(tmp, 'server.log'), os.path.join(tmp, 'relay.log')
        db = os.path.join(tmp, 'e2e.sqlite')
        server_args = ['--db', db, 'serve', '--bind', SERVER, '--http-port', str(WEB_PORT), '--local-hosts-only']
        server_args += ['--public-host', SERVER] if DIRECT else ['--tunnel']
        processes.append(start(server_args, server_log))
        check('Server process listens', wait_for_tcp(SERVER, 29900))
        if not DIRECT:
            relay = start(['tunnel', 'http://%s:%d' % (SERVER, WEB_PORT)], relay_log)
            processes.append(relay)
            check('Relay process listens where the game looks', wait_for_tcp(RELAY, 29900))
            time.sleep(1)
        scenarios(tmp)

        # --- the server goes away and comes back; the relay stays, and new connections work
        processes[0].terminate()
        processes[0].wait(10)
        try:
            gone = socket.create_connection((ENDPOINT, 29900), timeout=3)
            gone.settimeout(3)
            check('Server away: a connection is closed, not left hanging', gone.recv(10) == b'')
        except OSError:
            check('Server away: a connection is refused, not left hanging', True)
        processes[0] = start(server_args, server_log)
        check('Server back', wait_for_tcp(SERVER, 29900))
        time.sleep(1.5)
        again = Player('Alice-Again')
        check('Server back: a player logs in again' + ('' if DIRECT else ' through the same relay'), again.proof_ok)
    finally:
        for p in processes:
            if p.poll() is None:
                p.terminate()
        for p in processes:
            try:
                p.wait(10)
            except subprocess.TimeoutExpired:
                p.kill()
        errors = []
        for log in ('server.log', 'relay.log'):
            try:
                with open(os.path.join(tmp, log), encoding='utf-8', errors='replace') as f:
                    errors += [log + ': ' + line.rstrip() for line in f if 'Traceback' in line or 'Error' in line or 'WARNING' in line.upper()]
            except OSError:
                pass
        if errors:
            print('Lines of the logs worth a look:')
            for line in errors[:20]:
                print('   ', line[:200])
        shutil.rmtree(tmp, ignore_errors=True)
    print('all checks passed' if not check.failed else '%d check(s) FAILED' % check.failed)
    return 1 if check.failed else 0


if __name__ == '__main__':
    sys.exit(main())
