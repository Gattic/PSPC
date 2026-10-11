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
import string
import struct
import time

from . import games

log = logging.getLogger('gamespy')

QR_MAGIC = b'\xfe\xfd'
QUERY, CHALLENGE, ECHO, HEARTBEAT, ADDERROR, ECHO_RESPONSE = 0, 1, 2, 3, 4, 5
CLIENT_MESSAGE, CLIENT_MESSAGE_ACK, KEEPALIVE, AVAILABLE, CLIENT_REGISTERED = 6, 7, 8, 9, 10

STATE_CHANGED, STATE_EXITING, STATE_INITIAL = 1, 2, 3
HOST_TIMEOUT = 180          # seconds without a heartbeat or keepalive before a host is dropped
RESEND_INTERVAL = 2.0
RESEND_COUNT = 5


def is_loopback(ip):
    try:
        return ipaddress.ip_address(ip).is_loopback
    except ValueError:
        return False


class Reach:
    """Which address a player is given for another machine. Hosts and players on the server's own
    machine are seen at 127.0.0.1; players elsewhere reach them at the server's public address."""

    def __init__(self, public_host='127.0.0.1'):
        self.public_host = public_host

    def ip_for(self, ip, viewer_ip):
        if is_loopback(ip) and viewer_ip and not is_loopback(viewer_ip) and not is_loopback(self.public_host):
            return self.public_host
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
        self.last_seen = time.monotonic()

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
                'player_names': [p.get('player_', '') for p in self.players], 'since': int(self.created)}


def parse_heartbeat(payload):
    """(keys, players, teams) from a heartbeat's payload."""
    keys = {}
    pos = 0
    while pos < len(payload):
        name, pos = read_nts(payload, pos)
        if not name:
            break
        value, pos = read_nts(payload, pos)
        keys.setdefault(name, value)
    players, teams = [], []
    while pos + 2 <= len(payload):
        count = struct.unpack_from('>H', payload, pos)[0]
        pos += 2
        names = []
        while pos < len(payload):
            name, pos = read_nts(payload, pos)
            if not name:
                break
            names.append(name)
        if not names:
            if count == 0:
                continue
            break
        rows = []
        for _ in range(count):
            row = {}
            for name in names:
                row[name], pos = read_nts(payload, pos)
            rows.append(row)
        if all(n.endswith('_t') for n in names):
            teams = rows
        else:
            players = rows
    return keys, players, teams


def make_challenge(addr):
    a, b, c, d = (int(x) for x in addr[0].split('.'))
    return (''.join(random.choice(string.ascii_letters + string.digits) for _ in range(6)) +
            '%x%x' % (random.randrange(8), random.randrange(16)) +
            '%08X%04X' % ((a << 24) | (b << 16) | (c << 8) | d, addr[1]))


class Registry:
    """The hosted games, and who wants to hear about changes (server browser connections with push
    updates): watchers have host_added(host), host_updated(host) and host_removed(host)."""

    def __init__(self):
        self.hosts = {}                 # (ip, port) -> Host
        self.watchers = set()

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
                log.info('qr: %s (%s:%d) is no longer listed%s', host.name(), host.ip, host.port, why and ' (%s)' % why)
                self._notify('host_removed', host)

    def expire(self):
        now = time.monotonic()
        for h in list(self.hosts.values()):
            if now - h.last_seen > HOST_TIMEOUT:
                self.remove(h, 'timed out')


class MasterServer(asyncio.DatagramProtocol):
    def __init__(self, registry):
        self.registry = registry
        self.transport = None
        self._msg_keys = itertools.count(random.randrange(1, 1 << 30))
        self._pending = {}              # message key -> (host addr, packet, tries left)

    def connection_made(self, transport):
        self.transport = transport

    def send(self, packet, addr):
        if self.transport is not None:
            self.transport.sendto(packet, addr)

    def datagram_received(self, data, addr):
        try:
            self._handle(data, addr)
        except Exception:
            log.exception('qr: bad packet from %s:%d: %s', addr[0], addr[1], data[:64].hex())

    def _handle(self, data, addr):
        if len(data) < 5:
            return
        ptype, instance, payload = data[0], data[1:5], data[5:]
        host = self.registry.hosts.get(addr)
        if host is not None:
            host.last_seen = time.monotonic()
        if ptype == AVAILABLE:
            gamename, _ = read_nts(payload, 0)
            log.info('qr: %s:%d asks whether %s is available%s', addr[0], addr[1], gamename,
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

    def _heartbeat(self, addr, instance, payload):
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
            host = Host(addr, instance)
            self.registry.hosts[addr] = host
        host.gamename = gamename or host.gamename
        host.keys = keys
        host.players = players
        host.teams = teams
        if not games.find(host.gamename):
            log.warning('qr: heartbeat from %s:%d for %r, a game this server has no key for', addr[0], addr[1], host.gamename)
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
            log.info('qr: %s hosts %s on %s:%d (map %s, %s/%s players)', host.gamename, host.name(), addr[0], addr[1],
                     host.keys.get('mapname', '?'), host.keys.get('numplayers', len(host.players)),
                     host.keys.get('maxplayers', '?'))
            log.debug('qr: challenge %s answered %s', host.challenge, answer)
            self.registry.listed_now(host)

    # ---- messages to hosts

    def send_client_message(self, host, data):
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
