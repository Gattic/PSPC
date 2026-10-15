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
from firmware_paths import resolve_firmware  # noqa: E402

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
#   host0_extra: a generated folder (HOST0_EXTRAS) laid under host0: (--host0-extra), for files
#       the recording folder had and a checkout doesn't.
#   needs: files the test needs in pspc's flash0 (without them it is skipped).
#   rebuilt: the shipped .prx predates the .expected; run the one tools/rebuild_autotests.sh
#       builds in build/autotest-rebuilt instead (without it the test is skipped).
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
    # Same heap shift: the pattern lines read bytes just before the test's buffers.
    'jpeg/mjpegcsc': {'mode': 'display'},
    # Hardware details games don't see, modelled only in the interpreter on request: VRAM's
    # swizzled mirrors, and the data cache seen through the uncached segment.
    'gpu/ge/edramswizzle': {'args': ['--edram-mirrors']},
    'cpu/lsu/cacheop': {'args': ['--dcache-model']},
    'umd/raw_access/raw_access': {'disc': 'umd-test'},
    # Recorded through psplink, which shows host0: names as the PC has them (io/directory's
    # recording shows FAT's view, pspc's default), in the author's build folder: io.elf and io.o
    # sat next to io.c.
    'io/io/io': {'args': ['--host0-raw-names'], 'host0_extra': 'io-io'},
    # Recorded on a PSP whose user memory ended at 0x0BB00000, so thread stacks went above
    # 0x0A000000 and print as "UNKNOWN" (the pmf tests' T_Reader address shows the same end).
    'threads/threads/create': {'args': ['--large-mem', '--user-mem-end', '0x0BB00000']},
    # The shipped vregs.prx has 1e+07 in its value table where the source has 100.0, and lacks
    # three sections the .expected has.
    'cpu/vfpu/vregs': {'rebuilt': True},
}

# Generated discs (autotest_fixtures), made once per run in build/autotest-discs.
DISCS = {'umd-test': 'umd-test.iso'}

# Generated host0: overlays: empty stand-ins for files of the recording folder, in
# build/autotest-host0/<name>.
HOST0_EXTRAS = {'io-io': ['tests/io/io/io.elf', 'tests/io/io/io.o']}
HOST0_EXTRA_DIR = ROOT / 'build' / 'autotest-host0'
REBUILT_DIR = ROOT / 'build' / 'autotest-rebuilt'

# Tests whose expected output pspc can't reproduce, with why (docs/autotests-plan.md, "What
# stays different"). They are reported apart from failures; one that starts passing is reported
# so it can come off the list.
KNOWN_DIFFERENCES = {
    'threads/scheduling/delaylen':
        'hardware jitter: 210us needs at least 25us of overhead, 500us at most 24us',
    'threads/scheduling/preemptuser':
        'hardware jitter: the delay must be <=1024 here and >=1025 in delaylen',
    'threads/scheduling/scheduling':
        'one cost in a chain of five is off; which one needs timed hardware data',
    'display/hcountwrap': 'prints the phase samples of one hardware run',
    'threads/threads/profiler': "prints the recording PSP's uptime and cache warm-up",
    'audio/sceaudio/output': 'block 4 differs from blocks 1-3 under identical conditions (jitter)',
    'sysmem/partition': 'depends on what was resident on the recording PSP',
    'threads/vpl/memory': 'about 16 KB more was resident on the recording PSP',
    'threads/threads/create':
        "one scheduling line: whether a 1us-timeout wait ends inside sceKernelCreateThread's "
        '~185us (pspc: 235us); the other costs are calibrated by other tests',
    'video/psmfplayer/basic': "prints addresses inside libpsmfplayer.prx, which the suite doesn't ship",
    'video/psmfplayer/playmode': "slow motion, step frame and warm-up follow libpsmfplayer's rules",
    'video/psmfplayer/configplayer':
        'looping stalls ~100 calls on hardware (re-buffering from host0 over USB, not calibrated)',
    'video/mpeg/basic': "real AU sizes from the demuxer (sceMpegGetAvcAu) and the ringbuffer's pack count",
    'video/pmf/pmf': 'same as video/mpeg/basic',
    'video/pmf_simple/pmf_simple': 'same as video/mpeg/basic',
    'jpeg/decodeycbcr': 'where hardware writes into the frame buffer with a NULL output is unknown',
    'jpeg/csc': '"extra bits": not determinable from the test',
    'jpeg/mjpegcsc': 'heights under 16, strides -4 and 1, height 479: not determinable from the test',
    'io/iodrv/iodrv': 'line 2 came from an emulator that turned flags 0x202 into 7',
    'umd/raw_access/raw_access':
        "a 33-character sce_lbn path and byte offsets on umd0:, which Sony's isofs refuses (an M33 ISO driver)",
}


def prepare_fixtures(tests_dir):
    """Test data git can't carry (autotest_fixtures)."""
    file_c = tests_dir / 'io' / 'file' / 'file.c'
    if file_c.exists():
        autotest_fixtures.set_local_time(file_c, 2013, 1, 21, 0, 11, 54)
    discs = ROOT / 'build' / 'autotest-discs'
    discs.mkdir(parents=True, exist_ok=True)
    sfo = autotest_fixtures.raw_access_sfo(tests_dir / 'umd' / 'raw_access' / 'raw_access.c')
    autotest_fixtures.build_umd_test_disc(discs / DISCS['umd-test'], sfo)
    for name, files in HOST0_EXTRAS.items():
        for rel in files:
            p = HOST0_EXTRA_DIR / name / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.touch()
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
    flash0 = pathlib.Path(args.flash0)
    missing = [n for n in env.get('needs', []) if not (flash0 / n).exists()]
    skip = 'needs %s in flash0' % ', '.join(missing) if missing else None
    if env.get('rebuilt'):
        prx = REBUILT_DIR / (rel + '.prx')
        if not prx.exists():
            skip = 'needs the rebuilt prx (tools/rebuild_autotests.sh)'
    if skip:
        return {'test': rel, 'pass': False, 'skipped': skip, 'exit': None,
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
    if 'host0_extra' in env:
        cmd += ['--host0-extra', str(HOST0_EXTRA_DIR / env['host0_extra'])]
    # A disc image shipped next to the test is inserted as the UMD (or a generated one).
    if 'disc' in env:
        cmd += ['--umd', str(args.discs / DISCS[env['disc']])]
    else:
        for ext in ('.iso', '.cso'):
            if prx.with_suffix(ext).exists():
                cmd += ['--umd', str(prx.with_suffix(ext))]
                break
    cmd += ['--flash0', args.flash0, '--flash1', args.flash1]
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
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / ('pspc.exe' if sys.platform == 'win32' else 'pspc')))
    ap.add_argument('--tests', default=str(ROOT / 'reference' / 'pspautotests' / 'tests'))
    ap.add_argument('--jobs', type=int, default=max(1, (os.cpu_count() or 4) - 2))
    ap.add_argument('--timeout', type=int, default=20)
    ap.add_argument('--aot', action='store_true', help='allow AOT modules (default: interpreter only)')
    ap.add_argument('--json', default=None)
    ap.add_argument('--verbose', '-v', action='store_true')
    ap.add_argument('--flash0', default=None, help="override flash0 (default: the runtime's selected firmware profile)")
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
    try:
        args.flash0, args.flash1 = resolve_firmware(args.pspc, args.flash0)
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        ap.error(str(error))
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
                if status == 'KNOWN' and args.verbose:
                    print('    known: %s' % KNOWN_DIFFERENCES[r['test']])
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
