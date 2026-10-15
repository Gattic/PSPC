#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Installs a decrypted PSP firmware for pspc: the files the XMB and games read from flash0/flash1,
and native code for the firmware modules that run as guest code (pspc --vsh).

Usage: pspc_install_fw.py --profile <name> [--fw private/fw/401] [--build-dir build/RelWithDebInfo] [options]

Input: the tree `pspc-decrypt psar 401.PBP <dir>` writes (<dir>/flash0 with every module
decrypted), and optionally the M33 modules extracted by tools/m33extract.py (<dir>/m33).

What it does:
  1. Copies flash0/{vsh,font,dic,data,kd} into <data dir>/firmware/<profile>/flash0. The kernel modules (kd) are
     never run - PSPC's kernel is native - but the boot configuration (pspbtcnf) says which
     modules boot the XMB, and the XMB loads some drivers by path.
  2. Copies M33's user-mode UI modules (satelite.prx = the VSH menu, recovery.prx) into
     flash0/vsh/module, as the M33 installer does.
  3. Prepares the profile's flash1 (registry/, vsh/theme/, ...). flash1/registry/init.dat must not
     exist: the XMB reads it as "reset the registry".
  4. AOT-compiles the user-mode firmware modules (vsh/module/*.prx, satelite, recovery) with
     tools/pspc_build.py into the AOT cache, so the XMB runs as native code.

Firmware files are copyrighted: they go only into the (git-ignored) pspc directory and AOT cache.
"""
import argparse
import filecmp
import pathlib
import re
import signal
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
EXE_SUFFIX = '.exe' if sys.platform == 'win32' else ''
FLASH0_DIRS = ('vsh', 'font', 'dic', 'data', 'kd')
M33_UI_MODULES = ('satelite.prx', 'recovery.prx')
FLASH1_DIRS = ('registry', 'vsh/theme', 'dic', 'net/http', 'gps')


def module_info(data):
    """(name, attribute) of a plain PRX/ELF, or None."""
    if data[:4] != b'\x7fELF' or len(data) < 0x34:
        return None
    phoff, = struct.unpack_from('<I', data, 0x1C)
    phnum, = struct.unpack_from('<H', data, 0x2C)
    if phnum == 0 or phoff + 0x20 > len(data):
        return None
    _, _off, _vaddr, paddr = struct.unpack_from('<IIII', data, phoff)
    mi = paddr & 0x7FFFFFFF  # PRX: the file offset of the module info
    if mi < 0 or mi + 0x20 > len(data):
        return None
    attr, = struct.unpack_from('<H', data, mi)
    name = data[mi + 4:mi + 4 + 28].split(b'\0')[0].decode('ascii', 'replace')
    return name, attr


def copy_tree(src, dst, stats):
    for f in sorted(src.rglob('*')):
        if not f.is_file():
            continue
        out = dst / f.relative_to(src)
        if out.exists() and out.stat().st_size == f.stat().st_size and filecmp.cmp(f, out, shallow=False):
            stats['same'] += 1
            continue
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(f, out)
        stats['copied'] += 1


def valid_name(name):
    # Keep the on-disk name contract in sync with common/firmware.cpp.
    return (re.fullmatch(r'[a-z0-9](?:[a-z0-9._-]{0,62}[a-z0-9])?', name) is not None
            and re.fullmatch(r'(con|prn|aux|nul|com[0-9]|lpt[0-9])', name.split('.')[0]) is None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--fw', default=str(ROOT / 'private' / 'fw' / '401'),
                    help='decrypted firmware tree containing flash0/ (default: private/fw/401)')
    ap.add_argument('--m33', default=None, help='extracted M33 modules (default: <fw>/m33)')
    ap.add_argument('--build-dir', default=None,
                    help='build tree with pspc and pspc-recomp (default: build/RelWithDebInfo)')
    ap.add_argument('--pspc-dir', default=None, help='PSPC executables/installed SDK (default: auto-detect or <build dir>/bin)')
    ap.add_argument('--data-dir', help='writable PSPC data root (required on Linux; Windows default: <pspc dir>)')
    ap.add_argument('--profile', required=True, help='new firmware name, e.g. 4.01 or 4.01-m33; never overwrites an installation')
    ap.add_argument('--rebuild', action='store_true', help='build native code for an installed profile without changing its flash files')
    ap.add_argument('--cache', default=None, help='AOT cache (default: <pspc dir>/cache/aot)')
    ap.add_argument('--work', default=None, help='pspc_build.py work directory')
    ap.add_argument('--no-aot', action='store_true', help='copy files only')
    ap.add_argument('--aot-core', action='store_true',
                    help='compile only the modules the XMB starts with (paf, common_*, vshmain, heaparea, '
                         'the boot plugins and the M33 menu) instead of every vsh/module/*.prx')
    ap.add_argument('--modules-in-parallel', type=int, default=4)
    ap.add_argument('--jobs', type=int, default=4)
    ap.add_argument('--force', action='store_true', help='recompile modules that are up to date')
    args = ap.parse_args()
    if sys.platform != 'win32':
        if not args.data_dir:
            ap.error('--data-dir is required on Linux; use the runtime user-data directory')
        if not args.no_aot and (not args.cache or not args.work):
            ap.error('Linux firmware AOT requires writable --cache and --work paths')

    if not valid_name(args.profile):
        ap.error('invalid profile name: use 1-64 lowercase letters, digits, dots, hyphens or underscores; start/end with a letter or digit')
    if args.rebuild and args.no_aot:
        ap.error('--rebuild cannot be combined with --no-aot')
    build = pathlib.Path(args.build_dir) if args.build_dir else ROOT / 'build' / 'RelWithDebInfo'
    pspc_dir = pathlib.Path(args.pspc_dir) if args.pspc_dir else (ROOT if (ROOT / ('pspc' + EXE_SUFFIX)).is_file() else build / 'bin')
    data_root = pathlib.Path(args.data_dir) if args.data_dir else pspc_dir
    store = data_root / 'firmware'
    destination = store / args.profile
    if store.is_symlink() or destination.is_symlink():
        ap.error('firmware store/profile must not be a symbolic link')
    if args.rebuild:
        return install(args, destination, destination, build, pspc_dir)
    if destination.exists():
        ap.error('profile already exists; use --rebuild or choose a different name')
    store.mkdir(parents=True, exist_ok=True)
    # Publish only a complete install, on the same filesystem. Failed/cancelled builds leave
    # the existing selection and every installed profile unchanged.
    with tempfile.TemporaryDirectory(prefix='.building-' + args.profile + '-', dir=store) as temp:
        rc = install(args, pathlib.Path(args.fw), pathlib.Path(temp), build, pspc_dir)
        if rc:
            return rc
        if destination.exists() or destination.is_symlink():
            ap.error('profile appeared during the build; refusing to overwrite it')
        pathlib.Path(temp).rename(destination)
    print('Installed firmware profile: %s (select it in Firmware Manager)' % args.profile)
    return 0


def install(args, fw, data_dir, build, pspc_dir):
    flash0 = fw / 'flash0'
    vshmain = flash0 / 'vsh/module/vshmain.prx'
    if not vshmain.is_file() or module_info(vshmain.read_bytes()) is None:
        raise ValueError('%s is missing or not a decrypted module' % vshmain)
    bundled = not args.build_dir and (pspc_dir / 'aot').is_dir()
    if args.rebuild:
        return build_native(args, data_dir / 'flash0', build, pspc_dir, bundled)

    # 1. flash0
    dst0 = data_dir / 'flash0'
    stats = {'copied': 0, 'same': 0}
    for d in FLASH0_DIRS:
        if (flash0 / d).is_dir():
            copy_tree(flash0 / d, dst0 / d, stats)
    print('flash0: %d files copied, %d already up to date -> %s' % (stats['copied'], stats['same'], dst0))

    # 2. M33's VSH menu and recovery.
    m33 = pathlib.Path(args.m33) if args.m33 else fw / 'm33'
    m33_mods = []
    for name in M33_UI_MODULES:
        src = m33 / name
        if src.exists() and module_info(src.read_bytes()):
            out = dst0 / 'vsh' / 'module' / name
            out.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, out)
            m33_mods.append(out)
    print('M33: %s' % (', '.join(p.name for p in m33_mods) if m33_mods else 'not found (%s)' % m33))

    # 3. flash1
    dst1 = data_dir / 'flash1'
    for d in FLASH1_DIRS:
        (dst1 / d).mkdir(parents=True, exist_ok=True)
    if (fw / 'flash1').is_dir():
        copy_tree(fw / 'flash1', dst1, {'copied': 0, 'same': 0})
    init_dat = dst1 / 'registry' / 'init.dat'
    if init_dat.exists():
        init_dat.rename(init_dat.with_suffix('.dat.bak'))
        print('flash1: moved registry/init.dat aside (the XMB would reset the registry)')
    print('flash1: %s' % dst1)

    if args.no_aot:
        return 0

    return build_native(args, dst0, build, pspc_dir, bundled)


def build_native(args, dst0, build, pspc_dir, bundled):
    # 4. Native code for the user-mode firmware modules. Rebuilding never touches flash1.
    modules = []
    for p in sorted((dst0 / 'vsh' / 'module').glob('*.prx')):
        info = module_info(p.read_bytes())
        if info is None or info[1] & 0x1000:  # not decrypted, or a kernel module (native)
            continue
        modules.append(p)
    if args.aot_core:
        core = {'heaparea1.prx', 'heaparea2.prx', 'paf.prx', 'common_gui.prx', 'common_util.prx', 'vshmain.prx',
                'opening_plugin.prx', 'impose_plugin.prx', 'game_plugin.prx', 'sysconf_plugin.prx',
                'auth_plugin.prx', 'msgdialog_plugin.prx', 'osk_plugin.prx', 'savedata_plugin.prx'}
        core.update(M33_UI_MODULES)
        modules = [p for p in modules if p.name in core]
    work = pathlib.Path(args.work) if args.work else (pspc_dir / 'cache' / 'aot-work' if bundled else build)
    lst = work / 'pspc_install_fw_modules.txt'
    lst.parent.mkdir(parents=True, exist_ok=True)
    lst.write_text(''.join(str(p) + '\n' for p in modules), encoding='utf-8')
    cmd = [sys.executable, '-I', '-B', '-X', 'utf8', str(ROOT / 'tools' / 'pspc_build.py'), '@' + str(lst),
           '--pspc-dir' if bundled else '--build-dir', str(pspc_dir if bundled else build),
           '--cache', args.cache or str(pspc_dir / 'cache' / 'aot'),
           '--modules-in-parallel', str(args.modules_in_parallel), '--jobs', str(args.jobs)]
    if args.work:
        cmd += ['--work', args.work]
    if args.force:
        cmd.append('--force')
    print('AOT: %d modules (%s)' % (len(modules), ' '.join(cmd[2:4])), flush=True)
    return subprocess.run(cmd).returncode


if __name__ == '__main__':
    def cancelled(signum, frame):
        raise KeyboardInterrupt
    signal.signal(signal.SIGTERM, cancelled)
    try:
        sys.exit(main())
    except (OSError, ValueError) as error:
        sys.exit(str(error))
    except KeyboardInterrupt:
        sys.exit(130)
