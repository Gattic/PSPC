# SPDX-License-Identifier: GPL-2.0-or-later
"""One process with all of a GameSpy game's services: Presence (accounts, login, friends; gpcm
29900 and gpsp 29901), the master server (UDP 27900) and server browser (28910) for hosted games,
NAT negotiation (UDP 27901), stats and persistent storage (29920) and the web pages (HTTP 80)."""

import asyncio
import json
import logging
import os
import time

from . import leaderboards
from .careers import Careers
from .gp import Presence, Search
from .gstats import StatsServer
from .natneg import NatNegServer
from .qr import MasterServer, Reach, Registry
from .sb import ServerBrowser
from .web import WebServer

log = logging.getLogger('gamespy')

DEFAULT_MOTD = 'Welcome to a PSPC GameSpy server.'


class Config:
    def __init__(self, public_host='127.0.0.1', bind='0.0.0.0', gpcm_port=29900, gpsp_port=29901, qr_port=27900,
                 natneg_port=27901, sb_port=28910, gstats_port=29920, http_port=80, motd=DEFAULT_MOTD, status_file=None):
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


class GameSpyServer:
    def __init__(self, config, store):
        self.config = config
        self.store = store
        self.reach = Reach(config.public_host)
        self.presence = Presence(store)
        self.search = Search(store, self.presence)
        self.registry = Registry()
        self.careers = Careers(store)
        self.registry.watchers.add(self.careers)
        self.master = MasterServer(self.registry)
        self.natneg = NatNegServer(self.reach)
        self.browser = ServerBrowser(self.registry, self.master, self.reach)
        self.stats = StatsServer(store, self.presence, self.careers.round_ended)
        self.web = WebServer(store, config.motd)
        leaderboards.install(self.web, store)
        self._servers = []
        self._transports = []

    async def start(self):
        loop = asyncio.get_running_loop()
        c = self.config
        for name, handler, port in (('Presence (gpcm)', self.presence.handle, c.gpcm_port),
                                    ('Presence search (gpsp)', self.search.handle, c.gpsp_port),
                                    ('server browser', self.browser.handle, c.sb_port),
                                    ('stats', self.stats.handle, c.gstats_port),
                                    ('web', self.web.handle, c.http_port)):
            try:
                self._servers.append(await asyncio.start_server(handler, c.bind, port))
            except OSError as e:
                if handler == self.web.handle:
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

    def status(self):
        return {'time': int(time.time()),
                'online': [{'profileid': s.profileid, 'nick': s.profile['nick'], 'address': s.peer[0]}
                           for s in self.presence.online()],
                'games': [h.summary() for h in self.registry.listed()]}

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
        await self.start()
        await asyncio.gather(self.ticker(), *(s.serve_forever() for s in self._servers))

    def close(self):
        for s in self._servers:
            s.close()
        for t in self._transports:
            t.close()
