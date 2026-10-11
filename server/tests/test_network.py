# SPDX-License-Identifier: GPL-2.0-or-later
"""PSPC Network server tests: python -m unittest discover -s server/tests (from the repository)."""

import base64
import json
import os
import struct
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

from pspc_network import ticket  # noqa: E402
from pspc_network.api import NetworkServer  # noqa: E402
from pspc_network.store import Store  # noqa: E402


class TicketTests(unittest.TestCase):
    KEY = b'k' * 32

    def make(self, **kw):
        args = dict(serial=b's' * 20, issued_ms=1000, expires_ms=2000, account_id=0x1234567890ABCDEF,
                    online_id='Shadow-1', country='gb', service_id='UP9000-NPUG80114_00')
        args.update(kw)
        return ticket.build(self.KEY, **args)

    def test_layout(self):
        t = self.make()
        version, size = struct.unpack_from('>Ii', t, 0)
        self.assertEqual(version, 0x21010000)
        self.assertEqual(size, len(t) - 8)
        self.assertEqual(struct.unpack_from('>H', t, 8)[0], 0x3000)
        # First body parameter: the 20-byte serial id, type 8.
        self.assertEqual(struct.unpack_from('>HH', t, 12), (8, 20))
        f = ticket.parse(t)
        self.assertEqual(f['online_id'], 'Shadow-1')
        self.assertEqual(f['account_id'], 0x1234567890ABCDEF)
        self.assertEqual(f['country'], 'gb')
        self.assertEqual(f['service_id'], 'UP9000-NPUG80114_00')
        self.assertEqual(f['cipher'], b'PSPC')
        self.assertEqual(len(f['signature']), 32)

    def test_verify(self):
        t = self.make()
        self.assertIsNone(ticket.verify(self.KEY, t, 1500)[1])
        self.assertEqual(ticket.verify(self.KEY, t, 2000)[1], 'expired')
        self.assertEqual(ticket.verify(b'x' * 32, t, 1500)[1], 'bad signature')
        tampered = t.replace(b'Shadow-1', b'Shadow-2')
        self.assertEqual(ticket.verify(self.KEY, tampered, 1500)[1], 'bad signature')
        local = self.make(cipher=ticket.CIPHER_LOCAL)
        self.assertEqual(ticket.verify(self.KEY, local, 1500)[1], 'not issued by a PSPC Network server')
        self.assertTrue(ticket.verify(self.KEY, b'nonsense', 1500)[1].startswith('malformed'))


class ApiTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.store = Store(os.path.join(cls.tmp.name, 'np.sqlite'))
        cls.server = NetworkServer(('127.0.0.1', 0), cls.store, log=lambda line: None)
        cls.server.login_limit.count = 1000
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base = 'http://127.0.0.1:%d' % cls.server.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.store.close()
        cls.tmp.cleanup()

    def call(self, method, path, body=None, token=None):
        data = None if body is None else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        if data is not None:
            req.add_header('Content-Type', 'application/json')
        if token:
            req.add_header('Authorization', 'Bearer ' + token)
        try:
            with urllib.request.urlopen(req, timeout=10) as r:
                raw = r.read()
                return r.status, json.loads(raw) if raw else None
        except urllib.error.HTTPError as e:
            raw = e.read()
            return e.code, json.loads(raw) if raw else None

    def register(self, name, password='correct horse'):
        status, body = self.call('POST', '/v1/accounts', {'online_id': name, 'password': password})
        self.assertEqual(status, 201, body)
        return body['token']

    def test_info(self):
        status, body = self.call('GET', '/v1/info')
        self.assertEqual(status, 200)
        self.assertEqual(body['api'], 1)
        self.assertTrue(body['registration'])

    def test_accounts_and_sessions(self):
        token = self.register('Gabe_Logan')
        self.assertEqual(self.call('POST', '/v1/accounts', {'online_id': 'gabe_logan', 'password': 'whatever1'})[0], 409)
        self.assertEqual(self.call('POST', '/v1/accounts', {'online_id': '9lives', 'password': 'whatever1'})[0], 400)
        self.assertEqual(self.call('POST', '/v1/accounts', {'online_id': 'Teresa', 'password': 'short'})[0], 400)
        status, me = self.call('GET', '/v1/me', token=token)
        self.assertEqual((status, me['online_id'], me['country']), (200, 'Gabe_Logan', 'us'))
        self.assertEqual(self.call('POST', '/v1/sessions', {'online_id': 'GABE_LOGAN', 'password': 'nope nope'})[0], 401)
        status, s = self.call('POST', '/v1/sessions', {'online_id': 'GABE_LOGAN', 'password': 'correct horse'})
        self.assertEqual((status, s['online_id']), (200, 'Gabe_Logan'))
        self.assertEqual(self.call('DELETE', '/v1/sessions/current', token=s['token'])[0], 204)
        self.assertEqual(self.call('GET', '/v1/me', token=s['token'])[0], 401)
        self.assertEqual(self.call('GET', '/v1/me', token=token)[0], 200)  # the other session lives on
        self.assertEqual(self.call('GET', '/v1/me')[0], 401)

    def test_tickets(self):
        token = self.register('Lian_Xing')
        status, body = self.call('POST', '/v1/tickets', {'service_id': 'UP9000-NPUG80114_00'}, token=token)
        self.assertEqual(status, 200, body)
        blob = base64.b64decode(body['ticket'])
        f = ticket.parse(blob)
        self.assertEqual((f['online_id'], f['service_id'], f['cipher']), ('Lian_Xing', 'UP9000-NPUG80114_00', b'PSPC'))
        status, v = self.call('POST', '/v1/tickets/verify', {'ticket': body['ticket']})
        self.assertEqual((status, v['valid'], v['online_id']), (200, True, 'Lian_Xing'))
        bad = base64.b64encode(blob.replace(b'Lian_Xing', b'Lian_Xinh')).decode()
        self.assertFalse(self.call('POST', '/v1/tickets/verify', {'ticket': bad})[1]['valid'])
        self.assertEqual(self.call('POST', '/v1/tickets', {'service_id': 'x' * 25}, token=token)[0], 400)
        self.assertEqual(self.call('POST', '/v1/tickets', {}, token='bogus')[0], 401)
        # A suspended account's tickets stop verifying, and its sessions end.
        self.store.set_banned('Lian_Xing', True)
        self.assertFalse(self.call('POST', '/v1/tickets/verify', {'ticket': body['ticket']})[1]['valid'])
        self.assertEqual(self.call('GET', '/v1/me', token=token)[0], 401)
        self.assertEqual(self.call('POST', '/v1/sessions', {'online_id': 'Lian_Xing', 'password': 'correct horse'})[0], 403)
        self.store.set_banned('Lian_Xing', False)

    def test_friends(self):
        a = self.register('Mara_Aramov')
        b = self.register('Ellis')
        self.assertEqual(self.call('PUT', '/v1/friends/Ellis', token=a)[1]['status'], 'sent')
        self.assertEqual(self.call('GET', '/v1/friends', token=b)[1]['friends'],
                         [{'online_id': 'Mara_Aramov', 'status': 'received'}])
        self.assertEqual(self.call('PUT', '/v1/friends/mara_aramov', token=b)[1]['status'], 'friend')
        self.assertEqual(self.call('GET', '/v1/friends', token=a)[1]['friends'], [{'online_id': 'Ellis', 'status': 'friend'}])
        self.assertEqual(self.call('PUT', '/v1/friends/Nobody', token=a)[0], 404)
        self.assertEqual(self.call('PUT', '/v1/friends/Mara_Aramov', token=a)[0], 400)
        self.assertEqual(self.call('DELETE', '/v1/friends/Ellis', token=a)[0], 204)
        self.assertEqual(self.call('GET', '/v1/friends', token=b)[1]['friends'], [])

    def test_password_change(self):
        token = self.register('Imani_Gray')
        self.assertEqual(self.call('PUT', '/v1/me/password', {'old_password': 'wrong one', 'new_password': 'brand new pass'},
                                   token=token)[0], 401)
        status, body = self.call('PUT', '/v1/me/password', {'old_password': 'correct horse', 'new_password': 'brand new pass'},
                                 token=token)
        self.assertEqual(status, 200)
        self.assertEqual(self.call('GET', '/v1/me', token=token)[0], 401)  # old sessions ended
        self.assertEqual(self.call('GET', '/v1/me', token=body['token'])[0], 200)
        self.assertEqual(self.call('POST', '/v1/sessions', {'online_id': 'Imani_Gray', 'password': 'brand new pass'})[0], 200)

    def test_errors(self):
        self.assertEqual(self.call('GET', '/v1/nothing')[0], 404)
        req = urllib.request.Request(self.base + '/v1/sessions', data=b'{not json', method='POST')
        try:
            urllib.request.urlopen(req, timeout=10)
            self.fail('accepted bad JSON')
        except urllib.error.HTTPError as e:
            self.assertEqual(e.code, 400)


class RateLimitTests(unittest.TestCase):
    def attempt(self, server, forwarded=None):
        req = urllib.request.Request('http://127.0.0.1:%d/v1/sessions' % server.server_address[1], method='POST',
                                     data=json.dumps({'online_id': 'Nobody', 'password': 'wrong pass'}).encode())
        if forwarded:
            req.add_header('X-Forwarded-For', forwarded)
        try:
            urllib.request.urlopen(req, timeout=10)
        except urllib.error.HTTPError as e:
            e.read()
            return e.code
        return 200

    def run_server(self, **kw):
        tmp = tempfile.TemporaryDirectory()
        store = Store(os.path.join(tmp.name, 'np.sqlite'))
        server = NetworkServer(('127.0.0.1', 0), store, log=lambda line: None, **kw)
        server.login_limit.count = 2
        threading.Thread(target=server.serve_forever, daemon=True).start()

        def stop():
            server.shutdown()
            server.server_close()
            store.close()
            tmp.cleanup()
        self.addCleanup(stop)
        return server

    def test_limit_per_address(self):
        server = self.run_server()
        self.assertEqual([self.attempt(server, '1.1.1.1') for _ in range(3)], [401, 401, 429])
        self.assertEqual(self.attempt(server, '2.2.2.2'), 429)  # X-Forwarded-For isn't trusted by default

    def test_behind_proxy(self):
        server = self.run_server(behind_proxy=True)
        self.assertEqual([self.attempt(server, '1.1.1.1') for _ in range(3)], [401, 401, 429])
        self.assertEqual(self.attempt(server, '1.1.1.1, 2.2.2.2'), 401)  # the proxy's entry is the last one
        self.assertEqual(self.attempt(server, '2.2.2.2'), 401)
        self.assertEqual(self.attempt(server, '2.2.2.2'), 429)


class CommandTests(unittest.TestCase):
    """The admin commands the launcher's Online page runs: JSON lists, a password from stdin."""

    def run_main(self, args, stdin=''):
        import contextlib
        import io
        from pspc_network.__main__ import main
        out = io.StringIO()
        old = sys.stdin
        sys.stdin = io.StringIO(stdin)
        try:
            with contextlib.redirect_stdout(out):
                rc = main(['--db', self.db] + args)
        finally:
            sys.stdin = old
        return rc, out.getvalue()

    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.db = os.path.join(self.dir, 'np.sqlite')
        store = Store(self.db)
        store.create_account('Gabe_Logan', 'password1', 'us')
        store.close()

    def test_json_accounts_and_password(self):
        rc, out = self.run_main(['--json', 'accounts'])
        self.assertEqual(rc, 0)
        accounts = json.loads(out)
        self.assertEqual([(a['online_id'], a['banned']) for a in accounts], [('Gabe_Logan', False)])
        rc, _ = self.run_main(['passwd', 'Gabe_Logan', '--password-stdin'], 'new password 2\n')
        self.assertEqual(rc, 0)
        store = Store(self.db)
        try:
            self.assertTrue(store.check_password('Gabe_Logan', 'new password 2'))
        finally:
            store.close()
        # Too short: refused, nothing changes.
        self.assertEqual(self.run_main(['passwd', 'Gabe_Logan', '--password-stdin'], 'short\n')[0], 1)
        self.assertEqual(self.run_main(['ban', 'Gabe_Logan'])[0], 0)
        self.assertTrue(json.loads(self.run_main(['--json', 'accounts'])[1])[0]['banned'])


if __name__ == '__main__':
    unittest.main()
