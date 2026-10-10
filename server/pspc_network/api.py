# SPDX-License-Identifier: GPL-2.0-or-later
"""The PSPC Network HTTP API (JSON). See server/README.md for the endpoints."""

import base64
import json
import secrets
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import unquote

from . import __version__, ticket
from .store import StoreError

TICKET_LIFETIME_MS = 15 * 60 * 1000
MAX_BODY = 64 * 1024


class RateLimiter:
    """At most `count` attempts per `window` seconds per client address."""

    def __init__(self, count, window):
        self.count, self.window = count, window
        self._hits, self._lock = {}, threading.Lock()

    def allow(self, addr):
        now = time.monotonic()
        with self._lock:
            hits = [t for t in self._hits.get(addr, ()) if now - t < self.window]
            if len(hits) >= self.count:
                self._hits[addr] = hits
                return False
            hits.append(now)
            self._hits[addr] = hits
            return True


class NetworkServer(ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, address, store, *, name='PSPC Network', registration=True, behind_proxy=False, log=print):
        super().__init__(address, Handler)
        self.store = store
        self.name = name
        self.registration = registration
        self.behind_proxy = behind_proxy  # client addresses come from the proxy's X-Forwarded-For
        self.log_line = log
        self.ticket_key = store.ticket_key()
        self.login_limit = RateLimiter(10, 60)

    def handle_error(self, request, client_address):
        # Clients drop idle keep-alive connections with a reset: not an error worth a traceback.
        if isinstance(sys.exc_info()[1], (ConnectionError, TimeoutError)):
            return
        super().handle_error(request, client_address)


def _ms():
    return int(time.time() * 1000)


class Handler(BaseHTTPRequestHandler):
    server_version = 'PSPCNetwork/' + __version__
    protocol_version = 'HTTP/1.1'
    timeout = 30  # seconds an idle keep-alive connection may stay open

    def log_message(self, fmt, *args):
        self.server.log_line('%s %s' % (self._client_ip(), fmt % args))

    def log_error(self, fmt, *args):
        if fmt.startswith('Request timed out'):
            return  # an idle keep-alive connection closing (see timeout): routine
        super().log_error(fmt, *args)

    # ---- plumbing

    def _client_ip(self):
        # Behind a reverse proxy every connection comes from the proxy; the player's address is the
        # last one the proxy appended to X-Forwarded-For (earlier entries are the client's to forge).
        headers = getattr(self, 'headers', None)  # not there before the request line is read
        if self.server.behind_proxy and headers is not None:
            forwarded = [a.strip() for a in (headers.get('X-Forwarded-For') or '').split(',') if a.strip()]
            if forwarded:
                return forwarded[-1]
        return self.client_address[0]

    def _send(self, status, payload=None):
        body = b'' if payload is None else json.dumps(payload).encode('utf-8')
        self.send_response(status)
        if payload is not None:
            self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Cache-Control', 'no-store')
        self.end_headers()
        if body and self.command != 'HEAD':
            self.wfile.write(body)

    def _error(self, status, code, message):
        self._send(status, {'error': code, 'message': message})

    def _read_body(self):
        # Read before answering, even when the answer is an error: closing a socket with unread
        # data resets the connection, and the client may lose the response.
        try:
            length = int(self.headers.get('Content-Length') or 0)
        except ValueError:
            length = 0
        if length > MAX_BODY:
            self.close_connection = True
            self._body = None
            return
        self._body = self.rfile.read(length) if length > 0 else b''

    def _json(self):
        if self._body is None:
            raise StoreError(413, 'too_large', 'Request too large.')
        raw = self._body
        if not raw:
            return {}
        try:
            data = json.loads(raw)
        except ValueError:
            raise StoreError(400, 'bad_json', 'The request body is not JSON.')
        if not isinstance(data, dict):
            raise StoreError(400, 'bad_json', 'The request body must be a JSON object.')
        return data

    def _account(self):
        auth = self.headers.get('Authorization', '')
        token = auth[7:].strip() if auth.lower().startswith('bearer ') else ''
        account = self.server.store.account_by_token(token)
        if not account:
            raise StoreError(401, 'not_signed_in', 'Sign in again.')
        return account, token

    def _profile(self, account):
        return {'online_id': account['online_id'], 'account_id': str(account['account_id']),
                'country': account['country'], 'domain': 'c9'}

    def _dispatch(self, method):
        path = self.path.split('?', 1)[0].rstrip('/')
        self._read_body()
        try:
            route = ROUTES.get((method, path))
            if route:
                return route(self)
            if path.startswith('/v1/friends/') and method in ('PUT', 'DELETE'):
                name = unquote(path[len('/v1/friends/'):])
                return (self.put_friend if method == 'PUT' else self.delete_friend)(name)
            self._error(404, 'not_found', 'No such endpoint.')
        except StoreError as e:
            self._error(e.status, e.code, e.message)
        except Exception as e:  # never leave a client hanging
            self.server.log_line('error handling %s %s: %r' % (method, path, e))
            self._error(500, 'internal', 'Server error.')

    def do_GET(self):
        self._dispatch('GET')

    def do_POST(self):
        self._dispatch('POST')

    def do_PUT(self):
        self._dispatch('PUT')

    def do_DELETE(self):
        self._dispatch('DELETE')

    # ---- endpoints

    def get_info(self):
        self._send(200, {'name': self.server.name, 'version': __version__, 'api': 1,
                         'registration': self.server.registration, 'ticket_cipher': 'PSPC'})

    def post_accounts(self):
        if not self.server.registration:
            raise StoreError(403, 'registration_closed', 'This server does not accept new accounts.')
        if not self.server.login_limit.allow(self._client_ip()):
            raise StoreError(429, 'slow_down', 'Too many attempts; wait a minute.')
        data = self._json()
        account = self.server.store.create_account(data.get('online_id'), data.get('password'),
                                                   data.get('country') or 'us')
        self.server.log_line('account created: %s' % account['online_id'])
        token = self.server.store.create_session(account)
        self._send(201, dict(self._profile(account), token=token))

    def post_sessions(self):
        if not self.server.login_limit.allow(self._client_ip()):
            raise StoreError(429, 'slow_down', 'Too many attempts; wait a minute.')
        data = self._json()
        account = self.server.store.check_password(data.get('online_id'), data.get('password'))
        token = self.server.store.create_session(account)
        self._send(200, dict(self._profile(account), token=token))

    def delete_session(self):
        _, token = self._account()
        self.server.store.delete_session(token)
        self._send(204)

    def get_me(self):
        account, _ = self._account()
        self._send(200, self._profile(account))

    def put_password(self):
        account, _ = self._account()
        data = self._json()
        self.server.store.check_password(account['online_id'], data.get('old_password'))
        self.server.store.set_password(account, data.get('new_password'))
        token = self.server.store.create_session(account)
        self._send(200, dict(self._profile(account), token=token))

    def post_tickets(self):
        account, _ = self._account()
        data = self._json()
        service_id = data.get('service_id') or ''
        if not isinstance(service_id, str) or len(service_id) > 24:
            raise StoreError(400, 'bad_service_id', 'The service ID is up to 24 characters.')
        now = _ms()
        blob = ticket.build(self.server.ticket_key, serial=secrets.token_bytes(20), issued_ms=now,
                            expires_ms=now + TICKET_LIFETIME_MS, account_id=account['account_id'],
                            online_id=account['online_id'], country=account['country'], service_id=service_id)
        self.server.log_line('ticket for %s (%s)' % (account['online_id'], service_id or 'no service id'))
        self._send(200, {'ticket': base64.b64encode(blob).decode('ascii'), 'expires_ms': now + TICKET_LIFETIME_MS})

    def post_tickets_verify(self):
        data = self._json()
        try:
            blob = base64.b64decode(data.get('ticket') or '', validate=True)
        except (ValueError, TypeError):
            raise StoreError(400, 'bad_ticket', 'The ticket is not base64.')
        fields, reason = ticket.verify(self.server.ticket_key, blob, _ms())
        result = {'valid': reason is None}
        if reason:
            result['reason'] = reason
        if fields:
            result.update(online_id=fields['online_id'], account_id=str(fields['account_id']),
                          service_id=fields['service_id'], country=fields['country'],
                          issued_ms=fields['issued_ms'], expires_ms=fields['expires_ms'])
            if reason is None:
                account = self.server.store.account_by_online_id(fields['online_id'])
                if not account or account['banned'] or account['account_id'] != fields['account_id']:
                    result = {'valid': False, 'reason': 'account suspended or gone'}
        self._send(200, result)

    def get_friends(self):
        account, _ = self._account()
        self._send(200, {'friends': self.server.store.friends(account)})

    def put_friend(self, name):
        account, _ = self._account()
        self._send(200, self.server.store.add_friend(account, name))

    def delete_friend(self, name):
        account, _ = self._account()
        self.server.store.remove_friend(account, name)
        self._send(204)


ROUTES = {
    ('GET', '/v1/info'): Handler.get_info,
    ('POST', '/v1/accounts'): Handler.post_accounts,
    ('POST', '/v1/sessions'): Handler.post_sessions,
    ('DELETE', '/v1/sessions/current'): Handler.delete_session,
    ('GET', '/v1/me'): Handler.get_me,
    ('PUT', '/v1/me/password'): Handler.put_password,
    ('POST', '/v1/tickets'): Handler.post_tickets,
    ('POST', '/v1/tickets/verify'): Handler.post_tickets_verify,
    ('GET', '/v1/friends'): Handler.get_friends,
}
