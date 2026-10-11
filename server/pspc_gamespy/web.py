# SPDX-License-Identifier: GPL-2.0-or-later
"""The web services games reach over HTTP (port 80): GameSpy's message of the day and version
check (motd.gamespy.com/motd/motd.asp and vercheck.asp) and the games' own pages, such as Elite
Squadron's leaderboards (gamestats.gamespy.com/swbfespsp/bfes_score.asp)."""

import asyncio
import logging
import urllib.parse

log = logging.getLogger('gamespy')

REASONS = {200: 'OK', 400: 'Bad Request', 404: 'Not Found', 500: 'Internal Server Error'}
MAX_BODY = 1 << 20


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
    line = await reader.readline()
    while line in (b'\r\n', b'\n'):
        line = await reader.readline()
    if not line:
        return None
    method, target, _ = (line.decode('latin-1').rstrip('\r\n').split(' ', 2) + ['', ''])[:3]
    headers = {}
    while True:
        h = await reader.readline()
        if h in (b'\r\n', b'\n', b''):
            break
        name, _, value = h.decode('latin-1').partition(':')
        headers[name.strip().lower()] = value.strip()
    body = b''
    n = int(headers.get('content-length') or 0)
    if 0 < n <= MAX_BODY:
        body = await reader.readexactly(n)
    return Request(method, target, headers, body, peer)


class WebServer:
    def __init__(self, store, motd=''):
        self.store = store
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
            log.info('web %s: %s %s (Host %s): no such page', req.peer[0], req.method, req.target, req.host)
            return 404, 'Not found', 'text/plain'
        status, body, ctype = handler(req)
        log.info('web %s: %s %s -> %d, %d bytes', req.peer[0], req.method, req.target, status, len(body))
        return status, body, ctype

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername') or ('?', 0)
        try:
            while True:
                try:
                    req = await asyncio.wait_for(read_request(reader, peer), 30)
                except (asyncio.TimeoutError, asyncio.IncompleteReadError, ValueError):
                    break
                if req is None:
                    break
                try:
                    status, body, ctype = self.respond(req)
                except Exception:
                    log.exception('web: %s failed', req.target)
                    status, body, ctype = 500, 'Server error', 'text/plain'
                body = body.encode('utf-8') if isinstance(body, str) else body
                keep = req.headers.get('connection', '').lower() == 'keep-alive'
                head = ('HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: %s\r\n'
                        'Cache-Control: no-cache\r\n\r\n' % (status, REASONS.get(status, 'OK'), ctype, len(body),
                                                             'keep-alive' if keep else 'close'))
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
