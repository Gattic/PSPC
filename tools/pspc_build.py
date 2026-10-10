#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Ahead-of-time build: recompile PSP modules to native DLLs for pspc.

Usage: pspc_build.py [options] <module.prx|.elf|EBOOT.PBP> [...]

For each input the plain ELF is located (PBP: DATA.PSP; encrypted ~PSP files go through
pspc-decrypt), recompiled with pspc-recomp, compiled with clang-cl (or cl) into <sha1>.dll and
copied into the AOT cache that pspc.exe searches (default: <pspc dir>/cache/aot).

The cache's games.json records, per input (disc image, PBP or module file), the modules it was
built from and their SHA-1s, so the launcher can tell which games have native code.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
import pathlib
import shutil
import struct
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
BIN = ROOT / 'build' / 'RelWithDebInfo' / 'bin'
LIB = ROOT / 'build' / 'RelWithDebInfo' / 'lib'
SEM_HEADER = ROOT / 'build' / 'RelWithDebInfo' / 'generated' / 'pspc_sem_version.h'
LINK_LIBS = ('pspc_sem.lib', 'pspc_common.lib')

_seen_lock = threading.Lock()
_seen = set()
_module_sha = {}  # module path -> SHA-1 of its plain ELF (for games.json)


def source_sem_version():
    """Hash of src/allegrex computed exactly as src/CMakeLists.txt does."""
    files = sorted(p.resolve().as_posix() for p in (ROOT / 'src' / 'allegrex').iterdir() if p.is_file())
    joined = ''.join(hashlib.sha1(pathlib.Path(f).read_bytes()).hexdigest() for f in files)
    return hashlib.sha1(joined.encode()).hexdigest()[:8]


def sem_version():
    """The semantics version the runtime will demand. The DLLs are compiled against the current
    sources and link the build tree's libraries, so both must agree."""
    built = None
    for line in SEM_HEADER.read_text().splitlines():
        if line.startswith('#define PSPC_SEM_VERSION'):
            built = line.split()[2].rstrip('u').lower()[2:]
    if not built:
        raise RuntimeError('no PSPC_SEM_VERSION in %s' % SEM_HEADER)
    current = source_sem_version()
    if built != current:
        raise RuntimeError('src/allegrex changed since %s was built (%s != %s): rebuild the main tree first'
                           % (SEM_HEADER.parent.parent, built, current))
    return '0x' + built


def snapshot_libs(work, version):
    """Copies the libraries the DLLs link so a concurrent main build can't swap them mid-batch."""
    dest = work / ('lib-' + version)
    dest.mkdir(parents=True, exist_ok=True)
    for name in LINK_LIBS:
        src = LIB / name
        if not src.exists():
            raise RuntimeError('%s missing - build the main tree first' % src)
        shutil.copy2(src, dest / name)
    return dest


def vs_env():
    vswhere = pathlib.Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / \
        'Microsoft Visual Studio' / 'Installer' / 'vswhere.exe'
    out = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                          'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'],
                         stdout=subprocess.PIPE, text=True).stdout.strip()
    vcvars = pathlib.Path(out) / 'VC' / 'Auxiliary' / 'Build' / 'vcvars64.bat'
    env_out = subprocess.run('call "%s" >nul && set' % vcvars, shell=True, stdout=subprocess.PIPE, text=True).stdout
    env = dict(os.environ)
    for line in env_out.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            env[k] = v
    links = pathlib.Path(os.environ.get('LOCALAPPDATA', '')) / 'Microsoft' / 'WinGet' / 'Links'
    env['PATH'] = env.get('PATH', '') + os.pathsep + str(links) + os.pathsep + r'C:\Program Files\LLVM\bin'
    return env


def plain_elf(path, work, decrypt_tool):
    data = path.read_bytes()
    if data[:4] == b'\0PBP':
        offs = struct.unpack_from('<8I', data, 8)
        end = offs[7] if offs[7] > offs[6] else len(data)
        data = data[offs[6]:end]
    if data[:4] == b'\x7fELF':
        return data
    if data[:4] == b'~PSP':
        tmp_in = work / ('in_' + hashlib.sha1(data).hexdigest())
        tmp_out = tmp_in.with_suffix('.elf')
        tmp_in.write_bytes(data)
        r = subprocess.run([str(decrypt_tool), 'prx', str(tmp_in), str(tmp_out)], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        if r.returncode != 0:
            raise RuntimeError('pspc-decrypt failed (%d): %s' % (r.returncode, r.stderr.strip()))
        return tmp_out.read_bytes()
    raise RuntimeError('%s: not an ELF, PRX, PBP or ~PSP file' % path)


DISC_EXTS = {'.iso', '.cso', '.zso', '.dax'}


def expand_inputs(args, paths, origins=None):
    """UMD images contribute every executable on the disc (EBOOT.BIN, BOOT.BIN, *.PRX).
    `origins` (if given) maps each module path to (input path, name of the module in it)."""
    out = []
    for p in paths:
        if p.suffix.lower() not in DISC_EXTS:
            out.append(p)
            if origins is not None:
                origins[p] = (p, p.name)
            continue
        dest = pathlib.Path(args.work) / ('disc_' + hashlib.sha1(str(p.resolve()).encode()).hexdigest()[:12])
        dest.mkdir(parents=True, exist_ok=True)
        r = subprocess.run([str(BIN / 'pspc-iso.exe'), 'modules', str(p), str(dest)], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, text=True)
        if r.returncode not in (0, 3):
            raise RuntimeError('pspc-iso failed on %s: %s' % (p, r.stderr.strip()))
        for line in r.stdout.splitlines():
            if '\t' in line:
                name, extracted = line.split('\t', 1)
                out.append(pathlib.Path(extracted))
                if origins is not None:
                    origins[out[-1]] = (p, name)
    return out


def parse_sfo(data):
    """PARAM.SFO -> {key: string value} (string entries only)."""
    if data[:4] != b'\0PSF' or len(data) < 20:
        return {}
    key_table, data_table, count = struct.unpack_from('<III', data, 8)
    out = {}
    for i in range(count):
        if 20 + 16 * (i + 1) > len(data):
            break
        key_off, fmt, length, _, data_off = struct.unpack_from('<HHIII', data, 20 + 16 * i)
        start = key_table + key_off
        end = data.find(b'\0', start)
        if fmt == 0x0204 and end > start:
            raw = data[data_table + data_off:data_table + data_off + length]
            out[data[start:end].decode('ascii', 'replace')] = raw.split(b'\0')[0].decode('utf-8', 'replace')
    return out


def game_info(path):
    """DISC_ID and TITLE of a disc image or PBP ({} for anything else or on error)."""
    sfo = b''
    try:
        if path.suffix.lower() in DISC_EXTS:
            with tempfile.TemporaryDirectory() as tmp:
                out = pathlib.Path(tmp) / 'PARAM.SFO'
                subprocess.run([str(BIN / 'pspc-iso.exe'), 'get', str(path), '/PSP_GAME/PARAM.SFO', str(out)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                if out.exists():
                    sfo = out.read_bytes()
        else:
            with open(path, 'rb') as f:
                head = f.read(40)
                if head[:4] == b'\0PBP':
                    offs = struct.unpack_from('<8I', head, 8)
                    f.seek(offs[0])
                    sfo = f.read(max(0, offs[1] - offs[0]))
    except OSError:
        pass
    sfo = parse_sfo(sfo)
    return {k.lower(): sfo[k] for k in ('DISC_ID', 'TITLE') if sfo.get(k)}


def write_index(cache, origins, ok):
    """Records in <cache>/games.json which modules each input was built from:
    {"version": 1, "games": {"<input path>": {"disc_id", "title", "size", "built",
    "modules": [{"name", "sha1", "ok"}]}}}. Entries of other inputs are kept."""
    games = {}
    for module, (game, name) in origins.items():
        games.setdefault(game, []).append({'name': name, 'sha1': _module_sha.get(module), 'ok': ok.get(module, False)})
    index = pathlib.Path(cache) / 'games.json'
    try:
        data = json.loads(index.read_text(encoding='utf-8'))
        if not isinstance(data, dict) or data.get('version') != 1 or not isinstance(data.get('games'), dict):
            raise ValueError
    except (OSError, ValueError):
        data = {'version': 1, 'games': {}}
    for game, modules in games.items():
        try:
            size = game.stat().st_size
        except OSError:
            continue
        entry = {'size': size, 'built': time.strftime('%Y-%m-%dT%H:%M:%S'),
                 'modules': sorted(modules, key=lambda m: m['name'])}
        entry.update(game_info(game))
        data['games'][str(game.resolve())] = entry
    index.parent.mkdir(parents=True, exist_ok=True)
    tmp = index.with_name('games.json.%d.tmp' % os.getpid())
    tmp.write_text(json.dumps(data, indent=1, ensure_ascii=False, sort_keys=True) + '\n', encoding='utf-8')
    for attempt in range(20):
        try:
            os.replace(tmp, index)
            return
        except PermissionError:  # a reader has it open
            time.sleep(0.1)
    tmp.unlink(missing_ok=True)
    print('warning: could not update %s' % index)


# Must match the CMakeLists.txt pspc-recomp writes (used with --cmake).
CLANG_FLAGS = ['/O2', '/clang:-fno-strict-aliasing', '-Wno-unused-label', '-Wno-unused-variable']
MSVC_FLAGS = ['/O2', '/bigobj', '/wd4102', '/wd4189', '/utf-8', '/permissive-', '/Zc:preprocessor']


def build_direct(args, env, src, sha):
    """Compiles and links the generated sources directly: no CMake configure per module."""
    clang = args.compiler == 'clang'
    # CreateProcess searches our own PATH, not env's: resolve the tools against the VS environment.
    cc = shutil.which('clang-cl' if clang else 'cl', path=env['PATH'])
    linker = shutil.which('link', path=env['PATH'])
    if not cc or not linker:
        raise RuntimeError('compiler or linker not found (clang-cl/cl, link)')
    obj_dir = src / 'obj'
    obj_dir.mkdir(exist_ok=True)
    flags = ['/nologo', '/c', '/std:c++20', '/MT', '/EHsc', '/DNDEBUG', '/DNOMINMAX', '/DWIN32_LEAN_AND_MEAN',
             '/D_CRT_SECURE_NO_WARNINGS', '/I' + str(ROOT / 'src'), '/I' + str(SEM_HEADER.parent), '/I' + str(src)]
    flags += CLANG_FLAGS if clang else MSVC_FLAGS

    def compile_one(cpp):
        obj = obj_dir / (cpp.stem + '.obj')
        r = subprocess.run([cc] + flags + [str(cpp), '/Fo' + str(obj)], env=env,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode != 0:
            raise RuntimeError('compile failed (%s):\n%s' % (cpp.name, r.stdout[-6000:]))
        return obj

    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        objs = list(ex.map(compile_one, sorted(src.glob('*.cpp'))))
    dll = obj_dir / (sha + '.dll')
    cmd = [linker, '/nologo', '/DLL', '/INCREMENTAL:NO', '/OUT:' + str(dll), '/IMPLIB:' + str(obj_dir / (sha + '.lib'))]
    cmd += [str(o) for o in objs]
    cmd += [str(pathlib.Path(args.lib_dir) / n) for n in LINK_LIBS] + ['kernel32.lib']
    r = subprocess.run(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('link failed:\n' + r.stdout[-6000:])
    return dll


def build_with_cmake(args, env, src, sha):
    bdir = src / 'build'
    compiler = 'clang-cl' if args.compiler == 'clang' else 'cl'
    cfg = ['cmake', '-S', str(src), '-B', str(bdir), '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release',
           '-DCMAKE_CXX_COMPILER=' + compiler, '-DPSPC_SRC_DIR=' + str(ROOT / 'src').replace('\\', '/'),
           '-DPSPC_LIB_DIR=' + str(args.lib_dir).replace('\\', '/'),
           '-DPSPC_GEN_DIR=' + str(SEM_HEADER.parent).replace('\\', '/')]
    r = subprocess.run(cfg, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('cmake configure failed:\n' + r.stdout[-4000:])
    r = subprocess.run(['cmake', '--build', str(bdir), '--parallel', str(args.jobs)], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('compile failed:\n' + r.stdout[-6000:])
    return bdir / (sha + '.dll')


def build_one(args, env, path):
    work = pathlib.Path(args.work)
    work.mkdir(parents=True, exist_ok=True)
    elf = plain_elf(path, work, pathlib.Path(args.decrypt))
    sha = hashlib.sha1(elf).hexdigest()
    with _seen_lock:
        _module_sha[path] = sha
        if sha in _seen:
            return '%s: same module as another input (%s)' % (path.name, sha)
        _seen.add(sha)
    dest = pathlib.Path(args.cache) / (sha + '.dll')
    hints = pathlib.Path(args.cache) / (sha + '.hints')
    stamp = pathlib.Path(args.cache) / (sha + '.semver')
    stale = hints.exists() and dest.exists() and hints.stat().st_mtime > dest.stat().st_mtime
    if not stamp.exists() or stamp.read_text().strip() != args.sem_version:
        stale = True
    if dest.exists() and not args.force and not stale:
        return '%s: up to date (%s)' % (path.name, sha)
    elf_path = work / (sha + '.elf')
    elf_path.write_bytes(elf)
    src = work / sha
    if src.exists():
        shutil.rmtree(src)
    cmd = [str(BIN / 'pspc-recomp.exe'), str(elf_path), '-o', str(src)]
    if args.hints:
        cmd += ['--hints', args.hints]
    elif hints.exists():
        # Entry points the runtime saw the interpreter reach inside the previous build.
        cmd += ['--hints', str(hints)]
    if args.no_comments:
        cmd.append('--no-comments')
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if r.returncode != 0:
        raise RuntimeError('pspc-recomp failed:\n' + r.stdout)
    summary = r.stdout.strip()
    dll = build_with_cmake(args, env, src, sha) if args.cmake else build_direct(args, env, src, sha)
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(dll, dest)
    stamp.write_text(args.sem_version + '\n')
    if not args.keep_src:
        shutil.rmtree(src, ignore_errors=True)
        elf_path.unlink(missing_ok=True)
    return '%s -> %s\n%s' % (path.name, dest, summary)


def main():
    global BIN, LIB, SEM_HEADER
    # @file reads one argument per line (long module lists).
    ap = argparse.ArgumentParser(fromfile_prefix_chars='@')
    ap.add_argument('inputs', nargs='+')
    ap.add_argument('--build-dir', default=None,
                    help='build tree for pspc-recomp, the link libraries and the semantics version '
                         '(default: build/RelWithDebInfo)')
    ap.add_argument('--cache', default=None, help='AOT cache (default: <build dir>/bin/cache/aot)')
    ap.add_argument('--work', default=str(ROOT / 'build' / 'aot-work'))
    ap.add_argument('--decrypt', default=None)
    ap.add_argument('--compiler', choices=['clang', 'msvc'], default='clang')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--modules-in-parallel', type=int, default=1)
    ap.add_argument('--hints', default=None)
    ap.add_argument('--no-comments', action='store_true')
    ap.add_argument('--keep-src', action='store_true')
    ap.add_argument('--force', action='store_true')
    ap.add_argument('--cmake', action='store_true', help='build through the generated CMakeLists.txt')
    args = ap.parse_args()
    if args.build_dir:
        tree = pathlib.Path(args.build_dir).resolve()
        BIN, LIB, SEM_HEADER = tree / 'bin', tree / 'lib', tree / 'generated' / 'pspc_sem_version.h'
    args.cache = args.cache or str(BIN / 'cache' / 'aot')
    args.decrypt = args.decrypt or str(BIN / 'pspc-decrypt.exe')
    env = vs_env()
    args.sem_version = sem_version()
    args.lib_dir = snapshot_libs(pathlib.Path(args.work), args.sem_version)
    origins, ok = {}, {}
    paths = expand_inputs(args, [pathlib.Path(p) for p in args.inputs], origins)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.modules_in_parallel) as ex:
        futs = {ex.submit(build_one, args, env, p): p for p in paths}
        for f in concurrent.futures.as_completed(futs):
            try:
                print(f.result())
                ok[futs[f]] = True
            except Exception as e:  # noqa: BLE001 - report and continue with other modules
                failures += 1
                print('FAILED %s: %s' % (futs[f], e))
    try:
        write_index(args.cache, origins, ok)
    except Exception as e:  # noqa: BLE001 - the index is a convenience for the launcher
        print('warning: games.json not updated: %s' % e)
    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
