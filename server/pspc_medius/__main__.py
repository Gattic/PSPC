# SPDX-License-Identifier: GPL-2.0-or-later
"""python -m pspc_medius [--db file] serve|accounts|ban|unban|missions|news|board ... (see docs/combat-ops-server.md)."""

import argparse
import asyncio
import json
import logging
import sys
import time

from . import __version__
from . import umd
from .games import GAMES
from .server import Config, MediusServer
from .store import Store
from .tickets import TicketVerifier


def import_official(store, path):
    """Records the missions on a Combat Ops UMD (ISO or CSO image, or extracted folder) as official ones."""
    missions = umd.official_missions(path)
    for name, info in missions:
        mission_id = store.add_mission(info, official=True)
        logging.getLogger('medius').info('official mission %d: %s (%s, %s)', mission_id, info['name'], name,
                                         info['location'])
    return len(missions)


def serve(args):
    game = GAMES[args.game]()
    store = Store(args.db)
    if args.umd:
        n = import_official(store, args.umd)
        logging.getLogger('medius').info('%d official missions from %s', n, args.umd)
    config = Config(public_host=args.public_host, bind=args.bind, allow_password_login=args.allow_password_login)
    tickets = TicketVerifier(args.network, allow_local=args.allow_local or not args.network,
                             service_ids=game.service_ids)
    server = MediusServer(config, game, store, tickets)
    logging.getLogger('medius').info(
        'PSPC game server %s for %s: clients connect to %s; tickets checked by %s%s', __version__, game.name,
        args.public_host, args.network or 'nobody (local sign-ins)', ', local sign-ins allowed' if tickets.allow_local and args.network else '')
    try:
        asyncio.run(server.serve_forever())
    except KeyboardInterrupt:
        pass
    finally:
        store.close()


def missions(store, args):
    if args.action == 'list' and args.json:
        rows = store.q('SELECT m.*, a.name AS uploader FROM missions m LEFT JOIN accounts a ON a.account_id = m.account_id '
                       'ORDER BY m.mission_id')
        print(json.dumps([{'id': r['mission_id'], 'name': r['name'], 'author': r['author'] or r['uploader'] or '',
                           'downloads': r['downloads'], 'plays': r['plays'], 'official': bool(r['official']),
                           'hidden': bool(r['hidden'])} for r in rows]))
        return 0
    if args.action == 'list':
        for r in store.q('SELECT m.*, a.name AS uploader FROM missions m LEFT JOIN accounts a ON a.account_id = m.account_id '
                         'ORDER BY m.mission_id'):
            flags = ' '.join(f for f, on in (('official', r['official']), ('hidden', r['hidden'])) if on)
            print('%5d %-24s by %-16s %5d downloads %5d plays  %s' % (
                r['mission_id'], r['name'], r['author'] or r['uploader'] or '?', r['downloads'], r['plays'], flags))
        return 0
    if not args.target:
        print('missions %s needs %s' % (args.action, 'a UMD path' if args.action == 'import' else 'a mission id'))
        return 2
    if args.action == 'import':
        print('%d official missions imported' % import_official(store, args.target))
        return 0
    try:
        mission_id = int(args.target)
    except ValueError:
        print('not a mission id: %s' % args.target)
        return 2
    row = store.one('SELECT * FROM missions WHERE mission_id = ?', (mission_id,))
    if row is None:
        print('no mission %d' % mission_id)
        return 1
    if args.action == 'delete':
        store.x('DELETE FROM missions WHERE mission_id = ?', (mission_id,))
        store.x('DELETE FROM mission_lists WHERE mission_id = ?', (mission_id,))
        store.x('DELETE FROM mission_votes WHERE mission_id = ?', (mission_id,))
        store.x('DELETE FROM blobs WHERE file_id = ?', (row['file_id'],))
    else:
        store.x('UPDATE missions SET hidden = ? WHERE mission_id = ?', (1 if args.action == 'hide' else 0, mission_id))
    print('mission %d (%s): %s' % (mission_id, row['name'], {'delete': 'deleted', 'hide': 'hidden', 'unhide': 'shown'}[args.action]))
    return 0


def news(store, args):
    if args.action == 'list' and args.json:
        print(json.dumps([{'id': r['news_id'], 'created': r['created'], 'text': r['text']} for r in store.news(100)]))
    elif args.action == 'list':
        for r in store.news(100):
            print('%5d %s  %s' % (r['news_id'], time.strftime('%Y-%m-%d', time.gmtime(r['created'])), r['text']))
    elif args.action == 'add':
        text = ' '.join(args.rest).strip()
        if not text:
            print('news add needs the text')
            return 2
        print('news item %d added' % store.add_news(text))
    else:
        if not args.rest or not args.rest[0].isdigit():
            print('news delete needs an id')
            return 2
        store.delete_news(int(args.rest[0]))
        print('news item %s deleted' % args.rest[0])
    return 0


def board(store, args):
    if args.action == 'list':
        rows = store.q('SELECT * FROM board_posts ORDER BY post_id DESC LIMIT 50')
        if args.json:
            print(json.dumps([{'id': r['post_id'], 'board': r['board'], 'author': r['author'], 'reply_to': r['reply_to'],
                               'subject': r['subject'] or '', 'body': r['body'], 'created': r['created'],
                               'hidden': bool(r['hidden'])} for r in rows]))
            return 0
        for r in rows:
            print('%5d %-10s %-16s %s%s%s' % (r['post_id'], r['board'], r['author'], 're %d: ' % r['reply_to'] if r['reply_to'] else '',
                                             r['subject'] or r['body'][:40], '  (hidden)' if r['hidden'] else ''))
        return 0
    if not args.rest or not args.rest[0].isdigit():
        print('board %s needs a post id' % args.action)
        return 2
    store.set_post_hidden(int(args.rest[0]), args.action == 'hide')
    print('post %s %s' % (args.rest[0], 'hidden' if args.action == 'hide' else 'shown'))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(prog='python -m pspc_medius', description=__doc__)
    ap.add_argument('--db', default='pspc_medius.sqlite', help='database file (default: %(default)s)')
    ap.add_argument('--log', default='info', choices=('debug', 'info', 'warning'), help='log level')
    ap.add_argument('--log-file', help='also write the log here')
    ap.add_argument('--json', action='store_true', help='accounts and the list actions: one JSON array (for the launcher)')
    sub = ap.add_subparsers(dest='command', required=True)
    s = sub.add_parser('serve', help='run the game server')
    s.add_argument('--game', default='sfco', choices=sorted(GAMES), help='which game (default: %(default)s)')
    s.add_argument('--public-host', default='127.0.0.1',
                   help="the address players' consoles reach this server at (default: %(default)s)")
    s.add_argument('--bind', default='0.0.0.0', help='address to listen on (default: %(default)s)')
    s.add_argument('--network', help='PSPC Network server that checks login tickets, e.g. https://np.example.org')
    s.add_argument('--allow-local', action='store_true',
                   help='also let in players who sign in locally (no PSPC Network account); their names are not checked')
    s.add_argument('--allow-password-login', action='store_true', help='accept Medius name logins (no ticket)')
    s.add_argument('--umd', help="the game's UMD (an ISO or CSO image, or an extracted folder): serve its missions as the "
                   'official ones')
    for name in ('accounts', 'ban', 'unban'):
        p = sub.add_parser(name, help={'accounts': 'list accounts', 'ban': 'ban an account', 'unban': 'unban an account'}[name])
        p.add_argument('--game', default='sfco', choices=sorted(GAMES))
        if name != 'accounts':
            p.add_argument('name')
    m = sub.add_parser('missions', help='list and manage shared missions')
    m.add_argument('action', choices=('list', 'import', 'hide', 'unhide', 'delete'),
                   help="list; import <UMD>: add the game's own missions as official ones; hide/unhide/delete <id>")
    m.add_argument('target', nargs='?', help='the UMD (ISO or CSO image, or folder) for import; a mission id otherwise')
    n = sub.add_parser('news', help='the news shown on the Community pages')
    n.add_argument('action', choices=('list', 'add', 'delete'))
    n.add_argument('rest', nargs='*', help='add: the text; delete: the id')
    b = sub.add_parser('board', help='moderate the message board')
    b.add_argument('action', choices=('list', 'hide', 'unhide'))
    b.add_argument('rest', nargs='*', help='hide/unhide: the post id (hiding a topic hides its replies)')
    args = ap.parse_args(argv)
    handlers = [logging.StreamHandler(sys.stdout)]
    if args.log_file:
        handlers.append(logging.FileHandler(args.log_file, encoding='utf-8'))
    logging.basicConfig(level=getattr(logging, args.log.upper()), handlers=handlers,
                        format='%(asctime)s %(message)s', datefmt='%Y-%m-%d %H:%M:%S')
    if args.command == 'serve':
        return serve(args)
    store = Store(args.db)
    try:
        if args.command == 'missions':
            return missions(store, args)
        if args.command == 'news':
            return news(store, args)
        if args.command == 'board':
            return board(store, args)
        app_id = GAMES[args.game].app_ids[0]
        if args.command == 'accounts' and args.json:
            print(json.dumps([{'id': a['account_id'], 'name': a['name'], 'created': a['created'], 'banned': bool(a['banned'])}
                              for a in store.q('SELECT * FROM accounts WHERE app_id = ? ORDER BY account_id', (app_id,))]))
        elif args.command == 'accounts':
            for a in store.q('SELECT * FROM accounts WHERE app_id = ? ORDER BY account_id', (app_id,)):
                print('%6d %-16s %s %s' % (a['account_id'], a['name'], time.strftime('%Y-%m-%d', time.gmtime(a['created'])),
                                           'BANNED' if a['banned'] else ''))
        else:
            store.set_banned(app_id, args.name, args.command == 'ban')
            print('%s %sned' % (args.name, args.command))
    finally:
        store.close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
