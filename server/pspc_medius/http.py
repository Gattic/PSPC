# SPDX-License-Identifier: GPL-2.0-or-later
"""A small asyncio HTTP/1.1 server for the game's web services (SVO pages, mission server, file
uploads): request line, headers, a Content-Length or chunked body, keep-alive."""

import asyncio
import logging
import urllib.parse

log = logging.getLogger('medius.http')

REASONS = {200: 'OK', 204: 'No Content', 302: 'Found', 400: 'Bad Request', 403: 'Forbidden', 404: 'Not Found',
           405: 'Method Not Allowed', 413: 'Payload Too Large', 500: 'Internal Server Error'}
MAX_BODY = 4 << 20


class Request:
    def __init__(self, method, target, version, headers, body, peer):
        self.method = method
        self.target = target
        self.version = version
        self.headers = headers          # lowercase name -> value
        self.body = body
        self.peer = peer
        parts = urllib.parse.urlsplit(target)
        self.path = urllib.parse.unquote(parts.path)
        self.query = {k: v[-1] for k, v in urllib.parse.parse_qs(parts.query, keep_blank_values=True).items()}
        if headers.get('content-type', '').startswith('application/x-www-form-urlencoded') and body:
            for k, v in urllib.parse.parse_qs(body.decode('latin-1'), keep_blank_values=True).items():
                self.query.setdefault(k, v[-1])


class Response:
    def __init__(self, status=200, body=b'', content_type='text/plain', headers=None):
        self.status = status
        self.body = body.encode('utf-8') if isinstance(body, str) else bytes(body)
        self.content_type = content_type
        self.headers = dict(headers or {})


async def _read_request(reader, peer):
    line = await reader.readline()
    while line in (b'\r\n', b'\n'):      # the game's HTTP client sends stray CRLFs after POST bodies
        line = await reader.readline()
    if not line:
        return None
    try:
        method, target, version = line.decode('latin-1').rstrip('\r\n').split(' ', 2)
    except ValueError:
        raise ValueError('bad request line %r' % line[:80])
    headers = {}
    while True:
        h = await reader.readline()
        if h in (b'\r\n', b'\n', b''):
            break
        name, _, value = h.decode('latin-1').partition(':')
        headers[name.strip().lower()] = value.strip()
    body = b''
    if headers.get('transfer-encoding', '').lower() == 'chunked':
        chunks = []
        while True:
            size = int((await reader.readline()).split(b';')[0].strip() or b'0', 16)
            if size == 0:
                await reader.readline()
                break
            chunks.append(await reader.readexactly(size))
            await reader.readline()
            if sum(map(len, chunks)) > MAX_BODY:
                raise ValueError('body too large')
        body = b''.join(chunks)
    elif headers.get('content-length'):
        n = int(headers['content-length'])
        if n > MAX_BODY:
            raise ValueError('body too large')
        body = await reader.readexactly(n)
    return Request(method, target, version, headers, body, peer)


def serve(handler):
    """A client_connected_cb for asyncio.start_server; handler(request) -> Response (may be async)."""
    async def client(reader, writer):
        peer = writer.get_extra_info('peername') or ('?', 0)
        try:
            while True:
                try:
                    req = await asyncio.wait_for(_read_request(reader, peer), 60)
                except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
                    break
                except ValueError as e:
                    log.warning('http %s: %s', peer[0], e)
                    break
                if req is None:
                    break
                try:
                    resp = handler(req)
                    if asyncio.iscoroutine(resp):
                        resp = await resp
                except Exception:
                    log.exception('http %s: error handling %s %s', peer[0], req.method, req.target)
                    resp = Response(500, 'Internal Server Error')
                log.info('http %s:%d: %s %s -> %d (%d bytes)', peer[0], peer[1], req.method, req.target, resp.status, len(resp.body))
                keep = req.headers.get('connection', '').lower() != 'close' and req.version == 'HTTP/1.1'
                head = ['HTTP/1.1 %d %s' % (resp.status, REASONS.get(resp.status, 'OK')),
                        'Content-Type: %s' % resp.content_type, 'Content-Length: %d' % len(resp.body),
                        'Connection: %s' % ('keep-alive' if keep else 'close'), 'Cache-Control: no-cache']
                for k, v in resp.headers.items():        # a list value repeats the header (Set-Cookie)
                    head += ['%s: %s' % (k, x) for x in (v if isinstance(v, list) else [v])]
                writer.write(('\r\n'.join(head) + '\r\n\r\n').encode('latin-1') + (resp.body if req.method != 'HEAD' else b''))
                await writer.drain()
                if not keep:
                    break
        except ConnectionError:
            pass
        finally:
            writer.close()
    return client
