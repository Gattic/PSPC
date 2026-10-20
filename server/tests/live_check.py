# SPDX-License-Identifier: GPL-2.0-or-later
"""Drives a running GameSpy server the way a game does and reports what works: an account created and
logged in, a game hosted and found in the server list, the stats server's greeting and the web pages.

    python server/tests/live_check.py [--host 127.0.0.1] [--http-port 80] [--game swsspsp] [--games FILE]
                                      [--web-base URL] [--no-host] [--dns-only] [--here]

It creates one account (SmokeNNNNN) and, unless --no-host, one hosted game on the server it is pointed
at, so use it on a server you run. The hosted game goes away by itself a few minutes after the check.

A host name is checked first (`preflight`): it must lead to an address that is not private and is not
Cloudflare's (a proxied record: Cloudflare carries web pages only, so nothing else would connect), and with
--here (this machine is the server) to this network's public address. --dns-only stops after that.

Through the tunnel (relay.py listening at 127.77.0.1): --host 127.77.0.1 --no-host, since players can't
host through it, and --web-base https://swss.pspc.online for the web pages (the relay can't listen on
port 80 without privileges, and the pages are there anyway)."""

import argparse
import ipaddress
import os
import socket
import struct
import sys
import time
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_gamespy import crypt, games, kv, qr  # noqa: E402


class Check:
    def __init__(self):
        self.failed = 0

    def __call__(self, name, ok, detail=''):
        self.failed += 0 if ok else 1
        print('%-44s %s %s' % (name, 'ok' if ok else 'FAILED', detail))


class Gp:
    """A \\final\\-terminated text connection (Presence, search)."""

    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), timeout=10)
        self.buf = b''

    def send(self, text):
        self.s.sendall(text.encode('latin-1'))

    def message(self):
        while b'\\final\\' not in self.buf:
            data = self.s.recv(65536)
            if not data:
                raise ConnectionError('the server closed the connection')
            self.buf += data
        msg, _, self.buf = self.buf.partition(b'\\final\\')
        return kv.parse(msg.decode('latin-1'))


def public_address():
    """This network's public IPv4 address, as the internet sees it (None if it can't be asked)."""
    try:
        request = urllib.request.Request('https://api.ipify.org', headers={'User-Agent': 'pspc-live-check'})
        return urllib.request.urlopen(request, timeout=10).read().decode().strip()
    except OSError:
        return None


def is_cloudflare(ip):
    """Whether an address answers as Cloudflare's edge: only it serves /cdn-cgi/trace (a proxied DNS record)."""
    try:
        request = urllib.request.Request('http://%s/cdn-cgi/trace' % ip, headers={'User-Agent': 'pspc-live-check'})
        return b'colo=' in urllib.request.urlopen(request, timeout=6).read()
    except urllib.error.HTTPError as e:
        return 'cloudflare' in (e.headers.get('server') or '').lower()
    except OSError:
        return False


def preflight(host, here, check):
    """What a name has to be for players to use it. Returns the addresses it leads to."""
    try:
        ipaddress.IPv4Address(host)
        return [host]
    except ValueError:
        pass
    try:
        ips = sorted({a[4][0] for a in socket.getaddrinfo(host, None, socket.AF_INET)})
    except OSError as e:
        check('DNS: %s resolves' % host, False, e)
        return []
    check('DNS: %s resolves' % host, True, ', '.join(ips))
    behind = [ip for ip in ips if is_cloudflare(ip)]
    check('DNS: the record is DNS-only, not proxied by Cloudflare', not behind,
          'Cloudflare answers for %s: set the record to "DNS only" (grey cloud) in the dashboard' % ', '.join(behind) if behind else '')
    private = [ip for ip in ips if not ipaddress.ip_address(ip).is_global]
    check('DNS: the address is a public one', not private, ', '.join(private))
    mine = public_address()
    if mine is None:
        print('    (this network\'s public address could not be looked up, so the record was not compared with it)')
    elif here:
        check('DNS: the record is this network\'s public address (%s)' % mine, mine in ips, 'it leads to %s' % ', '.join(ips))
    elif mine not in ips:
        print('    (this network\'s public address is %s, not the record\'s: expected when checking from another network)' % mine)
    return ips


def run(host, http_port, gamename, web_base=None, hosting=True, dns_only=False, here=False):
    check = Check()
    web_base = web_base or 'http://%s:%d' % (host, http_port)
    preflight(host, here, check)
    if dns_only:
        return check.failed
    game = games.find(gamename)
    if game is None:
        sys.exit('%s is not a game this server code knows' % gamename)
    nick = 'Smoke%d' % (int(time.time()) % 100000)
    email, password = 'smoke@example.com', 'secret'
    user = '%s@%s' % (nick, email)

    gp = Gp(host, 29900)
    server_challenge = gp.message()['challenge']
    gp.send(kv.build(('newuser', ''), ('email', email), ('nick', nick), ('uniquenick', nick),
                     ('passwordenc', crypt.encode_password(password)), ('productid', game.product_id),
                     ('gamename', gamename), ('namespaceid', game.namespace_id), ('id', 1)))
    check('Presence: account created', gp.message().command == 'nur')
    client_challenge = 'C' * 32
    gp.send(kv.build(('login', ''), ('challenge', client_challenge), ('user', user),
                     ('response', crypt.login_hash(crypt.md5hex(password), user, client_challenge, server_challenge)),
                     ('port', 3658), ('productid', game.product_id), ('gamename', gamename),
                     ('namespaceid', game.namespace_id), ('id', 1)))
    reply = gp.message()
    proof = crypt.login_hash(crypt.md5hex(password), user, server_challenge, client_challenge)
    check('Presence: logged in, server proof valid', reply.command == 'lc' and reply.get('proof') == proof)

    search = Gp(host, 29901)
    search.send(kv.build(('valid', ''), ('email', email)))
    check('Presence search: e-mail known', search.message().get('vr') == '1')

    udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    udp.settimeout(10)
    udp.sendto(bytes([qr.AVAILABLE]) + bytes(4) + gamename.encode() + b'\0', (host, 27900))
    check('Master server: game available', udp.recvfrom(100)[0][:3] == b'\xfe\xfd\x09')
    if hosting:
        instance = os.urandom(4)
        keys = {'gamename': gamename, 'hostname': 'Live check', 'mapname': 'Check', 'gametype': 'conquest', 'numplayers': 1,
                'maxplayers': 24, 'gamever': 10901, 'levelsettings': 0, 'natneg': 0}
        heartbeat = bytes([qr.HEARTBEAT]) + instance + b''.join(k.encode() + b'\0' + str(v).encode() + b'\0' for k, v in keys.items())
        heartbeat += b'\0' + struct.pack('>H', 1) + b'player_\0score_\0\0' + nick.encode() + b'\0' + b'5\0' + struct.pack('>H', 0) + b'\0'
        udp.sendto(heartbeat, (host, 27900))
        check('Master server: challenge sent', udp.recvfrom(200)[0][2] == qr.CHALLENGE)
        udp.sendto(bytes([qr.CHALLENGE]) + instance + b'answer\0', (host, 27900))
        check('Master server: game registered', udp.recvfrom(100)[0][2] == qr.CLIENT_REGISTERED)

    browser = socket.create_connection((host, 28910), timeout=10)
    challenge = b'abcdefgh'
    request = (bytes([0, 1, 3]) + struct.pack('>I', 10901) + gamename.encode() + b'\0' + gamename.encode() + b'\0' + challenge +
               b'gamever = 10901 and levelsettings < 1073741824\0\\hostname\\mapname\0' + struct.pack('>I', 0))
    browser.sendall(struct.pack('>H', len(request) + 2) + request)
    data = b''
    browser.settimeout(1)
    try:                                 # the server sends the whole list at once, then waits
        while True:
            chunk = browser.recv(65536)
            if not chunk:
                break
            data += chunk
    except socket.timeout:
        pass
    offset = 1 + (data[0] ^ 0xEC)
    server_challenge_length = data[offset] ^ 0xEA
    cipher = crypt.GOACipher(crypt.sb_crypt_key(challenge, game.secret_key, data[offset + 1:offset + 1 + server_challenge_length]))
    plain = cipher.decrypt(data[offset + 1 + server_challenge_length:])
    check('Server browser: list decrypts' + (', host listed' if hosting else ''),
          b'Live check' in plain if hosting else plain.endswith(b'\0\xff\xff\xff\xff'))

    stats = socket.create_connection((host, 29920), timeout=10)
    greeting = crypt.gamespy3d_xor(stats.recv(1024))
    check('Stats: greeting (XOR with GameSpy3D)', greeting.startswith(b'\\lc\\1\\challenge\\'))

    try:
        page = urllib.request.urlopen('%s/games/%s/web/bfes_score.asp?version=0&sortcol=12&col=0&col=13&offset=0&numrows=5'
                                      % (web_base, gamename), timeout=10).read()
        check('Web: leaderboard page', page.startswith(b'BFESScore'))
        motd = urllib.request.urlopen('%s/motd/motd.asp' % web_base, timeout=10).read()
        check('Web: message of the day', bool(motd))
    except OSError as e:
        check('Web: pages', False, e)
    return check.failed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--http-port', type=int, default=80)
    ap.add_argument('--game', default='swbfespsp', help='the game name to use (default: %(default)s)')
    ap.add_argument('--games', metavar='FILE', help="the server's games file, for a game that isn't built in")
    ap.add_argument('--web-base', metavar='URL', help='where the web pages are, if not http://HOST:HTTP-PORT')
    ap.add_argument('--no-host', action='store_true', help="don't host a game (players can't, through the tunnel)")
    ap.add_argument('--dns-only', action='store_true', help='only check what the host name leads to')
    ap.add_argument('--here', action='store_true', help="this machine is the server: the name must lead to this network's public address")
    args = ap.parse_args()
    if args.games:
        games.load(args.games)
    failed = run(args.host, args.http_port, args.game, args.web_base, not args.no_host, args.dns_only, args.here)
    print('all checks passed' if not failed else '%d check(s) FAILED' % failed)
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
