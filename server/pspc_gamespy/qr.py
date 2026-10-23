# SPDX-License-Identifier: GPL-2.0-or-later
"""The master server (Query & Reporting 2, <gamename>.master.gamespy.com, UDP 27900): games that
host a match register here and keep it up to date, and the server browser (sb.py) lists them.

Client packets are [type][instance key 4][payload]; the server's begin with FE FD, then the type
and the instance key:

- available (9): "is this game's service up" (the payload is the game name); answered FE FD 09 and
  four zero bytes (no services disabled).
- heartbeat (3): the host's keys as name/value strings up to an empty name, then a player section
  and a team section, each [count, 2 bytes big-endian][key names, up to an empty one][the values,
  count times]. statechanged=3 starts a registration, 2 ends it. A new host is sent a challenge
  (1): 6 random characters, 2 hex digits of backend flags and the address it was seen at,
  %08X%04X. Its answer (1) completes the registration, acknowledged with 10 (registered), and the
  host is listed from then on.
- keepalive (8): answered with the same type and the server's time.
- client messages: the server browser forwards players' messages to hosts (6: [message key 4]
  [data], resent until acknowledged by 7). NAT negotiation starts this way.
"""

import asyncio
import ipaddress
import itertools
import logging
import random
import secrets
import socket
import string
import struct
import time

from . import access
from . import games

log = logging.getLogger('gamespy')

QR_MAGIC = b'\xfe\xfd'
QUERY, CHALLENGE, ECHO, HEARTBEAT, ADDERROR, ECHO_RESPONSE = 0, 1, 2, 3, 4, 5
CLIENT_MESSAGE, CLIENT_MESSAGE_ACK, KEEPALIVE, AVAILABLE, CLIENT_REGISTERED = 6, 7, 8, 9, 10

STATE_CHANGED, STATE_EXITING, STATE_INITIAL = 1, 2, 3
HOST_TIMEOUT = 180          # seconds without a heartbeat or keepalive before a host is dropped
DEDICATED_LEVEL = 1 << 30   # the bit of a host's levelsettings that puts it on the game's list of dedicated servers (the list asks
                            # levelsettings >= 1073741824, the other 'player-hosted' asks < that)
PENDING_TIMEOUT = 30        # seconds a host has to answer the server's challenge before it gives up its place
RESEND_INTERVAL = 2.0
RESEND_COUNT = 5
# What a hosted game may say about itself. A 64-player game's heartbeat is a few kilobytes.
MAX_KEYS = 96               # server keys, and each player and team column
MAX_ROWS = 128              # players, teams
MAX_TEXT = 256              # a key's name or value
MAX_MESSAGE = 256           # what a player may have the server send a host
MAX_PENDING_MESSAGES = 1024 # messages to hosts waiting to be acknowledged, in all


def is_loopback(ip):
    try:
        return ipaddress.ip_address(ip).is_loopback
    except ValueError:
        return False


def resolve_ipv4(host):
    """The IPv4 address of a name (or the address itself); OSError if there is none."""
    try:
        ipaddress.IPv4Address(host)
        return host
    except ValueError:
        pass
    return socket.getaddrinfo(host, None, socket.AF_INET)[0][4][0]


class Reach:
    """Which address a player is given for another machine. Hosts and players on the server's own
    machine are seen at 127.0.0.1; players elsewhere - on the internet, or tunnelled (tunnel.py) -
    reach them at the server's public address. That is a name or an address: the lists and NAT
    negotiation carry addresses, so a name is looked up (and again every few minutes: refresh(), for
    a name whose address changes). It must be a name that leads to this server: not one that Cloudflare
    proxies, which answers with Cloudflare's addresses."""

    def __init__(self, public_host='127.0.0.1'):
        self.public_host = public_host
        self.public_ip = resolve_ipv4(public_host)      # OSError if the name has no address

    def refresh(self):
        """Looks the public host up again; an answer that fails leaves the last good address."""
        try:
            ip = resolve_ipv4(self.public_host)
        except OSError as e:
            log.warning("can't look up %s (%s); players keep being given %s", self.public_host, e, self.public_ip)
            return
        if ip != self.public_ip:
            log.info('%s is now %s (was %s)', self.public_host, ip, self.public_ip)
            self.public_ip = ip

    def ip_for(self, ip, viewer_ip):
        viewer_is_elsewhere = viewer_ip and (not is_loopback(viewer_ip) or access.is_tunnel(viewer_ip))
        if access.is_local(ip) and viewer_is_elsewhere and self.public_ip != ip:
            return self.public_ip
        return ip


def read_nts(data, pos):
    end = data.find(b'\0', pos)
    if end < 0:
        return data[pos:].decode('latin-1'), len(data)
    return data[pos:end].decode('latin-1'), end + 1


class Host:
    """A hosted game as its heartbeats describe it."""

    _ids = itertools.count(1)

    def __init__(self, addr, instance):
        self.id = next(Host._ids)
        self.addr = addr                # (ip, port) the host's heartbeats come from
        self.instance = instance        # 4 bytes, the host's QR2 instance key
        self.gamename = ''
        self.keys = {}
        self.players = []               # one dict per player: key name (e.g. "player_") -> value
        self.teams = []
        self.validated = False
        self.challenge = ''
        self.created = time.time()
        self.born = self.last_seen = time.monotonic()

    @property
    def ip(self):
        return self.addr[0]

    @property
    def port(self):
        return self.addr[1]

    def name(self):
        return self.keys.get('hostname') or '%s:%d' % self.addr

    def rules(self):
        """All of the host's keys, players' and teams' keys numbered (player_0, team_t1, ...)."""
        out = list(self.keys.items())
        for i, p in enumerate(self.players):
            out += [('%s%d' % (k, i), v) for k, v in p.items()]
        for i, t in enumerate(self.teams):
            out += [('%s%d' % (k, i), v) for k, v in t.items()]
        return out

    def summary(self):
        return {'id': self.id, 'game': self.gamename, 'address': '%s:%d' % self.addr, 'name': self.name(),
                'map': self.keys.get('mapname', ''), 'mode': self.keys.get('gamemode', ''),
                'players': self.keys.get('numplayers', str(len(self.players))), 'max': self.keys.get('maxplayers', ''),
                'player_names': [p.get('player_', '') for p in self.players], 'since': int(self.created),
                'udp_ports': self.udp_ports()}

    def udp_ports(self):
        """The UDP ports players have to reach on this game's machine, so the ones a router has to forward for a game
        hosted here: the port its heartbeats come from (the port the server list gives players) and the `localport` it
        says it has. They are the same for a game that uses one socket for both. The authority is the game's own
        process: `ss -lunp` lists every UDP port it has open."""
        ports = {self.port}
        try:
            ports.add(int(self.keys.get('localport') or 0))
        except ValueError:
            pass
        return sorted(p for p in ports if 0 < p < 65536)


def parse_heartbeat(payload):
    """(keys, players, teams) from a heartbeat's payload. What there is too much of is dropped:
    the work is proportional to the payload's size, whatever counts it claims."""
    keys = {}
    pos = 0
    while pos < len(payload) and len(keys) < MAX_KEYS:
        name, pos = read_nts(payload, pos)
        if not name:
            break
        value, pos = read_nts(payload, pos)
        keys.setdefault(name[:MAX_TEXT], value[:MAX_TEXT])
    players, teams = [], []
    while pos + 2 <= len(payload):
        count = struct.unpack_from('>H', payload, pos)[0]
        pos += 2
        names = []
        while pos < len(payload) and len(names) < MAX_KEYS:
            name, pos = read_nts(payload, pos)
            if not name:
                break
            names.append(name[:MAX_TEXT])
        if not names:
            if count == 0:
                continue
            break
        rows = []
        for _ in range(min(count, MAX_ROWS)):
            if pos >= len(payload):
                break
            row = {}
            for name in names:
                row[name], pos = read_nts(payload, pos)
                row[name] = row[name][:MAX_TEXT]
            rows.append(row)
        if count > MAX_ROWS:
            break
        if all(n.endswith('_t') for n in names):
            teams = rows
        else:
            players = rows
    return keys, players, teams


GAME_WINDOW = 512           # how much of a heartbeat is looked at to find its game name (a game's keys start with it)


def level_settings(keys):
    try:
        return int(keys.get('levelsettings') or 0)
    except ValueError:
        return 0


def heartbeat_game(payload):
    """The game name in a heartbeat's keys ('' if it is not there), read from the first GAME_WINDOW bytes without parsing
    the rest. For a heartbeat the server will not list, where parse_heartbeat's work (the cost of the whole packet, which
    anyone can send from any address) would be spent only to decide whether to say so in the log. A name and its value
    must both end with their NUL, as a real heartbeat's do; the first gamename wins, as in parse_heartbeat."""
    fields = payload[:GAME_WINDOW].split(b'\0')
    for i in range(0, len(fields) - 2, 2):              # the last field is what follows the final NUL: not complete
        if not fields[i]:
            break                                       # an empty name ends the keys
        if fields[i] == b'gamename':
            return fields[i + 1].decode('latin-1')[:MAX_TEXT]
    return ''


def make_challenge(addr):
    a, b, c, d = (int(x) for x in addr[0].split('.'))
    return (''.join(secrets.choice(string.ascii_letters + string.digits) for _ in range(6)) +
            '%x%x' % (secrets.randbelow(8), secrets.randbelow(16)) +
            '%08X%04X' % ((a << 24) | (b << 16) | (c << 8) | d, addr[1]))


class Registry:
    """The hosted games, and who wants to hear about changes (server browser connections with push
    updates): watchers have host_added(host), host_updated(host) and host_removed(host)."""

    def __init__(self, limits=None):
        self.limits = limits or access.Limits.none()
        self.hosts = {}                 # (ip, port) -> Host
        self.watchers = set()

    def has_room(self, addr):
        """Whether a new host at this address may be added: only so many games hosted by players (every address but the
        server's own machine; games that have not answered the challenge yet count) and so many from one address. The
        server's own machine hosts as many as it runs, and they take no place from the players'."""
        if not self.limits.enabled or access.is_local(addr[0]):
            return True                 # the server's own machine runs the games it was set up to run
        players = [a for a in self.hosts if not access.is_local(a[0])]
        if len(players) >= self.limits.max_player_hosts:
            return False
        return sum(1 for a in players if a[0] == addr[0]) < self.limits.hosts_per_ip

    def listed(self, gamename=None):
        want = games.find(gamename).gamename if gamename and games.find(gamename) else gamename
        out = []
        for h in self.hosts.values():
            if not h.validated:
                continue
            g = games.find(h.gamename)
            if want and (g.gamename if g else h.gamename) != want:
                continue
            out.append(h)
        return sorted(out, key=lambda h: h.id)

    def find(self, ip, port):
        h = self.hosts.get((ip, port))
        if h:
            return h
        # A player may name a host on this machine by the server's public address.
        return next((h for h in self.hosts.values() if h.port == port and is_loopback(h.ip)), None)

    def _notify(self, event, host):
        for w in list(self.watchers):
            try:
                getattr(w, event)(host)
            except Exception:
                log.exception('server list update failed')

    def listed_now(self, host):
        host.validated = True
        self._notify('host_added', host)

    def updated(self, host):
        if host.validated:
            self._notify('host_updated', host)

    def remove(self, host, why=''):
        if self.hosts.get(host.addr) is host:
            del self.hosts[host.addr]
            if host.validated:
                log.info('qr: %s (%s:%d) is no longer listed%s', access.safe(host.name()), host.ip, host.port,
                         why and ' (%s)' % why)
                self._notify('host_removed', host)

    def expire(self):
        now = time.monotonic()
        for h in list(self.hosts.values()):
            if now - h.last_seen > HOST_TIMEOUT:
                self.remove(h, 'timed out')
            elif not h.validated and now - h.born > PENDING_TIMEOUT:
                self.remove(h, 'did not answer the challenge')


class MasterServer(asyncio.DatagramProtocol):
    def __init__(self, registry, can_host=None):
        self.registry = registry
        self.limits = registry.limits
        self.can_host = can_host or (lambda ip: True)     # who may list a game: see GameSpyServer
        self.transport = None
        self.not_listed = access.RareLog()  # heartbeats for a game served here that are dropped: see _not_listed
        self._msg_keys = itertools.count(random.randrange(1, 1 << 30))
        self._pending = {}              # message key -> (host addr, packet, tries left)

    def connection_made(self, transport):
        self.transport = transport

    def send(self, packet, addr):
        if self.transport is not None:
            self.transport.sendto(packet, addr)

    def datagram_received(self, data, addr):
        if not self.limits.allow(self.limits.qr_packets, addr[0]):
            return
        try:
            self._handle(data, addr)
        except Exception:
            log.debug('qr: bad packet from %s:%d: %s', addr[0], addr[1], data[:64].hex(), exc_info=True)

    def _handle(self, data, addr):
        if len(data) < 5:
            return
        ptype, instance, payload = data[0], data[1:5], data[5:]
        host = self.registry.hosts.get(addr)
        if host is not None:
            host.last_seen = time.monotonic()
        if ptype == AVAILABLE:
            gamename, _ = read_nts(payload, 0)
            log.info('qr: %s:%d asks whether %s is available%s', addr[0], addr[1], access.safe(gamename),
                     '' if games.find(gamename) else ' (a game this server doesn\'t know)')
            self.send(QR_MAGIC + bytes([AVAILABLE]) + b'\0\0\0\0', addr)
        elif ptype == HEARTBEAT:
            self._heartbeat(addr, instance, payload)
        elif ptype == CHALLENGE:
            self._challenge_response(addr, instance, payload)
        elif ptype == KEEPALIVE:
            self.send(QR_MAGIC + bytes([KEEPALIVE]) + instance + struct.pack('>I', int(time.time()) & 0xFFFFFFFF), addr)
        elif ptype == CLIENT_MESSAGE_ACK:
            if len(payload) >= 4:
                key = struct.unpack_from('>I', payload, 0)[0]
                if self._pending.pop(key, None):
                    log.debug('qr: %s:%d got client message %d', addr[0], addr[1], key)
        elif ptype == ECHO_RESPONSE:
            pass
        else:
            log.debug('qr: packet type %d from %s:%d: %s', ptype, addr[0], addr[1], payload[:64].hex())

    def _not_listed(self, addr, payload, how, why):
        """A heartbeat that is dropped (`how`: 'ignored' or 'not listed'; `why`). Anyone can send these from any address, so
        it costs next to nothing (no parsing) and its line in the log is rare: for a game this server serves only (the
        internet sends the rest as noise), at the rate access.RareLog allows, with the number so far."""
        if games.find(heartbeat_game(payload)) and self.not_listed.allow(addr[0]):
            log.info('qr: heartbeat from %s:%d %s: %s (%d such heartbeats so far)', addr[0], addr[1], how, why, self.not_listed.count)

    def _may_register(self, addr, payload):
        """Whether a heartbeat from an address with no game listed is worth parsing: there is room for another game, and
        not too many of these are being parsed (a forged flood would otherwise cost the whole packet's parsing each)."""
        if not self.registry.has_room(addr):
            self._not_listed(addr, payload, 'not listed', 'the games players host are full (or this address has its share)')
            return False
        if not access.is_local(addr[0]) and not self.limits.allow(self.limits.new_hosts, 'all'):
            self._not_listed(addr, payload, 'not listed', 'too many new games are being registered at once')
            return False
        return True

    def _heartbeat(self, addr, instance, payload):
        if not self.can_host(addr[0]):
            self._not_listed(addr, payload, 'ignored', 'only this machine may list games on this server')
            return
        if addr not in self.registry.hosts and not self._may_register(addr, payload):
            return
        keys, players, teams = parse_heartbeat(payload)
        try:
            state = int(keys.get('statechanged') or STATE_CHANGED)
        except ValueError:
            state = STATE_CHANGED
        host = self.registry.hosts.get(addr)
        if state == STATE_EXITING:
            if host:
                self.registry.remove(host, 'the host closed it')
            return
        gamename = keys.get('gamename', host.gamename if host else '')
        if host is None or state == STATE_INITIAL or host.instance != instance:
            if host is not None:
                self.registry.remove(host, 'registered again')
            if not self.registry.has_room(addr):
                self._not_listed(addr, payload, 'not listed', 'the games players host are full (or this address has its share)')
                return
            host = Host(addr, instance)
            self.registry.hosts[addr] = host
        host.gamename = gamename or host.gamename
        host.keys = keys
        if access.is_local(addr[0]):
            keys['levelsettings'] = str(level_settings(keys) | DEDICATED_LEVEL)     # the games of the server's own machine are its dedicated servers
        host.players = players
        host.teams = teams
        if not games.find(host.gamename):
            log.warning('qr: heartbeat from %s:%d for %s, a game this server has no key for', addr[0], addr[1],
                        access.safe(host.gamename))
            self.send(QR_MAGIC + bytes([ADDERROR]) + instance + b'\x01' + b'Unknown game name.\0', addr)
            del self.registry.hosts[addr]
            return
        if not host.validated:
            if not host.challenge:
                host.challenge = make_challenge(addr)
            log.debug('qr: %s:%d heartbeat (%s), sending challenge %s', addr[0], addr[1], host.name(), host.challenge)
            self.send(QR_MAGIC + bytes([CHALLENGE]) + instance + host.challenge.encode('ascii') + b'\0', addr)
        else:
            log.debug('qr: %s:%d heartbeat: %s', addr[0], addr[1], host.rules())
            self.registry.updated(host)

    def _challenge_response(self, addr, instance, payload):
        host = self.registry.hosts.get(addr)
        if host is None or host.instance != instance:
            log.debug('qr: challenge answer from %s:%d, which has not sent a heartbeat', addr[0], addr[1])
            return
        answer, _ = read_nts(payload, 0)
        self.send(QR_MAGIC + bytes([CLIENT_REGISTERED]) + instance, addr)
        if not host.validated:
            log.info('qr: %s hosts %s on %s:%d, UDP ports players use: %s (map %s, %s/%s players)', host.gamename,
                     access.safe(host.name()), addr[0], addr[1], ', '.join(map(str, host.udp_ports())),
                     access.safe(host.keys.get('mapname', '?')),
                     access.safe(host.keys.get('numplayers', len(host.players))), access.safe(host.keys.get('maxplayers', '?')))
            log.debug('qr: challenge %s answered %s', host.challenge, answer)
            self.registry.listed_now(host)

    # ---- messages to hosts

    def send_client_message(self, host, data):
        if len(self._pending) >= MAX_PENDING_MESSAGES:
            log.info('qr: too many unacknowledged messages to hosts; one to %s:%d dropped', host.ip, host.port)
            return
        key = next(self._msg_keys) & 0xFFFFFFFF
        packet = QR_MAGIC + bytes([CLIENT_MESSAGE]) + host.instance + struct.pack('>I', key) + data
        self._pending[key] = (host.addr, packet, RESEND_COUNT)
        self.send(packet, host.addr)
        log.debug('qr: client message %d to %s:%d: %s', key, host.ip, host.port, data[:64].hex())

    def tick(self):
        """Resends unacknowledged client messages and drops silent hosts (call every couple of seconds)."""
        for key, (addr, packet, tries) in list(self._pending.items()):
            if tries <= 0:
                del self._pending[key]
                continue
            self._pending[key] = (addr, packet, tries - 1)
            self.send(packet, addr)
        self.registry.expire()
