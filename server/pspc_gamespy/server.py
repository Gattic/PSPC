# SPDX-License-Identifier: GPL-2.0-or-later
"""One process with all of a GameSpy game's services: Presence (accounts, login, friends; gpcm
29900 and gpsp 29901), the master server (UDP 27900) and server browser (28910) for hosted games,
NAT negotiation (UDP 27901), stats and persistent storage (29920) and the web pages (HTTP 80)."""

import asyncio
import json
import logging
import os
import time

from . import access
from . import leaderboards
from .careers import Careers
from .gp import Presence, Search
from .gstats import StatsServer
from .natneg import NatNegServer
from .qr import MasterServer, Reach, Registry
from .sb import ServerBrowser
from .tunnel import Gateway
from .web import WebServer

log = logging.getLogger('gamespy')

DEFAULT_MOTD = 'Welcome to a PSPC GameSpy server.'


class Config:
    def __init__(self, public_host='127.0.0.1', bind='0.0.0.0', gpcm_port=29900, gpsp_port=29901, qr_port=27900,
                 natneg_port=27901, sb_port=28910, gstats_port=29920, http_port=80, motd=DEFAULT_MOTD, status_file=None,
                 sessions=None, limits=None, tunnel=False, behind_proxy=False, local_hosts_only=False):
        self.public_host = public_host      # the address players on other PCs reach this server at
        self.bind = bind
        self.gpcm_port = gpcm_port
        self.gpsp_port = gpsp_port
        self.qr_port = qr_port
        self.natneg_port = natneg_port
        self.sb_port = sb_port
        self.gstats_port = gstats_port
        self.http_port = http_port
        self.motd = motd
        self.status_file = status_file      # where to keep a JSON summary (online players, hosted games)
        self.sessions = sessions            # sessions.Sessions: programs run alongside (dedicated servers), or None
        self.limits = limits or access.Limits()     # access.py: what one client may do
        self.tunnel = tunnel                # accept players over WebSockets on the web port (tunnel.py)
        self.behind_proxy = behind_proxy    # the web port's requests come through a proxy (cloudflared): trust its client address
        self.local_hosts_only = local_hosts_only    # only this machine (its dedicated servers) may list games


class GameSpyServer:
    def __init__(self, config, store):
        self.config = config
        self.store = store
        self.limits = config.limits
        self.reach = Reach(config.public_host)
        self.presence = Presence(store, self.limits)
        self.search = Search(store, self.presence)
        self.registry = Registry(self.limits)
        self.careers = Careers(store)
        self.registry.watchers.add(self.careers)
        # Games are listed from the server's own machine (its dedicated servers) and, unless local_hosts_only,
        # from other addresses; never from tunnelled players (tunnel.py), whom other players could not reach.
        self.master = MasterServer(self.registry, can_host=self.may_host)
        self.natneg = NatNegServer(self.reach, self.limits)
        self.browser = ServerBrowser(self.registry, self.master, self.reach)
        self.stats = StatsServer(store, self.presence, self.careers.round_ended, self.limits)
        self.web = WebServer(store, config.motd, self.limits, config.behind_proxy)
        if config.tunnel:
            self.web.tunnel = self.gateway = Gateway(config, self.registry, self.limits)
        leaderboards.install(self.web, store)
        self._servers = []
        self._transports = []

    def may_host(self, ip):
        if self.config.local_hosts_only:
            return access.is_local(ip)
        return not access.is_tunnel(ip)

    async def start(self):
        loop = asyncio.get_running_loop()
        c = self.config
        for name, service, handler, port in (('Presence (gpcm)', 'Presence', self.presence.handle, c.gpcm_port),
                                             ('Presence search (gpsp)', 'Presence search', self.search.handle, c.gpsp_port),
                                             ('server browser', 'server browser', self.browser.handle, c.sb_port),
                                             ('stats', 'stats', self.stats.handle, c.gstats_port),
                                             ('web', 'web', self.web.handle, c.http_port)):
            try:
                self._servers.append(await asyncio.start_server(self.limits.guard(service, handler), c.bind, port))
            except OSError as e:
                if service == 'web':
                    # Games play without it (they ignore the message of the day); only leaderboards go.
                    log.warning('the web pages (leaderboards) are off: TCP %d is in use or not allowed (%s)', port, e.strerror)
                    continue
                raise OSError(e.errno, 'the %s port, TCP %d, is in use or not allowed (%s)' % (name, port, e.strerror))
            log.info('%s listening on %s:%d', name, c.bind, port)
        for name, proto, port in (('master server (QR2)', self.master, c.qr_port), ('NAT negotiation', self.natneg, c.natneg_port)):
            try:
                transport, _ = await loop.create_datagram_endpoint(lambda p=proto: p, local_addr=(c.bind, port))
            except OSError as e:
                raise OSError(e.errno, 'the %s port, UDP %d, is in use or not allowed (%s)' % (name, port, e.strerror))
            self._transports.append(transport)
            log.info('%s listening on %s:%d (UDP)', name, c.bind, port)

    async def ticker(self):
        n = 0
        while True:
            await asyncio.sleep(1)
            n += 1
            self.natneg.tick()
            if n % 2 == 0:
                self.master.tick()
            if n % 5 == 0 and self.config.status_file:
                self.write_status()
            if n % 300 == 0:
                await asyncio.get_running_loop().run_in_executor(None, self.reach.refresh)     # a name's address may change
            if n % 86400 == 0:
                self.prune()

    def prune(self):
        gone = self.store.prune()
        if gone:
            log.info('forgot %d old games, messages and requests', gone)

    def status(self):
        return {'time': int(time.time()),
                'online': [{'profileid': s.profileid, 'nick': s.profile['nick'], 'address': s.peer[0]}
                           for s in self.presence.online()],
                'games': [h.summary() for h in self.registry.listed()],
                'sessions': self.config.sessions.status() if self.config.sessions else [],
                'tunnel': self.web.tunnel.status() if self.web.tunnel else None}

    def write_status(self):
        path = self.config.status_file
        try:
            tmp = path + '.tmp'
            with open(tmp, 'w', encoding='utf-8') as f:
                json.dump(self.status(), f)
            os.replace(tmp, path)
        except OSError as e:
            log.debug('status file: %s', e)

    async def serve_forever(self):
        self.prune()
        await self.start()
        tasks = [self.ticker()] + [s.serve_forever() for s in self._servers]
        if self.config.sessions:
            tasks.append(self.config.sessions.run())
        await asyncio.gather(*tasks)

    def close(self):
        if self.config.sessions:
            self.config.sessions.stop()
        for s in self._servers:
            s.close()
        for t in self._transports:
            t.close()
