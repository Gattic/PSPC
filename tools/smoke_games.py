#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot every game in Games/ headless and report how far it gets.

Usage: smoke_games.py [--pspc PATH] [--games DIR] [--seconds N] [--interp] [--jobs N]
                      [--out DIR] [filter ...]

Each game runs with --fast-time and --dump-frames. Per-game scripted input comes from
tools/smoke_inputs.txt ("<disc id or file stem> <vblank>:<BUTTONS>[:<frames>] ..."). The report
lists the exit status (crash / timeout / exit), the last frame number reached, how many distinct
frames were seen (a frozen screen shows up as 1-2), and the first lines of any crash backtrace.
Frame dumps stay in --out/<game>/ for inspection.
"""
import argparse
import concurrent.futures
import hashlib
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
GAME_EXTS = {'.iso', '.cso', '.zso', '.dax', '.pbp'}


def load_inputs(path):
    table = {}
    if not path.exists():
        return table
    for line in path.read_text(encoding='utf-8').splitlines():
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split()
        table[parts[0].upper()] = parts[1:]
    return table


def disc_id(pspc_dir, game):
    if game.suffix.lower() == '.pbp':
        return ''
    r = subprocess.run([str(pspc_dir / 'pspc-iso.exe'), 'info', str(game)], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True, errors='replace')
    for line in r.stdout.splitlines():
        if line.startswith('DISC_ID='):
            return line.split('=', 1)[1].strip()
    return ''


def run_one(args, game, presses):
    out = pathlib.Path(args.out) / game.stem
    out.mkdir(parents=True, exist_ok=True)
    for f in out.glob('frame_*.bmp'):
        f.unlink()
    cmd = [args.pspc, '--config', 'none', '--no-plugins', '--headless', '--fast-time', '--timeout', str(args.seconds), '--log', 'warn',
           '--dump-frames', str(out), '--dump-every', str(args.every)]
    if args.interp:
        cmd.append('--interp')
    for p in presses:
        cmd += ['--press', p]
    cmd.append(str(game))
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.seconds + 30)
        code, err = r.returncode, r.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        code, err = None, 'host timeout'
    frames = sorted(out.glob('frame_*.bmp'))
    distinct = len({hashlib.sha1(f.read_bytes()).hexdigest() for f in frames})
    if code == 124:
        status = 'ran'
    elif code == 0:
        status = 'exited'
    elif code is None:
        status = 'HUNG'
    else:
        status = 'CRASH %08x' % (code & 0xFFFFFFFF)
    crash = [l for l in err.splitlines() if l.startswith('[FATAL]') or l.lstrip().startswith('#')][:8]
    return {'game': game.name, 'status': status, 'last': frames[-1].stem if frames else '-',
            'frames': len(frames), 'distinct': distinct, 'crash': crash}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / 'pspc.exe'))
    ap.add_argument('--games', default=str(ROOT / 'Games'))
    ap.add_argument('--seconds', type=int, default=45)
    ap.add_argument('--every', type=int, default=300, help='dump a frame every N VBlanks')
    ap.add_argument('--interp', action='store_true')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--out', default=str(ROOT / 'build' / 'smoke'))
    ap.add_argument('filters', nargs='*')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='backslashreplace')

    pspc_dir = pathlib.Path(args.pspc).parent
    inputs = load_inputs(ROOT / 'tools' / 'smoke_inputs.txt')
    games = [g for g in sorted(pathlib.Path(args.games).rglob('*'))
             if g.is_file() and g.suffix.lower() in GAME_EXTS
             and (not args.filters or any(f.lower() in g.name.lower() for f in args.filters))]
    if not games:
        print('no games found in %s' % args.games)
        return 1
    jobs = []
    for g in games:
        did = disc_id(pspc_dir, g).upper()
        presses = inputs.get(did) or inputs.get(g.stem.upper()) or []
        jobs.append((g, presses))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(lambda j: run_one(args, *j), jobs))
    bad = 0
    for r in results:
        print('%-32s %-16s last=%-14s frames=%-3d distinct=%d' % (r['game'], r['status'], r['last'], r['frames'],
                                                                    r['distinct']))
        for l in r['crash']:
            print('    ' + l[:150])
        if r['status'].startswith(('CRASH', 'HUNG')) or r['distinct'] <= 1:
            bad += 1
    print('\n%d game(s), %d with problems; frames in %s' % (len(results), bad, args.out))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
