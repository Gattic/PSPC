# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m pspc_network serve|accounts|ban|unban|passwd ... (see server/README.md)."""

import argparse
import getpass
import json
import ssl
import sys
import time

from . import __version__
from .api import NetworkServer
from .store import Store, StoreError


def _log(line):
    print('%s %s' % (time.strftime('%Y-%m-%d %H:%M:%S'), line), flush=True)


def serve(args):
    store = Store(args.db)
    host, _, port = args.listen.rpartition(':')
    server = NetworkServer((host or '0.0.0.0', int(port)), store, name=args.name,
                           registration=not args.no_registration, behind_proxy=args.behind_proxy, log=_log)
    scheme = 'http'
    if args.tls_cert:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.minimum_version = ssl.TLSVersion.TLSv1_2
        context.load_cert_chain(args.tls_cert, args.tls_key)
        server.socket = context.wrap_socket(server.socket, server_side=True)
        scheme = 'https'
    _log('PSPC Network %s on %s://%s:%d (database %s, registration %s)' % (
        __version__, scheme, host or '0.0.0.0', server.server_address[1], args.db,
        'closed' if args.no_registration else 'open'))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        store.close()


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m pspc_network', description=__doc__)
    ap.add_argument('--db', default='pspc_network.sqlite', help='database file (default: %(default)s)')
    ap.add_argument('--json', action='store_true', help='accounts: one JSON array (for the launcher)')
    sub = ap.add_subparsers(dest='command', required=True)
    s = sub.add_parser('serve', help='run the server')
    s.add_argument('--listen', default='0.0.0.0:8423', help='address:port (default: %(default)s)')
    s.add_argument('--name', default='PSPC Network', help='server name shown to players')
    s.add_argument('--no-registration', action='store_true', help="don't accept new accounts")
    s.add_argument('--tls-cert', help='serve HTTPS with this certificate chain (PEM)')
    s.add_argument('--tls-key', help='private key for --tls-cert (PEM)')
    s.add_argument('--behind-proxy', action='store_true',
                   help="behind a reverse proxy: take players' addresses from X-Forwarded-For")
    sub.add_parser('accounts', help='list accounts')
    for name in ('ban', 'unban', 'passwd'):
        p = sub.add_parser(name, help='%s an account' % name if name != 'passwd' else "set an account's password")
        p.add_argument('online_id')
        if name == 'passwd':
            p.add_argument('--password-stdin', action='store_true', help='read the new password from standard input')
    args = ap.parse_args(argv)
    if args.command == 'serve':
        if bool(args.tls_cert) != bool(args.tls_key):
            ap.error('--tls-cert and --tls-key go together')
        return serve(args)
    store = Store(args.db)
    try:
        if args.command == 'accounts' and args.json:
            print(json.dumps([{k: a[k] for k in ('online_id', 'account_id', 'country', 'created')} | {'banned': bool(a['banned'])}
                              for a in store.list_accounts()]))
        elif args.command == 'accounts':
            for a in store.list_accounts():
                print('%-16s %20d %s %s%s' % (a['online_id'], a['account_id'], a['country'],
                                               time.strftime('%Y-%m-%d', time.gmtime(a['created'])),
                                               '  BANNED' if a['banned'] else ''))
        elif args.command in ('ban', 'unban'):
            store.set_banned(args.online_id, args.command == 'ban')
            print('%s %sned' % (args.online_id, args.command))
        elif args.command == 'passwd':
            account = store.account_by_online_id(args.online_id)
            if not account:
                raise StoreError(404, 'no_such_account', 'No account with that online ID.')
            password = sys.stdin.readline().rstrip('\r\n') if args.password_stdin else getpass.getpass('New password: ')
            store.set_password(account, password)
            print('Password changed; existing sign-ins ended.')
    except StoreError as e:
        print(e.message, file=sys.stderr)
        return 1
    finally:
        store.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
