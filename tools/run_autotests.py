#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run pspautotests through pspc and compare against the recorded .expected output.

Usage: run_autotests.py [--pspc PATH] [--tests DIR] [--jobs N] [--timeout S] [--aot]
                        [--json OUT] [--verbose] [filter ...]

A filter is a substring of the test path relative to the tests directory (e.g. "cpu/" or
"threads/semaphores"). Exit status is 0 when every selected test passes.
"""
import argparse
import concurrent.futures
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def find_tests(tests_dir, filters):
    out = []
    for prx in sorted(tests_dir.rglob('*.prx')):
        exp = prx.with_suffix('.expected')
        if not exp.exists():
            continue
        rel = prx.relative_to(tests_dir).as_posix()[:-4]
        if filters and not any(f in rel for f in filters):
            continue
        out.append((rel, prx, exp))
    return out


def normalize(text):
    lines = text.replace('\r\n', '\n').replace('\r', '\n').split('\n')
    while lines and lines[-1] == '':
        lines.pop()
    return lines


def run_one(args, rel, prx, exp):
    cmd = [args.pspc, '--config', 'none', '--no-plugins', '--headless', '--fast-time', '--timeout', str(args.timeout), '--log', 'warn']
    if not args.aot:
        cmd.append('--interp')
    # Tests address their data as host0:/tests/..., i.e. host0: is the pspautotests root.
    cmd += ['--host0', str(pathlib.Path(args.tests).parent)]
    # A fresh memory stick per test: savedata tests share folder names and would collide.
    # Per run (pid) as well, so concurrent runs of the suite don't share folders.
    ms = ROOT / 'build' / 'autotest-ms' / str(os.getpid()) / rel.replace('/', '_')
    shutil.rmtree(ms, ignore_errors=True)
    ms.mkdir(parents=True, exist_ok=True)
    cmd += ['--memstick', str(ms)]
    # A disc image shipped next to the test is inserted as the UMD.
    for ext in ('.iso', '.cso'):
        if prx.with_suffix(ext).exists():
            cmd += ['--umd', str(prx.with_suffix(ext))]
            break
    cmd.append(str(prx))
    start = time.time()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout + 10)
        out = proc.stdout.decode('utf-8', 'replace')
        err = proc.stderr.decode('utf-8', 'replace')
        code = proc.returncode
    except subprocess.TimeoutExpired:
        out, err, code = '', 'host timeout', -1
    shutil.rmtree(ms, ignore_errors=True)
    elapsed = time.time() - start
    want = normalize(exp.read_text(encoding='utf-8', errors='replace'))
    got = normalize(out)
    ok = want == got
    first_diff = None
    if not ok:
        for i in range(max(len(want), len(got))):
            w = want[i] if i < len(want) else '<missing>'
            g = got[i] if i < len(got) else '<missing>'
            if w != g:
                first_diff = {'line': i + 1, 'expected': w, 'got': g}
                break
    return {
        'test': rel, 'pass': ok, 'exit': code, 'seconds': round(elapsed, 2),
        'expected_lines': len(want), 'got_lines': len(got), 'first_diff': first_diff,
        'stderr_tail': err.strip().splitlines()[-6:] if not ok else [],
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / 'pspc.exe'))
    ap.add_argument('--tests', default=str(ROOT / 'reference' / 'pspautotests' / 'tests'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--timeout', type=int, default=20)
    ap.add_argument('--aot', action='store_true', help='allow AOT modules (default: interpreter only)')
    ap.add_argument('--json', default=None)
    ap.add_argument('--verbose', '-v', action='store_true')
    ap.add_argument('filters', nargs='*')
    args = ap.parse_args()
    # Test output can hold characters the console code page lacks.
    sys.stdout.reconfigure(errors='backslashreplace')

    tests = find_tests(pathlib.Path(args.tests), args.filters)
    if not tests:
        print('no tests selected')
        return 1
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_one, args, *t) for t in tests]
        for f in concurrent.futures.as_completed(futs):
            r = f.result()
            results.append(r)
            if args.verbose or not r['pass']:
                status = 'PASS' if r['pass'] else 'FAIL'
                print('%s %-50s exit=%s %.1fs' % (status, r['test'], r['exit'], r['seconds']))
                if not r['pass'] and r['first_diff'] and args.verbose:
                    d = r['first_diff']
                    print('    line %d: expected %r' % (d['line'], d['expected']))
                    print('    %s       got %r' % (' ' * len(str(d['line'])), d['got']))
    results.sort(key=lambda r: r['test'])
    passed = sum(1 for r in results if r['pass'])
    print('\n%d / %d passed' % (passed, len(results)))
    by_dir = {}
    for r in results:
        top = r['test'].split('/')[0]
        p, n = by_dir.get(top, (0, 0))
        by_dir[top] = (p + (1 if r['pass'] else 0), n + 1)
    for d in sorted(by_dir):
        print('  %-10s %3d / %3d' % (d, *by_dir[d]))
    if args.json:
        pathlib.Path(args.json).write_text(json.dumps(results, indent=1), encoding='utf-8')
    shutil.rmtree(ROOT / 'build' / 'autotest-ms' / str(os.getpid()), ignore_errors=True)
    return 0 if passed == len(results) else 1


if __name__ == '__main__':
    sys.exit(main())
