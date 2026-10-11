# SPDX-License-Identifier: GPL-2.0-or-later
"""NAT negotiation (natneg1/2/3.gamespy.com, UDP 27901): two players who want to talk (a host and
someone joining it, who agreed on a random cookie through a server browser message) each send init
packets here; once both have, each is told the other's address and they open the connection to
each other.

Packets are [FD FC 1E 66 6A B2][version][type][cookie 4][...]. Init (0) and its ack (1), address
check (10) and reply (11), ERT test (2): [port type][client index][use game port][local IP 4]
[local port 2] (version 1 has no local address; init adds the game name). Port types: 0 the game's
own socket, 1-3 the negotiation socket (to natneg1, 2 and 3). Connect (5): [remote IP 4][remote
port 2][got your data][finished: 0, or 1 when the partner never came]; acknowledged with 6.
Report (13) and its ack (14): the outcome."""

import asyncio
import logging
import socket
import struct
import time

log = logging.getLogger('gamespy')

MAGIC = b'\xfd\xfc\x1e\x66\x6a\xb2'
INIT, INITACK, ERTTEST, ERTACK, STATEUPDATE, CONNECT, CONNECT_ACK, CONNECT_PING = 0, 1, 2, 3, 4, 5, 6, 7
BACKUP_TEST, BACKUP_ACK, ADDRESS_CHECK, ADDRESS_REPLY, NATIFY_REQUEST, REPORT, REPORT_ACK = 8, 9, 10, 11, 12, 13, 14
PREINIT, PREINIT_ACK = 15, 16
PT_GP = 0
FINISHED_NOERROR, FINISHED_ERROR_DEADBEAT_PARTNER = 0, 1
PREINIT_WAITING_FOR_CLIENT, PREINIT_WAITING_FOR_MATCHUP, PREINIT_READY = 0, 1, 2

PARTNER_WAIT = 30.0         # seconds a client waits here for its partner
CONNECT_RESENDS = 4
SESSION_LIFETIME = 120.0


class Client:
    def __init__(self, index):
        self.index = index
        self.ports = {}                 # port type -> (ip, port) the init came from
        self.use_game_port = False
        self.local = ('0.0.0.0', 0)
        self.version = 3
        self.gamename = ''
        self.acked = False

    def game_address(self):
        """Where its game traffic comes from: the game's socket, or else the negotiation socket."""
        if self.use_game_port:
            return self.ports.get(PT_GP)
        return self.ports.get(1) or next((a for t, a in sorted(self.ports.items()) if t != PT_GP), None)

    def ready(self):
        return self.game_address() is not None


class Session:
    def __init__(self, cookie):
        self.cookie = cookie
        self.clients = {}               # client index -> Client
        self.created = time.monotonic()
        self.connected_at = None
        self.resends = 0
        self.preinit = set()


class NatNegServer(asyncio.DatagramProtocol):
    def __init__(self, reach):
        self.reach = reach
        self.transport = None
        self.sessions = {}              # cookie -> Session

    def connection_made(self, transport):
        self.transport = transport

    def send(self, packet, addr):
        if self.transport is not None and addr:
            self.transport.sendto(packet, addr)

    def datagram_received(self, data, addr):
        try:
            self._handle(data, addr)
        except Exception:
            log.exception('natneg: bad packet from %s:%d: %s', addr[0], addr[1], data[:64].hex())

    @staticmethod
    def header(version, ptype, cookie):
        return MAGIC + bytes([version, ptype]) + struct.pack('>I', cookie)

    def _handle(self, data, addr):
        if len(data) < 12 or data[:6] != MAGIC:
            log.debug('natneg: %d bytes from %s:%d that are not NAT negotiation', len(data), addr[0], addr[1])
            return
        version, ptype, cookie = data[6], data[7], struct.unpack_from('>I', data, 8)[0]
        body = data[12:]
        if ptype == INIT:
            self._init(version, cookie, body, addr)
        elif ptype == CONNECT_ACK:
            index = body[1] if len(body) > 1 else 0
            s = self.sessions.get(cookie)
            if s and index in s.clients:
                s.clients[index].acked = True
            log.debug('natneg %08x: client %d got its partner\'s address', cookie, index)
        elif ptype == ADDRESS_CHECK:
            reply = bytearray(body[:9].ljust(9, b'\0'))
            reply[3:7] = socket.inet_aton(addr[0])
            reply[7:9] = struct.pack('>H', addr[1])
            self.send(self.header(version, ADDRESS_REPLY, cookie) + bytes(reply), addr)
        elif ptype == NATIFY_REQUEST:
            # The NAT type check: answer from here (the original sent ERT tests from other addresses
            # too; one answer reads as an open NAT, which is what players on one network have).
            self.send(self.header(version, ERTTEST, cookie) + body[:9].ljust(9, b'\0'), addr)
        elif ptype == REPORT:
            if len(body) >= 3:
                gamename = body[11:61].split(b'\0')[0].decode('latin-1', 'replace') if len(body) > 11 else ''
                log.info('natneg %08x: client %d reports %s%s', cookie, body[1],
                         'success' if body[2] else 'failure', ' (%s)' % gamename if gamename else '')
            self.send(self.header(version, REPORT_ACK, cookie) + body[:61].ljust(61, b'\0'), addr)
        elif ptype == PREINIT:
            index = body[0] if body else 0
            s = self.sessions.setdefault(cookie, Session(cookie))
            s.preinit.add(index)
            state = PREINIT_READY if len(s.preinit) >= 2 else PREINIT_WAITING_FOR_MATCHUP
            self.send(self.header(version, PREINIT_ACK, cookie) + bytes([index, state]) + body[2:6].ljust(4, b'\0'), addr)
        else:
            log.debug('natneg %08x: packet type %d from %s:%d', cookie, ptype, addr[0], addr[1])

    def _init(self, version, cookie, body, addr):
        porttype, index, use_game_port = body[0], body[1], body[2]
        s = self.sessions.get(cookie)
        if s is None:
            s = self.sessions[cookie] = Session(cookie)
            log.info('natneg %08x: negotiation started from %s:%d', cookie, addr[0], addr[1])
        c = s.clients.get(index)
        if c is None:
            c = s.clients[index] = Client(index)
        c.version = version
        c.use_game_port = bool(use_game_port)
        c.ports[porttype] = addr
        if version > 1 and len(body) >= 9:
            c.local = (socket.inet_ntoa(body[3:7]), struct.unpack_from('>H', body, 7)[0])
            end = body.find(b'\0', 9)
            c.gamename = body[9:end if end >= 0 else len(body)].decode('latin-1', 'replace')
        size = 3 if version == 1 else 9
        self.send(self.header(version, INITACK, cookie) + body[:size].ljust(size, b'\0'), addr)
        log.debug('natneg %08x: init from client %d, port type %d, %s:%d (local %s:%d)', cookie, index, porttype,
                  addr[0], addr[1], c.local[0], c.local[1])
        if s.connected_at is None and len(s.clients) >= 2 and all(cl.ready() for cl in s.clients.values()):
            self._connect(s)
        elif s.connected_at is not None and not c.acked:
            self._send_connect(s, c)           # it missed the connect packet

    def _partner_address(self, partner, viewer):
        a = partner.game_address()
        viewer_addr = viewer.game_address()
        ip = self.reach.ip_for(a[0], viewer_addr[0] if viewer_addr else '')
        return ip, a[1]

    def _send_connect(self, s, c, finished=FINISHED_NOERROR):
        partner = next((p for i, p in s.clients.items() if i != c.index), None)
        if finished == FINISHED_NOERROR and partner is not None:
            ip, port = self._partner_address(partner, c)
        else:
            ip, port = '0.0.0.0', 0
        body = socket.inet_aton(ip) + struct.pack('>H', port) + bytes([0x42, finished])
        self.send(self.header(c.version, CONNECT, s.cookie) + body, c.game_address())
        return ip, port

    def _connect(self, s):
        s.connected_at = time.monotonic()
        for c in s.clients.values():
            ip, port = self._send_connect(s, c)
            log.info('natneg %08x: client %d (%s:%d) is told to connect to %s:%d', s.cookie, c.index,
                     *c.game_address(), ip, port)

    def tick(self):
        """Resends connects that weren't acknowledged, gives up on missing partners, forgets old
        sessions (call every second or so)."""
        now = time.monotonic()
        for cookie, s in list(self.sessions.items()):
            if s.connected_at is not None:
                if s.resends < CONNECT_RESENDS and not all(c.acked for c in s.clients.values()):
                    s.resends += 1
                    for c in s.clients.values():
                        if not c.acked:
                            self._send_connect(s, c)
                if now - s.connected_at > SESSION_LIFETIME:
                    del self.sessions[cookie]
            elif now - s.created > PARTNER_WAIT:
                for c in s.clients.values():
                    if c.ready():
                        self._send_connect(s, c, FINISHED_ERROR_DEADBEAT_PARTNER)
                log.info('natneg %08x: the partner never came', cookie)
                del self.sessions[cookie]
