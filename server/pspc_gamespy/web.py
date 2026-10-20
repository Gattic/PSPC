# SPDX-License-Identifier: GPL-2.0-or-later
"""The web services games reach over HTTP (port 80): GameSpy's message of the day and version
check (motd.gamespy.com/motd/motd.asp and vercheck.asp) and the games' own pages, such as Elite
Squadron's leaderboards (gamestats.gamespy.com/swbfespsp/bfes_score.asp)."""

import asyncio
import logging
import urllib.parse

from . import access
from . import tunnel

log = logging.getLogger('gamespy')

REASONS = {200: 'OK', 400: 'Bad Request', 404: 'Not Found', 429: 'Too Many Requests', 500: 'Internal Server Error'}
MAX_BODY = 1 << 16
MAX_LINE = 4096                 # a request line or a header
MAX_HEADERS = 64
MAX_REQUESTS = 100              # per connection


class Request:
    def __init__(self, method, target, headers, body, peer):
        self.method = method
        self.target = target
        self.headers = headers
        self.body = body
        self.peer = peer
        parts = urllib.parse.urlsplit(target)
        self.path = urllib.parse.unquote(parts.path)
        self.query = {}
        self.query_all = urllib.parse.parse_qs(parts.query, keep_blank_values=True)
        for k, v in self.query_all.items():
            self.query[k.lower()] = v[-1]
        self.host = headers.get('host', '').split(':')[0].lower()


async def read_request(reader, peer):
    """The next request, None at the end of the connection; ValueError for one that is too big or malformed."""
    line = await reader.readline()
    while line in (b'\r\n', b'\n'):
        line = await reader.readline()
    if not line:
        return None
    if len(line) > MAX_LINE:
        raise ValueError('request line too long')
    method, target, _ = (line.decode('latin-1').rstrip('\r\n').split(' ', 2) + ['', ''])[:3]
    headers = {}
    while True:
        h = await reader.readline()
        if h in (b'\r\n', b'\n', b''):
            break
        if len(h) > MAX_LINE or len(headers) >= MAX_HEADERS:
            raise ValueError('too many or too long headers')
        name, _, value = h.decode('latin-1').partition(':')
        headers[name.strip().lower()] = value.strip()
    if 'transfer-encoding' in headers:
        raise ValueError('chunked bodies are not accepted')      # one way a request is read differently by a proxy and here
    body = b''
    n = int(headers.get('content-length') or 0)
    if n > MAX_BODY or n < 0:
        raise ValueError('body too long')
    if n > 0:
        body = await reader.readexactly(n)
    return Request(method, target, headers, body, peer)


class WebServer:
    def __init__(self, store, motd='', limits=None, behind_proxy=False):
        self.store = store
        self.limits = limits or access.Limits.none()
        self.behind_proxy = behind_proxy    # requests come through a proxy here: the player's address is in its headers
        self.tunnel = None                  # tunnel.Gateway: answers /tunnel/ (a WebSocket for each connection a player's relay opens)
        self.motd = motd
        self.pages = {}                 # lower-case path -> handler(request) -> (status, body, content type)
        self.add('/motd/motd.asp', self.motd_page)
        self.add('/motd/vercheck.asp', self.vercheck_page)

    def add(self, path, handler):
        self.pages[path.lower()] = handler

    def motd_page(self, req):
        return 200, self.motd, 'text/html'

    def vercheck_page(self, req):
        return 200, '\\newver\\0', 'text/html'          # no newer version

    def respond(self, req):
        handler = self.pages.get(req.path.lower())
        if handler is None:
            log.info('web %s: %s %s (Host %s): no such page', req.peer[0], access.safe(req.method, 10), access.safe(req.target),
                     access.safe(req.host, 60))
            return 404, 'Not found', 'text/plain'
        status, body, ctype = handler(req)
        log.info('web %s: %s %s -> %d, %d bytes', req.peer[0], access.safe(req.method, 10), access.safe(req.target), status, len(body))
        return status, body, ctype

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername') or ('?', 0)
        try:
            for _ in range(MAX_REQUESTS):
                try:
                    req = await asyncio.wait_for(read_request(reader, peer), 30)
                except (asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
                    break
                if req is None:
                    break
                req.peer = (access.client_ip(peer[0], req.headers, self.behind_proxy), peer[1])
                if self.tunnel is not None and req.path.startswith(tunnel.PATH):
                    await self.tunnel.handle(req, reader, writer)
                    return
                if not self.limits.allow(self.limits.web, req.peer[0]):
                    status, body, ctype = 429, 'Too many requests', 'text/plain'
                else:
                    try:
                        status, body, ctype = self.respond(req)
                    except Exception:
                        log.exception('web: %s failed', access.safe(req.target))
                        status, body, ctype = 500, 'Server error', 'text/plain'
                body = body.encode('utf-8') if isinstance(body, str) else body
                keep = req.headers.get('connection', '').lower() == 'keep-alive' and status != 429
                head = ('HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n'
                        'Cache-Control: no-cache\r\nX-Content-Type-Options: nosniff\r\n\r\n' %
                        (status, REASONS.get(status, 'OK'), ctype, len(body), 'keep-alive' if keep else 'close'))
                writer.write(head.encode('latin-1') + body)
                await writer.drain()
                if not keep:
                    break
        except (ConnectionError, OSError):
            pass
        finally:
            try:
                writer.close()
            except Exception:
                pass
