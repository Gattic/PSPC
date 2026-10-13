#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Boot every game in Games/ headless and check how far it gets.

Usage: smoke_games.py [--pspc PATH] [--flash0 DIR] [--aot-dir DIR] [--games DIR] [--interp]
                      [--jobs N] [--out DIR] [--refs DIR] [--record] [--seconds N] [filter ...]

Each game runs with --fast-time and --dump-frames, with its scripted input from
tools/smoke_inputs.txt ("<disc id or file stem> <vblank>:<BUTTONS>[:<frames>] ... [osk=<text>]").

Games with checkpoints in tools/checkpoints.txt run until their last checkpoint's VBlank (pspc
--exit-vblank), so a run ends at the same point in game time however busy the machine is, and the
frames at the checkpoints are compared with references recorded on this machine: --record writes
them to --refs/<aot|interp>/<game>/ (game frames stay out of the repository), with PNG copies to
look at. A checkpoint passes when its frame is identical to the reference or, for one marked ~N,
when at most N% of its pixels differ (a screen that animates differently from run to run). Games
without checkpoints run --seconds of wall time and only report how far they got.

pspc always gets a firmware flash0 (fonts, modules): --flash0, else the one next to pspc.exe if it
has files, else build/RelWithDebInfo's. Games take other paths without one, so the references note
the flash0 they were made with and a check with another one says so.

The report lists each game's exit status (crash / timeout / exit), the last frame dumped, how many
distinct frames there were (a frozen screen shows up as 1-2), the checkpoints, and the first lines
of any crash backtrace. Frame dumps stay in --out/<game>/. Exit status 1 when a game crashed, hung
or froze, or missed or changed at a checkpoint.
"""
import argparse
import concurrent.futures
import datetime
import hashlib
import json
import math
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from functools import reduce

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE_SUFFIX = '.exe' if sys.platform == 'win32' else ''
GAME_EXTS = {'.iso', '.cso', '.zso', '.dax', '.pbp'}
RTC_BASE = 1767225600  # 2026-01-01 00:00:00 UTC


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


def load_checkpoints(path):
    """{KEY: [(vblank, tolerance %, label), ...]} sorted by VBlank."""
    table = {}
    if not path.exists():
        return table
    for n, line in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if len(parts) < 2:
            raise ValueError('%s:%d: expected <game> <vblank>[~<percent>] <label>' % (path, n))
        vblank, _, tol = parts[1].partition('~')
        table.setdefault(parts[0].upper(), []).append((int(vblank), float(tol) if tol else 0.0,
                                                       parts[2] if len(parts) > 2 else ''))
    for points in table.values():
        points.sort()
    return table


def disc_id(pspc_dir, game):
    if game.suffix.lower() == '.pbp':
        return ''
    r = subprocess.run([str(pspc_dir / ('pspc-iso' + EXE_SUFFIX)), 'info', str(game)], stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, text=True, errors='replace')
    for line in r.stdout.splitlines():
        if line.startswith('DISC_ID='):
            return line.split('=', 1)[1].strip()
    return ''


def find_flash0(arg, pspc_dir):
    if arg:
        return pathlib.Path(arg).resolve()
    for d in (pspc_dir / 'flash0', ROOT / 'build' / 'RelWithDebInfo' / 'bin' / 'flash0'):
        if d.is_dir() and any(d.iterdir()):
            return d.resolve()
    return None


def read_bmp(path):
    """(width, height, rows): 24/32-bit uncompressed BMP, rows top to bottom without padding."""
    d = path.read_bytes()
    off = struct.unpack_from('<I', d, 10)[0]
    w, h = struct.unpack_from('<ii', d, 18)
    step = struct.unpack_from('<H', d, 28)[0] // 8
    stride = (w * step + 3) & ~3
    rows = [d[off + y * stride: off + y * stride + w * step] for y in range(abs(h))]
    if h > 0:
        rows.reverse()
    return w, abs(h), step, rows


def differing_share(a, b):
    """Share of pixels (0-1) that differ between two frames; 1 if their sizes differ."""
    wa, ha, sa, ra = read_bmp(a)
    wb, hb, sb, rb = read_bmp(b)
    if (wa, ha, sa) != (wb, hb, sb):
        return 1.0
    diff = 0
    for x, y in zip(ra, rb):
        if x != y:
            diff += sum(1 for i in range(0, len(x), sa) if x[i:i + 3] != y[i:i + 3])
    return diff / float(wa * ha)


def write_png(bmp, png):
    w, h, step, rows = read_bmp(bmp)
    raw = b''.join(b'\0' + bytes(c for i in range(0, len(r), step) for c in (r[i + 2], r[i + 1], r[i]))
                   for r in rows)

    def chunk(t, data):
        return struct.pack('>I', len(data)) + t + data + struct.pack('>I', zlib.crc32(t + data) & 0xFFFFFFFF)
    png.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def frame_name(vblank):
    return 'frame_%06d.bmp' % vblank


def run_one(args, game, presses, points, flash0):
    out = pathlib.Path(args.out) / game.stem
    out.mkdir(parents=True, exist_ok=True)
    for f in out.glob('frame_*.bmp'):
        f.unlink()
    # Networking off (a game that starts it makes PSPC keep real time) and a fixed boot time (games
    # seed their random numbers from the clock): the same input then gives the same frames.
    # A fresh memory stick: no saves from earlier runs or from playing (which would change the
    # path), and the run's own saves don't land in pspc's ms0.
    ms = tempfile.mkdtemp(prefix='pspc_smoke_ms_')
    cmd = [args.pspc, '--config', 'none', '--set', 'network=0', '--rtc-base', str(RTC_BASE), '--no-plugins',
           '--headless', '--fast-time', '--log', 'warn', '--memstick', ms, '--dump-frames', str(out)]
    if points:
        # Every checkpoint gets a dump; the run ends at the last one.
        every = reduce(math.gcd, [vb for vb, _, _ in points])
        seconds = args.max_seconds
        cmd += ['--dump-every', str(every), '--exit-vblank', str(points[-1][0])]
    else:
        seconds = args.seconds
        cmd += ['--dump-every', str(args.every)]
    cmd += ['--timeout', str(seconds)]
    if flash0:
        cmd += ['--flash0', str(flash0)]
    if args.aot_dir:
        cmd += ['--aot-dir', args.aot_dir]
    if args.interp:
        cmd.append('--interp')
    for p in presses:
        # osk=<text>: what the system keyboard returns (a|b|c for successive keyboards).
        cmd += ['--osk-text', p[4:]] if p.startswith('osk=') else ['--press', p]
    cmd.append(str(game))
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=seconds + 30)
        code, err = r.returncode, r.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        code, err = None, 'host timeout'
    finally:
        shutil.rmtree(ms, ignore_errors=True)
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
    if points and status == 'ran' and not (out / frame_name(points[-1][0])).exists():
        status = 'TOO SLOW'  # the wall-clock limit came before the last checkpoint
    crash = [l for l in err.splitlines() if l.startswith('[FATAL]') or l.lstrip().startswith('#')][:8]
    return {'game': game, 'status': status, 'last': frames[-1].stem if frames else '-', 'frames': len(frames),
            'distinct': distinct, 'crash': crash, 'out': out, 'points': points}


def check(result, refs, flash0):
    """Per checkpoint: (vblank, label, verdict, failed)."""
    meta_path = refs / 'meta.json'
    meta = json.loads(meta_path.read_text(encoding='utf-8')) if meta_path.exists() else None
    lines = []
    if meta and flash0 and meta.get('flash0') != str(flash0):
        lines.append((None, '', 'note: references made with flash0 %s' % meta.get('flash0'), False))
    for vb, tol, label in result['points']:
        frame, ref = result['out'] / frame_name(vb), refs / frame_name(vb)
        if not ref.exists():
            lines.append((vb, label, 'no reference (--record)', False))
        elif not frame.exists():
            lines.append((vb, label, 'MISSED', True))
        elif frame.read_bytes() == ref.read_bytes():
            lines.append((vb, label, 'same', False))
        else:
            share = differing_share(frame, ref) * 100
            if share <= tol:
                lines.append((vb, label, 'within %.1f%% (%.2f%% differ)' % (tol, share), False))
            else:
                lines.append((vb, label, 'CHANGED: %.2f%% of pixels differ' % share, True))
    return lines


def record(result, refs, args, flash0):
    missing = [vb for vb, _, _ in result['points'] if not (result['out'] / frame_name(vb)).exists()]
    if missing:
        return 'not recorded: no frame at %s' % ', '.join(map(str, missing))
    if refs.exists():
        shutil.rmtree(refs)
    refs.mkdir(parents=True)
    for vb, _, _ in result['points']:
        shutil.copyfile(result['out'] / frame_name(vb), refs / frame_name(vb))
        write_png(refs / frame_name(vb), (refs / frame_name(vb)).with_suffix('.png'))
    meta = {'pspc': str(pathlib.Path(args.pspc).resolve()), 'flash0': str(flash0) if flash0 else None,
            'aot_dir': args.aot_dir, 'interp': args.interp,
            'recorded': datetime.datetime.now().isoformat(timespec='seconds'),
            'checkpoints': {str(vb): label for vb, _, label in result['points']}}
    (refs / 'meta.json').write_text(json.dumps(meta, indent=1) + '\n', encoding='utf-8')
    return 'recorded %d checkpoint(s) in %s' % (len(result['points']), refs)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / ('pspc' + EXE_SUFFIX)))
    ap.add_argument('--flash0', help='firmware flash0 (default: next to pspc.exe, else build/RelWithDebInfo)')
    ap.add_argument('--aot-dir', help='AOT module DLLs (default: pspc\'s own cache)')
    ap.add_argument('--games', default=str(ROOT / 'Games'))
    ap.add_argument('--seconds', type=int, default=45, help='wall time for games without checkpoints')
    ap.add_argument('--max-seconds', type=int, default=600, help='wall-time limit for checkpoint runs')
    ap.add_argument('--every', type=int, default=300, help='dump a frame every N VBlanks (no checkpoints)')
    ap.add_argument('--interp', action='store_true')
    ap.add_argument('--jobs', type=int, default=2)
    ap.add_argument('--out', default=str(ROOT / 'build' / 'smoke'))
    ap.add_argument('--refs', default=str(ROOT / 'build' / 'checkpoints'), help='reference frames')
    ap.add_argument('--record', action='store_true', help='make this run\'s checkpoint frames the references')
    ap.add_argument('filters', nargs='*')
    args = ap.parse_args()
    sys.stdout.reconfigure(errors='backslashreplace')

    pspc_dir = pathlib.Path(args.pspc).parent
    flash0 = find_flash0(args.flash0, pspc_dir)
    if not flash0:
        print('warning: no firmware flash0 found; games take other paths without one (--flash0)')
    inputs = load_inputs(ROOT / 'tools' / 'smoke_inputs.txt')
    checkpoints = load_checkpoints(ROOT / 'tools' / 'checkpoints.txt')
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
        points = checkpoints.get(did) or checkpoints.get(g.stem.upper()) or []
        jobs.append((g, presses, points))
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        results = list(ex.map(lambda j: run_one(args, *j, flash0), jobs))
    mode = 'interp' if args.interp else 'aot'
    bad = 0
    for r in results:
        failed = r['status'].startswith(('CRASH', 'HUNG', 'TOO SLOW')) or r['distinct'] <= 1
        refs = pathlib.Path(args.refs) / mode / r['game'].stem
        lines = []
        if r['points']:
            if args.record:
                lines = [(None, '', record(r, refs, args, flash0), False)]
            else:
                lines = check(r, refs, flash0)
        passed = sum(1 for vb, _, v, f in lines if vb is not None and not f and not v.startswith('no reference'))
        summary = ('checkpoints %d/%d' % (passed, len(r['points']))) if r['points'] and not args.record else ''
        print('%-32s %-16s last=%-14s frames=%-3d distinct=%-3d %s' % (r['game'].name, r['status'], r['last'],
                                                                        r['frames'], r['distinct'], summary))
        for vb, label, verdict, f in lines:
            if vb is None:
                print('    ' + verdict)
            elif f or verdict.startswith(('no reference', 'within')) or args.record:
                print('    %6d %-32s %s' % (vb, label[:32], verdict))
            failed = failed or f
        for l in r['crash']:
            print('    ' + l[:150])
        bad += failed
    print('\n%d game(s), %d with problems; frames in %s' % (len(results), bad, args.out))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
