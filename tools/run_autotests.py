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

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))  # also when run with python -I
import autotest_fixtures  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent

# How a test's expected output was recorded, where that differs from what pspc gives a test by
# default (docs/autotests-plan.md has the evidence for each entry):
#   mode 'emulator' (default): pspc's emulator devices are there, so common.c sends the output
#       through them and draws nothing on the screen.
#   mode 'display': as with an emulator, but with a screen (--test-display): common.c also draws
#       its output with the debug font and writes it to host0:/__testoutput.txt (two more fds).
#   mode 'psp': no emulator devices (--test-as-psp), as on a PSP: common.c also starts its
#       update_thread and writes its output to host0:/__testoutput.txt only, which pspc sends to
#       a file of the test's own. Tests that load modules the suite doesn't ship can't use it.
#   args: more pspc arguments.
#   disc: a generated disc (DISCS) inserted as the UMD instead of the one next to the test.
#   needs: files the test needs in pspc's flash0 (without them it is skipped).
ENVIRONMENT = {
    # Recorded with a screen: the expected pixels are the top-left of the debug font's "R" that
    # common.c printed into the framebuffer (sceGuClear leaves alpha, so FF000000 stays).
    'gpu/simple/simple': {'mode': 'display'},
    # The freopen to host0:/__testoutput.txt and __testerror.txt took fds 3 and 4, so the
    # test's tty opens start at 5.
    'io/open/tty0': {'mode': 'display'},
    # Same freopen: its heap blocks (Font_Free addresses) are 0x20 higher. Opens jpn0.pgf.
    'font/fonttest': {'mode': 'display', 'needs': ['font/jpn0.pgf']},
    'umd/io/umd_io': {'disc': 'umd-test'},
    # Hardware details games don't see, modelled only in the interpreter on request: VRAM's
    # swizzled mirrors, and the data cache seen through the uncached segment.
    'gpu/ge/edramswizzle': {'args': ['--edram-mirrors']},
    'cpu/lsu/cacheop': {'args': ['--dcache-model']},
    'umd/raw_access/raw_access': {'disc': 'umd-test'},
}

# Generated discs (autotest_fixtures), made once per run in build/autotest-discs.
DISCS = {'umd-test': 'umd-test.iso'}

# Tests whose expected output pspc can't reproduce, with why (docs/autotests-plan.md, "What
# stays different"). They are reported apart from failures; one that starts passing is reported
# so it can come off the list.
KNOWN_DIFFERENCES = {}


def prepare_fixtures(tests_dir):
    """Test data git can't carry (autotest_fixtures)."""
    file_c = tests_dir / 'io' / 'file' / 'file.c'
    if file_c.exists():
        autotest_fixtures.set_local_time(file_c, 2013, 1, 21, 0, 11, 54)
    discs = ROOT / 'build' / 'autotest-discs'
    discs.mkdir(parents=True, exist_ok=True)
    sfo = autotest_fixtures.raw_access_sfo(tests_dir / 'umd' / 'raw_access' / 'raw_access.c')
    autotest_fixtures.build_umd_test_disc(discs / DISCS['umd-test'], sfo)
    return discs


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
    env = ENVIRONMENT.get(rel, {})
    flash0 = pathlib.Path(args.flash0) if args.flash0 else pathlib.Path(args.pspc).parent / 'flash0'
    missing = [n for n in env.get('needs', []) if not (flash0 / n).exists()]
    if missing:
        return {'test': rel, 'pass': False, 'skipped': 'needs %s in flash0' % ', '.join(missing), 'exit': None,
                'seconds': 0, 'expected_lines': 0, 'got_lines': 0, 'first_diff': None, 'stderr_tail': []}
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
    # The test's host0:/__testoutput.txt (and .err, .finish) is a file of its own, so tests running
    # in parallel don't share one.
    out_file = ms.parent / (ms.name + '.out')
    for f in (out_file, out_file.with_name(out_file.name + '.err'), out_file.with_name(out_file.name + '.finish')):
        f.unlink(missing_ok=True)
    cmd += ['--test-output', str(out_file)]
    mode = args.mode or env.get('mode', 'emulator')
    if mode == 'display':
        cmd.append('--test-display')
    elif mode == 'psp':
        cmd.append('--test-as-psp')
    cmd += env.get('args', [])
    # A disc image shipped next to the test is inserted as the UMD (or a generated one).
    if 'disc' in env:
        cmd += ['--umd', str(args.discs / DISCS[env['disc']])]
    else:
        for ext in ('.iso', '.cso'):
            if prx.with_suffix(ext).exists():
                cmd += ['--umd', str(prx.with_suffix(ext))]
                break
    if args.flash0:
        cmd += ['--flash0', args.flash0]
    cmd.append(str(prx))
    start = time.time()
    try:
        proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=args.timeout + 10)
        out = proc.stdout.decode('utf-8', 'replace')
        err = proc.stderr.decode('utf-8', 'replace')
        code = proc.returncode
    except subprocess.TimeoutExpired:
        out, err, code = '', 'host timeout', -1
    if mode == 'psp':
        out = out_file.read_bytes().decode('utf-8', 'replace') if out_file.exists() else ''
    for f in (out_file, out_file.with_name(out_file.name + '.err'), out_file.with_name(out_file.name + '.finish')):
        f.unlink(missing_ok=True)
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
    ap.add_argument('--flash0', default=None, help="pspc's flash0 (default: the one next to pspc.exe)")
    ap.add_argument('--mode', choices=('emulator', 'display', 'psp'),
                    help='run every selected test in this mode instead of its own (see ENVIRONMENT)')
    ap.add_argument('filters', nargs='*')
    args = ap.parse_args()
    # Test output can hold characters the console code page lacks.
    sys.stdout.reconfigure(errors='backslashreplace')

    tests = find_tests(pathlib.Path(args.tests), args.filters)
    if not tests:
        print('no tests selected')
        return 1
    args.discs = prepare_fixtures(pathlib.Path(args.tests))
    results = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        futs = [ex.submit(run_one, args, *t) for t in tests]
        for f in concurrent.futures.as_completed(futs):
            r = f.result()
            results.append(r)
            if r.get('skipped'):
                print('SKIP %-50s %s' % (r['test'], r['skipped']))
                continue
            if args.verbose or not r['pass']:
                status = 'PASS' if r['pass'] else ('KNOWN' if r['test'] in KNOWN_DIFFERENCES else 'FAIL')
                print('%s %-50s exit=%s %.1fs' % (status, r['test'], r['exit'], r['seconds']))
                if not r['pass'] and r['first_diff'] and args.verbose:
                    d = r['first_diff']
                    print('    line %d: expected %r' % (d['line'], d['expected']))
                    print('    %s       got %r' % (' ' * len(str(d['line'])), d['got']))
    results.sort(key=lambda r: r['test'])
    passed = sum(1 for r in results if r['pass'])
    skipped = [r['test'] for r in results if r.get('skipped')]
    known = [r['test'] for r in results if not r['pass'] and not r.get('skipped') and r['test'] in KNOWN_DIFFERENCES]
    unexpected = [r['test'] for r in results if not r['pass'] and not r.get('skipped') and r['test'] not in KNOWN_DIFFERENCES]
    fixed = [r['test'] for r in results if r['pass'] and r['test'] in KNOWN_DIFFERENCES]
    print('\n%d / %d passed' % (passed, len(results)))
    print('%d known differences, %d unexpected failures, %d skipped' % (len(known), len(unexpected), len(skipped)))
    for t in fixed:
        print('  now passes, take it off KNOWN_DIFFERENCES: %s' % t)
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
    return 0 if not unexpected else 1


if __name__ == '__main__':
    sys.exit(main())
