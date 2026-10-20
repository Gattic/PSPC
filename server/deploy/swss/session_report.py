#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""What each client of the SWSS server did and where it stopped, from the server's log.

    python3 session_report.py                      # the service's journal since it last started
    python3 session_report.py --since "-30min"     # any journalctl --since value
    python3 session_report.py --log server.log     # a saved log (journalctl -o cat, or the server's own output)
    python3 session_report.py --ip 203.0.113.9     # one client's timeline only

For every client address it lists the events in order and the stages reached - an account, a login, the
server list, a game listed, NAT negotiation, the stats server, the web pages - then names what looks wrong
(a refused login, a stats sign-in from another address than Presence's, a NAT negotiation whose partner
never came, a player who tried to host, ...). The server logs at info level; nothing here changes it."""

import argparse
import collections
import re
import subprocess
import sys

LINE = re.compile(r'^(\d{4}-\d\d-\d\d \d\d:\d\d:\d\d) (.*)$')
ADDR = r"\(?'?(\d+\.\d+\.\d+\.\d+)"      # an address as the log writes it: 1.2.3.4, 1.2.3.4:5 or ('1.2.3.4', 5)

# (stage, regular expression whose first group is the client's address). Order matters: the first match wins.
EVENTS = [
    ('account', r'gpcm: new account .* from (\S+)$'),
    ('account', r'gpsp: new account .* from (\S+)$'),
    ('account refused', r'gpcm ' + ADDR + r".*new account .* refused"),
    ('login', r'gpcm: .* logged in \(profile \d+\) from (\S+)$'),
    ('login refused', r'gpcm ' + ADDR + r'.*login refused'),
    ('rate limited', r'gpcm ' + ADDR + r'.*too many'),
    ('rate limited', r'(?:sb|gstats|gpsp) ' + ADDR + r'.*too many'),
    ('rate limited', r'.*: too many connections \((\d+\.\d+\.\d+\.\d+) has'),
    ('game available?', r'qr: ' + ADDR + r'.* asks whether (\S+) is available'),
    ('host ignored', r'qr: heartbeat from ' + ADDR + r'.* ignored'),
    ('game listed', r'qr: \S+ hosts .* on ' + ADDR + r'.*UDP ports'),
    ('game unlisted', r'qr: .* \(' + ADDR + r'.*is no longer listed'),
    ('unknown game', r'qr: heartbeat from ' + ADDR + r'.* no key for'),
    ('unknown game', r'gstats ' + ADDR + r".*auth for .* doesn't know"),
    ('server list', r'sb ' + ADDR + r'.*: \S+ list, (\d+) of'),
    ('message to host', r'sb ' + ADDR + r'.*message for'),
    ('NAT: joined', r'natneg \w+: client \d+ joined from ' + ADDR),
    ('NAT: told to connect', r'natneg \w+: client \d+ \(' + ADDR + r'.*told to connect'),
    ('NAT: partner never came', r'natneg \w+: the partner never came \(here: client \d+ at ' + ADDR),
    ('NAT: failure reported', r'natneg \w+: client \d+ reports failure'),
    ('stats refused', r'gstats ' + ADDR + r'.*sign-in .*refused'),
    ('stats sign-in', r'gstats ' + ADDR + r'.* signed in to'),
    ('stats game', r'gstats ' + ADDR + r'.*game \d+ (?:started|finished)'),
    ('web', r'web ' + ADDR + r'.*->'),
    ('web 404', r'web ' + ADDR + r'.*no such page'),
    ('closed', r'(?:gpcm|gpsp|gstats|sb) ' + ADDR + r'.*(?:closed|not handled)'),
]
EVENTS = [(stage, re.compile(rx)) for stage, rx in EVENTS]

# What a complete visit has, in the order a game does it.
STAGES = ['login', 'game available?', 'server list', 'stats sign-in', 'web']

PROBLEMS = {
    'account refused': 'an account was refused: see its line for the reason (a bad name or e-mail, or too many accounts from this address)',
    'login refused': 'a login was refused: a wrong password or unknown account (see the code in the line); a game that shows "cannot log in" ends here',
    'rate limited': 'this address hit a limit (failed logins, messages or connections): wait a few minutes, or look for a program retrying in a loop',
    'host ignored': 'this address tried to list a game and the server ignored it: only this machine (its dedicated servers) may host on this server',
    'unknown game': 'a game name this server has no key for: the disc is not the SWSS build the server expects, or it is a different game',
    'stats refused': 'the stats sign-in was refused: the player must be logged in to Presence from the same address; a client behind carrier-grade NAT '
                     'or two internet connections can reach the two servers from different addresses',
    'NAT: partner never came': 'NAT negotiation waited 30 seconds for the other side: the other player never contacted UDP 27901 '
                               '(their redirect, their router, or they gave up)',
    'NAT: failure reported': 'a game reported that NAT negotiation failed: the two sides could not reach each other (the log line has the '
                             'cookie; the "told to connect" lines with the same cookie say what each was told)',
    'web 404': 'a web page the server does not have was asked for',
    'closed': 'a connection was ended by the server (a limit, a timeout or an unreadable message)',
}


def parse(lines):
    """[(time, address, stage, text)] for the lines that are about a client, in order."""
    events, warnings = [], []
    for line in lines:
        m = LINE.match(line.rstrip('\n'))
        if not m:
            continue
        when, text = m.groups()
        if 'not handled' in text or 'Traceback' in text:
            warnings.append((when, text))
        for stage, rx in EVENTS:
            hit = rx.search(text)
            if hit:
                address = next((g for g in hit.groups() if g and '.' in g), '(no address in the line)')
                events.append((when, address, stage, text))
                break
    return events, warnings


def report(events, warnings, only=None, out=sys.stdout):
    by_address = collections.OrderedDict()
    for when, address, stage, text in events:
        by_address.setdefault(address, []).append((when, stage, text))
    hosting = [e for e in events if e[2] == 'game listed']
    for address, items in by_address.items():
        if only and address != only:
            continue
        stages = {stage for _, stage, _ in items}
        print('=== %s: %d events, %s to %s' % (address, len(items), items[0][0], items[-1][0]), file=out)
        for when, stage, text in items:
            print('  %s  %-24s %s' % (when[11:], stage, text[:150]), file=out)
        if 'game listed' not in stages and address[0] != '(':                 # a dedicated game is a host, not a player
            print('  stages: ' + ' '.join('[%s] %s' % ('x' if s in stages else ' ', s) for s in STAGES), file=out)
        lists = [e for e in items if e[1] == 'server list']
        if lists and not any(re.search(r'list, [1-9]\d* of', e[2]) for e in lists):
            print('  ! the server list was empty every time: no game was being hosted (nothing for this client to join)', file=out)
        for stage in PROBLEMS:
            if stage in stages:
                print('  ! ' + PROBLEMS[stage], file=out)
        if address[0] != '(' and 'login' in stages and 'stats sign-in' not in stages and 'stats refused' not in stages:
            print('  ? logged in but never reached the stats server (UDP/TCP 29920 blocked on its side, or the game has not got that far)', file=out)
        if 'NAT: joined' in stages and 'NAT: told to connect' not in stages and 'NAT: partner never came' not in stages:
            print('  ? NAT negotiation started and is waiting for the partner', file=out)
        print(file=out)
    print('games listed in this log: %s' % (', '.join('%s %s' % (e[0][11:], e[3][:90]) for e in hosting) or 'none'), file=out)
    if warnings:
        print('warnings the server logged (%d):' % len(warnings), file=out)
        for when, text in warnings[:10]:
            print('  %s %s' % (when[11:], text[:160]), file=out)
    return by_address


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--log', help='read this file instead of the journal')
    ap.add_argument('--since', help="journalctl --since value (default: since the service last started)")
    ap.add_argument('--unit', default='swss-server', help='the user service to read (default: %(default)s)')
    ap.add_argument('--ip', help="only this client's address")
    args = ap.parse_args(argv)
    if args.log:
        with open(args.log, encoding='utf-8', errors='replace') as f:
            lines = f.readlines()
    else:
        since = args.since or subprocess.run(['systemctl', '--user', 'show', args.unit, '-p', 'ActiveEnterTimestamp', '--value'],
                                             capture_output=True, text=True).stdout.strip().rsplit(' ', 1)[0]
        lines = subprocess.run(['journalctl', '--user', '-u', args.unit, '--no-pager', '-o', 'cat', '--since', since],
                               capture_output=True, text=True).stdout.splitlines()
    events, warnings = parse(lines)
    if not events:
        print('no client activity in the log')
        return 0
    report(events, warnings, args.ip)
    return 0


if __name__ == '__main__':
    sys.exit(main())
