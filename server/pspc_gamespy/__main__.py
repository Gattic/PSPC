# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m pspc_gamespy [--db file] serve|accounts|ban|unban|passwd|games|stats ... (see docs/gamespy-server.md)."""

import argparse
import asyncio
import getpass
import ipaddress
import json
import logging
import os
import sys
import time

from . import __version__
from . import access
from . import crypt
from . import sessions as sessions_mod
from .games import GAMES, load as load_games
from .tunnel import VIRTUAL_HOST
from .gstats import decode_kv
from .server import DEFAULT_MOTD, Config, GameSpyServer
from .store import Store


GAMES_FILE = 'gamespy-games.json'


def serve(args):
    store = Store(args.db)
    log = logging.getLogger('gamespy')
    sessions = None
    path = args.sessions or os.path.join(os.path.dirname(os.path.abspath(args.db)), sessions_mod.SESSIONS_FILE)
    if args.sessions or os.path.exists(path):
        try:
            accounts, entries = sessions_mod.load(path)
        except (OSError, ValueError) as e:  # the server still runs, without them
            log.error("can't read the sessions in %s: %s", path, e)
        else:
            made = sessions_mod.make_accounts(store, accounts)
            log.info('sessions from %s: %s%s', path, ', '.join(s['name'] for s in entries) or 'none',
                     ' (%d accounts made)' % made if made else '')
            if entries:
                sessions = sessions_mod.Sessions(entries, os.path.join(os.path.dirname(os.path.abspath(args.db)), 'sessions'))
    if args.tunnel and args.bind != '0.0.0.0' and not ipaddress.ip_address(args.bind).is_loopback:
        log.error('--tunnel connects to the services on this machine: --bind has to be 0.0.0.0 or a 127.x.y.z address, not %s', args.bind)
        return 1
    public_host = args.public_host or (VIRTUAL_HOST if args.tunnel else '127.0.0.1')
    limits = access.Limits()
    if args.max_player_hosts is not None:
        limits.max_player_hosts = args.max_player_hosts
    config = Config(public_host=public_host, bind=args.bind, http_port=args.http_port, motd=args.motd,
                    status_file=args.status_file, sessions=sessions, tunnel=args.tunnel, behind_proxy=args.behind_proxy,
                    local_hosts_only=args.local_hosts_only, limits=limits)
    try:
        server = GameSpyServer(config, store)
    except OSError as e:
        log.error("can't look up the public host %s: %s", public_host, e)
        return 1
    if args.local_hosts_only and not args.tunnel and ipaddress.ip_address(server.reach.public_ip).is_loopback:
        log.error('a server for other PCs needs --public-host: the address or name they reach it at (games are listed with it)')
        return 1
    log.info('PSPC GameSpy server %s for %s; players on other PCs reach it at %s%s', __version__,
             ', '.join(g.name for g in GAMES.values()), public_host, ' (through the tunnel)' if args.tunnel else '')
    try:
        asyncio.run(server.serve_forever())
    except KeyboardInterrupt:
        pass
    except OSError as e:
        log.error("can't start: %s", e.strerror or e)
        return 1
    finally:
        if sessions:
            sessions.stop()
        store.close()
    return 0


def accounts(store, args):
    rows = store.list_profiles()
    if args.json:
        print(json.dumps([{'id': r['profileid'], 'name': r['nick'], 'uniquenick': r['uniquenick'], 'email': r['email'],
                           'created': r['created'], 'last_login': r['last_login'], 'banned': bool(r['banned'])}
                          for r in rows]))
        return 0
    for r in rows:
        print('%10d %-20s %-28s %s %s' % (r['profileid'], r['nick'], r['email'],
                                          time.strftime('%Y-%m-%d', time.gmtime(r['created'])),
                                          'BANNED' if r['banned'] else ''))
    return 0


def games(store, args):
    rows = store.q('SELECT g.*, p.nick, p.uniquenick FROM games g LEFT JOIN profiles p ON p.profileid = g.host_profileid '
                   'ORDER BY g.game_id DESC LIMIT 100')
    out = []
    for r in rows:
        data = decode_kv(r['data'].encode('latin-1', 'replace'))
        out.append({'id': r['game_id'], 'game': r['gamename'], 'host': r['host_profileid'],
                    'host_name': r['uniquenick'] or r['nick'] or '', 'map': data.get('mapname', ''), 'started': r['started'],
                    'finished': r['finished'], 'data': data})
    if args.json:
        print(json.dumps(out))
        return 0
    for g in out:
        print('%6d %-12s %s %-16s %-20s %s' % (g['id'], g['game'], time.strftime('%Y-%m-%d %H:%M', time.gmtime(g['started'])),
                                               g['host_name'], g['map'], 'finished' if g['finished'] else ''))
    return 0


def stats(store, args):
    rows = store.q('SELECT s.profileid, p.nick, s.gamename, s.key, s.value FROM player_stats s '
                   'LEFT JOIN profiles p ON p.profileid = s.profileid ORDER BY s.profileid, s.key')
    if args.json:
        out = {}
        for r in rows:
            out.setdefault(r['profileid'], {'id': r['profileid'], 'name': r['nick'] or '', 'game': r['gamename'],
                                            'stats': {}})['stats'][r['key']] = r['value']
        print(json.dumps(list(out.values())))
        return 0
    for r in rows:
        print('%10d %-20s %-12s %-16s %g' % (r['profileid'], r['nick'] or '?', r['gamename'], r['key'], r['value']))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m pspc_gamespy', description=__doc__)
    ap.add_argument('--db', default='pspc_gamespy.sqlite', help='database file (default: %(default)s)')
    ap.add_argument('--log', default='info', choices=('debug', 'info', 'warning'), help='log level')
    ap.add_argument('--log-file', help='also write the log here')
    ap.add_argument('--json', action='store_true', help='accounts, games and stats: one JSON array (for the launcher)')
    ap.add_argument('--games', action='append', metavar='FILE',
                    help='serve: more games, from a JSON file (default: %s beside the database, if there is one)' % GAMES_FILE)
    sub = ap.add_subparsers(dest='command', required=True)
    s = sub.add_parser('serve', help='run the servers')
    s.add_argument('--public-host',
                   help="the address or name players on other PCs reach this server at (default: 127.0.0.1; with --tunnel, %s). "
                        "A name must lead to this server (not a Cloudflare-proxied one)" % VIRTUAL_HOST)
    s.add_argument('--bind', default='0.0.0.0', help='address to listen on (default: %(default)s)')
    s.add_argument('--http-port', type=int, default=80, help='the web pages\' port (default: %(default)s; games ask port 80)')
    s.add_argument('--motd', default=DEFAULT_MOTD, help='the message of the day')
    s.add_argument('--status-file', help='keep a JSON summary of online players and hosted games here')
    s.add_argument('--local-hosts-only', action='store_true',
                   help="list only games hosted on this machine (its dedicated servers): for a server on the internet, where "
                        "UDP heartbeats can be forged")
    s.add_argument('--max-player-hosts', type=int, metavar='N', choices=range(0, 513),
                   help="list at most N games hosted by players at once (default: %d). The games on this machine (its dedicated "
                        "servers) don't count, and --local-hosts-only lists no others" % access.Limits().max_player_hosts)
    s.add_argument('--tunnel', action='store_true',
                   help='let players reach the server over WebSockets on the web port (/tunnel/), for a Cloudflare Tunnel')
    s.add_argument('--behind-proxy', action='store_true',
                   help="the web port's requests come through a proxy on this machine (cloudflared): take players' addresses from "
                        "its CF-Connecting-IP or X-Forwarded-For header")
    s.add_argument('--sessions', metavar='FILE',
                   help='programs to run alongside, such as dedicated game servers (default: %s beside the '
                        'database, if there is one)' % sessions_mod.SESSIONS_FILE)
    t = sub.add_parser('tunnel', help="run a player's relay to a public server (see relay.py)")
    t.add_argument('url', help="the server's address, such as https://swss.pspc.online")
    t.add_argument('--listen', default=VIRTUAL_HOST, help='the address to listen on for the game (default: %(default)s)')
    t.add_argument('--parent-pid', type=int, help='end when this process does')
    sub.add_parser('accounts', help='list accounts (GameSpy profiles)')
    for name in ('ban', 'unban'):
        p = sub.add_parser(name, help='%s the account owning a profile' % name)
        p.add_argument('name', help='the nick or unique nick')
    p = sub.add_parser('passwd', help="set the password of the account owning a profile")
    p.add_argument('name', help='the nick, unique nick or e-mail address')
    p.add_argument('--password-stdin', action='store_true', help='read the new password from standard input')
    sub.add_parser('games', help='the last reported games')
    sub.add_parser('stats', help="players' totals from reported games")
    args = ap.parse_args(argv)
    handlers = [logging.StreamHandler(sys.stdout)]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding='utf-8'))
    logging.basicConfig(level=getattr(logging, args.log.upper()), handlers=handlers,
                        format='%(asctime)s %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
    if args.command == 'tunnel':
        from . import relay
        return relay.main(args)
    if args.command == 'serve':
        files = args.games or [os.path.join(os.path.dirname(os.path.abspath(args.db)), GAMES_FILE)]
        for f in files:
            if args.games or os.path.exists(f):
                try:
                    added = load_games(f)
                except (OSError, ValueError) as e:  # the built-in games are still served
                    logging.getLogger('gamespy').error("can't read the games in %s: %s", f, e)
                    continue
                logging.getLogger('gamespy').info('games from %s: %s', f, ', '.join(g.name for g in added) or 'none')
        return serve(args)
    store = Store(args.db)
    try:
        if args.command == 'accounts':
            return accounts(store, args)
        if args.command == 'games':
            return games(store, args)
        if args.command == 'stats':
            return stats(store, args)
        if args.command in ('ban', 'unban'):
            n = store.set_banned(args.name, args.command == 'ban')
            if not n:
                print('no profile named %s' % args.name)
                return 1
            print('%s %sned' % (args.name, args.command))
            return 0
        rows = store.q('SELECT DISTINCT u.userid FROM users u JOIN profiles p ON p.userid = u.userid '
                       'WHERE (p.nick = ? OR p.uniquenick = ? OR u.email = ?) AND p.deleted = 0',
                       (args.name, args.name, args.name))
        if not rows:
            print('no account for %s' % args.name)
            return 1
        if len(rows) > 1:
            print('%s names %d accounts; use the e-mail address or unique nick' % (args.name, len(rows)))
            return 1
        password = sys.stdin.readline().rstrip('\r\n') if args.password_stdin else getpass.getpass('New password: ')
        if not password:
            print('the password is empty')
            return 2
        store.set_password(rows[0]['userid'], crypt.md5hex(password))
        print('password set for %s' % args.name)
        return 0
    finally:
        store.close()


if __name__ == '__main__':
    sys.exit(main())
