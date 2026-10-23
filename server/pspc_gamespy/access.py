# SPDX-License-Identifier: GPL-2.0-or-later
"""Who is asking, and how much they may ask: the limits that keep one client, or one flood, from
using up a server that anyone on the internet can reach.

- Addresses: the server's own machine (127.0.0.1: its dedicated game servers, its administrator) is
  trusted and never limited. Players that reach the server through the tunnel (tunnel.py) appear at
  addresses in TUNNEL_NET, one per real client address, and are limited like any other client.
- ConnectionLimit: simultaneous TCP connections, per client address and in all, for one service.
- RateLimiter: a token bucket per client address, for requests, UDP packets and failed logins.
- safe(): what to log of text a client sent."""

import ipaddress
import logging
import time

log = logging.getLogger('gamespy')

# Tunnelled players are given an address here for as long as they're connected (tunnel.py).
TUNNEL_NET = ipaddress.ip_network('127.64.0.0/16')


def _ip(text):
    try:
        return ipaddress.ip_address(text)
    except ValueError:
        return None


def _could_be_loopback(ip):
    """False for a dotted address outside 127.0.0.0/8, which is every client on the internet. Parsing it with `ipaddress`
    costs more than everything else the server does with a UDP packet that it drops, and spoofed sources make the
    answer needed for each one: a string that is not an IPv4 address in 127/8 and has no colon is not loopback."""
    return ip.startswith('127.') or ':' in ip


def is_tunnel(ip):
    if not _could_be_loopback(ip):
        return False
    a = _ip(ip)
    return a is not None and a in TUNNEL_NET


def is_local(ip):
    """The server's own machine: loopback, other than the tunnel's players."""
    if not _could_be_loopback(ip):
        return False
    a = _ip(ip)
    return a is not None and a.is_loopback and a not in TUNNEL_NET


def client_ip(peer, headers, behind_proxy):
    """Who is asking: the connection's address - or, for a request that came through a proxy on this
    machine (a Cloudflare Tunnel's cloudflared), the address the proxy says the player has. Only the
    server's own machine is believed about that. Cloudflare's CF-Connecting-IP is the player's address
    and nothing a player sent; X-Forwarded-For (for other proxies) is a list a player can start, so its
    last entry - the one our own proxy added - is the one to believe."""
    if behind_proxy and is_local(peer):
        forwarded = headers.get('cf-connecting-ip') or headers.get('x-forwarded-for', '').split(',')[-1].strip()
        try:
            return str(ipaddress.ip_address(forwarded))
        except ValueError:
            pass
    return peer


def peer_ip(writer):
    peer = writer.get_extra_info('peername')
    return peer[0] if peer else ''


def drop_if_backlogged(writer, limit=1 << 20):
    """A client that isn't reading what it was sent (more than `limit` bytes are waiting) is
    disconnected rather than letting what the server has to say to it pile up in memory.
    Returns whether it was."""
    transport = writer.transport
    if transport is None or transport.get_write_buffer_size() <= limit:
        return False
    transport.abort()
    return True


def patience(ip, seconds):
    """How long a connection may stay silent before it is closed: None (as long as it likes) for the
    server's own machine, whose dedicated servers wait for hours between reports."""
    return None if is_local(ip) else seconds


def safe(text, limit=80):
    """Text from a client, for a log line: no control characters or line breaks, cut short."""
    text = ascii(str(text))[1:-1]
    return text if len(text) <= limit else text[:limit] + '...'


class RateLimiter:
    """A token bucket per key (a client address): `burst` requests at once, `per_second` refilled
    each second. Keys that have been quiet long enough to be full again are forgotten."""

    def __init__(self, burst, per_second, max_keys=50000):
        self.burst = float(burst)
        self.per_second = float(per_second)
        self.max_keys = max_keys
        self.buckets = {}               # key -> [tokens, time of the last update]

    def _bucket(self, key, now):
        b = self.buckets.get(key)
        if b is None:
            if len(self.buckets) >= self.max_keys:
                self._purge(now)
            b = self.buckets[key] = [self.burst, now]
        else:
            b[0] = min(self.burst, b[0] + (now - b[1]) * self.per_second)
            b[1] = now
        return b

    def _purge(self, now):
        full = self.burst / self.per_second if self.per_second else 0
        for k in [k for k, b in self.buckets.items() if now - b[1] >= full]:
            del self.buckets[k]
        if len(self.buckets) >= self.max_keys:      # still full: forget the oldest half
            for k in sorted(self.buckets, key=lambda k: self.buckets[k][1])[:self.max_keys // 2]:
                del self.buckets[k]

    def allow(self, key, cost=1.0):
        """Takes `cost` tokens if the key has them; False (and nothing taken) if not."""
        if is_local(key):
            return True
        b = self._bucket(key, time.monotonic())
        if b[0] < cost:
            return False
        b[0] -= cost
        return True

    def available(self, key, cost=1.0):
        """Whether the key could take `cost` tokens now, without taking them."""
        if is_local(key):
            return True
        return self._bucket(key, time.monotonic())[0] >= cost

    def spend(self, key, cost=1.0):
        """Takes tokens whether or not the key has them (to a floor of -burst)."""
        if not is_local(key):
            b = self._bucket(key, time.monotonic())
            b[0] = max(-self.burst, b[0] - cost)


class RareLog:
    """Permission to write a log line about something that anyone can cause from any address (a forged heartbeat, a
    connection that says nothing): once a minute for each address (a table of at most `max_keys`), at most `burst` lines at
    once and then `per_second` in all, whatever the number of addresses. `count` is how often it happened, said or not."""

    def __init__(self, per_address_seconds=60.0, burst=10, per_second=0.1, max_keys=1000):
        self._by_address = RateLimiter(1, 1.0 / per_address_seconds, max_keys=max_keys)
        self._overall = RateLimiter(burst, per_second)
        self.count = 0

    def allow(self, address):
        self.count += 1
        if not self._by_address.available(address) or not self._overall.allow('all'):
            return False
        self._by_address.spend(address)
        return True


class ConnectionLimit:
    """At most `per_ip` simultaneous connections from one address and `total` in all, for one
    service. guard() wraps a connection handler; a connection over the limit is closed at once."""

    def __init__(self, name, per_ip, total):
        self.name = name
        self.per_ip = per_ip
        self.total = total
        self.count = 0
        self.by_ip = {}

    def enter(self, ip):
        if is_local(ip):
            self.count += 1
            return True
        if self.count >= self.total or self.by_ip.get(ip, 0) >= self.per_ip:
            return False
        self.count += 1
        self.by_ip[ip] = self.by_ip.get(ip, 0) + 1
        return True

    def leave(self, ip):
        self.count -= 1
        if ip in self.by_ip:
            self.by_ip[ip] -= 1
            if self.by_ip[ip] <= 0:
                del self.by_ip[ip]

    def guard(self, handler):
        async def limited(reader, writer):
            ip = peer_ip(writer)
            if not self.enter(ip):
                log.info('%s: too many connections (%s has %d of %d, %d of %d in all); closed', self.name, safe(ip),
                         self.by_ip.get(ip, 0), self.per_ip, self.count, self.total)
                writer.close()
                return
            try:
                await handler(reader, writer)
            finally:
                self.leave(ip)
        return limited


class Limits:
    """All of a server's limits in one place. Defaults suit a public server; `Limits.none()` (the
    default for components built without one) limits nothing."""

    def __init__(self, enabled=True):
        self.enabled = enabled
        # TCP connections per client address and in all, for each service.
        self.connections = {name: ConnectionLimit(name, per_ip, total) for name, per_ip, total in (
            ('Presence', 16, 2000), ('Presence search', 16, 500), ('server browser', 16, 1000), ('stats', 16, 1000),
            ('web', 32, 500))}
        # Wrong passwords: ten, then one more every 30 seconds (guessing is slowed to 2 a minute).
        self.failed_logins = RateLimiter(10, 1 / 30.0)
        self.new_accounts = RateLimiter(10, 1 / 360.0)          # ten at once, then ten an hour
        self.searches = RateLimiter(60, 2.0)                    # Presence search requests
        self.stats_messages = RateLimiter(100, 25.0)
        self.presence_messages = RateLimiter(100, 20.0)         # messages on Presence connections
        self.browser_requests = RateLimiter(20, 4.0)            # list and info requests to the server browser
        self.web = RateLimiter(60, 20.0)
        self.host_messages = RateLimiter(10, 0.5)               # messages players have the server send to hosts
        self.qr_packets = RateLimiter(60, 20.0)                 # UDP, per source address
        self.natneg_packets = RateLimiter(120, 40.0)
        self.hosts_per_ip = 2                                   # hosted games listed per address (not the server's own)
        self.max_player_hosts = 512                             # games hosted by players listed at once (not the server's own)
        self.new_hosts = RateLimiter(10, 5.0)                   # heartbeats parsed, in all, from addresses with no game listed
        self.max_natneg_sessions = 4096

    def guard(self, service, handler):
        return self.connections[service].guard(handler) if self.enabled else handler

    def allow(self, limiter, key, cost=1.0):
        return not self.enabled or limiter.allow(key, cost)

    def available(self, limiter, key):
        return not self.enabled or limiter.available(key)

    def spend(self, limiter, key, cost=1.0):
        if self.enabled:
            limiter.spend(key, cost)

    @classmethod
    def none(cls):
        return cls(enabled=False)
