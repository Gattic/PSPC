#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""PSPC's own guest test programs (tests/guest/<name>/, built by tools/build_guest_tests.sh).

usage: guest_tests.py [--pspc PATH] [--timeout S] [--interp-only] [name ...]

Each test with an expected.txt runs in the interpreter and as native code that is built fresh for
the run (tools/pspc_build.py into a temporary folder: no hints from earlier runs, the way a
player's first build is), and its output must equal expected.txt. Tests whose program isn't built
are skipped. tests/guest/large_image has its own runner (tools/large_image_test.py).
"""
import argparse
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
BUILT = ROOT / 'build' / 'guest-tests'


def build_native(prx, aot_dir, build_dir):
    """Recompiles `prx` into aot_dir with pspc_build.py. Returns an error message or None."""
    work = pathlib.Path(tempfile.mkdtemp(prefix='pspc_guest_work_'))
    try:
        r = subprocess.run([sys.executable, '-I', str(ROOT / 'tools' / 'pspc_build.py'), str(prx),
                            '--build-dir', str(build_dir), '--cache', str(aot_dir), '--work', str(work), '--force'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace')
        modules = pathlib.Path(aot_dir).glob('*.dll' if sys.platform == 'win32' else '*.so')
        if r.returncode != 0 or not any(modules):
            return 'pspc_build.py failed:\n' + '\n'.join(r.stdout.splitlines()[-10:])
        return None
    finally:
        shutil.rmtree(work, ignore_errors=True)


def run_pspc(pspc, target, timeout, aot_dir=None, extra=()):
    """Output lines of `target` run headless, or (None, stderr tail) if it didn't finish."""
    ms = tempfile.mkdtemp(prefix='pspc_guest_ms_')
    cmd = [str(pspc), '--config', 'none', '--no-plugins', '--headless', '--fast-time', '--timeout', str(timeout),
           '--log', 'warn', '--memstick', ms]
    cmd += ['--aot-dir', str(aot_dir)] if aot_dir else ['--interp']
    cmd += list(extra) + [str(target)]
    try:
        r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 30)
        out = r.stdout.decode('utf-8', 'replace').replace('\r\n', '\n').rstrip('\n').split('\n')
        err = r.stderr.decode('utf-8', 'replace').strip().splitlines()
        return (out if r.returncode == 0 else None), err[-8:]
    except subprocess.TimeoutExpired:
        return None, ['host timeout']
    finally:
        shutil.rmtree(ms, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / ('pspc.exe' if sys.platform == 'win32' else 'pspc')))
    ap.add_argument('--timeout', type=int, default=120)
    ap.add_argument('--interp-only', action='store_true')
    ap.add_argument('names', nargs='*')
    args = ap.parse_args()
    pspc = pathlib.Path(args.pspc).resolve()
    failed = 0
    for test in sorted((ROOT / 'tests' / 'guest').iterdir()):
        exp = test / 'expected.txt'
        if not exp.exists() or (args.names and test.name not in args.names):
            continue
        prx = BUILT / test.name / (test.name + '.prx')
        if not prx.exists():
            print('%-16s SKIP: not built (tools/build_guest_tests.sh)' % test.name)
            continue
        want = exp.read_text(encoding='utf-8').rstrip('\n').split('\n')
        modes = ['interpreter'] if args.interp_only else ['interpreter', 'native']
        for mode in modes:
            aot = None
            if mode == 'native':
                aot = pathlib.Path(tempfile.mkdtemp(prefix='pspc_guest_aot_'))
                err = build_native(prx, aot, pspc.parent.parent)
                if err:
                    print('%-16s %-12s FAIL: %s' % (test.name, mode, err))
                    failed += 1
                    shutil.rmtree(aot, ignore_errors=True)
                    continue
            got, err = run_pspc(pspc, prx, args.timeout, aot)
            if aot:
                shutil.rmtree(aot, ignore_errors=True)
            if got == want:
                print('%-16s %-12s ok' % (test.name, mode))
                continue
            failed += 1
            print('%-16s %-12s FAIL' % (test.name, mode))
            if got is None:
                print('    did not finish: ' + '\n                    '.join(err))
            else:
                for i, (w, g) in enumerate(zip(want + ['<missing>'] * len(got), got + ['<missing>'] * len(want))):
                    if w != g:
                        print('    line %d: expected %r, got %r' % (i + 1, w, g))
                        break
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
