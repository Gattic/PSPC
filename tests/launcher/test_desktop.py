#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real installed desktop registration/activation. --gui also opens/closes the owned GTK window.

CLI checks require desktop-file-validate and gio. GUI checks require X11 and PyGObject's
GioUnix/Gdk/Atspi typelibs; absent prerequisites fail rather than silently skip.
"""
import argparse
import os
from pathlib import Path
import runpy
import shutil
import signal
import subprocess
import sys
import tempfile
import time

ID = 'io.github.Gattic.PSPC.desktop'


def run(args, env, cwd, expected=0):
    result = subprocess.run(list(map(str, args)), env=env, cwd=cwd, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=20)
    assert result.returncode == expected, (args, result.returncode, result.stdout)
    return result.stdout


def gui_launch(entry, prefix, env, work):
    import gi
    gi.require_version('GioUnix', '2.0')
    from gi.repository import Gio, GioUnix, GLib
    owned_window = runpy.run_path(str(Path(__file__).with_name('test_ui.py')))['owned_window']
    app = GioUnix.DesktopAppInfo.new_from_filename(str(entry))
    assert app and app.get_icon().to_string() == str(prefix / 'data/pspc.png')
    context = Gio.AppLaunchContext()
    for key, value in env.items():
        context.setenv(key, value)
    children = []
    log_path = work / 'desktop-gui.log'
    with log_path.open('w') as log, open(os.devnull) as null:
        # Ubuntu's typelib exposes a function; Fedora's exposes a method. An
        # explicit receiver preserves the same launch/reaping contract in both.
        assert GioUnix.DesktopAppInfo.launch_uris_as_manager_with_fds(
            app, [], context, GLib.SpawnFlags.DO_NOT_REAP_CHILD, None, None,
            lambda _app, pid, _data: children.append(pid), None,
            null.fileno(), log.fileno(), log.fileno())
        assert len(children) == 1, children
        pid = children[0]
        reaped = False
        try:
            deadline = time.monotonic() + 15
            while 'Launcher ready:' not in log_path.read_text():
                exited, status = os.waitpid(pid, os.WNOHANG)
                if exited:
                    reaped = True
                    raise AssertionError((status, log_path.read_text()))
                assert time.monotonic() < deadline, log_path.read_text()
                time.sleep(0.05)
            assert Path(f'/proc/{pid}/exe').resolve() == prefix / 'pspc-launcher'
            title = run([prefix / 'pspc', '--version'], env, work).strip()
            owned_window(pid, title, close=True)
            deadline = time.monotonic() + 10
            while True:
                exited, status = os.waitpid(pid, os.WNOHANG)
                if exited:
                    reaped = True
                    assert os.waitstatus_to_exitcode(status) == 0, log_path.read_text()
                    break
                assert time.monotonic() < deadline, 'desktop-launched GTK did not close'
                time.sleep(0.05)
        finally:
            if not reaped:
                os.kill(pid, signal.SIGKILL)
                os.waitpid(pid, 0)
            GLib.spawn_close_pid(pid)
    print('GIO desktop activation opened the actual GTK launcher and owned close exited 0.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--gui', action='store_true')
    args = parser.parse_args()
    if args.gui and not os.environ.get('DISPLAY'):
        parser.error('--gui requires X11 (use dbus-run-session -- xvfb-run -a)')
    fixture = runpy.run_path(str(Path(__file__).with_name('install_fixture.py')))
    with tempfile.TemporaryDirectory(prefix='pspc desktop ') as temp:
        work = Path(temp)
        stage = work / 'stage'
        fixture['stage'](args.build_dir.resolve(), stage)
        prefix = work / 'package λ %f "quotes" \\backslash $HOME `literal`'
        stage.rename(prefix)
        before = fixture['snapshot'](prefix)
        fixture['readonly'](prefix)
        try:
            for mode in ('explicit', 'defaults', 'relative'):
                home = work / mode
                home.mkdir()
                env = dict(os.environ, HOME=str(home), GDK_BACKEND='x11', GSETTINGS_BACKEND='memory',
                           XDG_CONFIG_HOME=str(home / 'config'), XDG_CACHE_HOME=str(home / 'cache'),
                           XDG_STATE_HOME=str(home / 'state'), NO_AT_BRIDGE='1')
                env.pop('XDG_DATA_HOME', None)
                data = home / '.local/share'
                if mode == 'explicit':
                    data = home / 'data λ'
                    env['XDG_DATA_HOME'] = str(data)
                elif mode == 'relative':
                    env['XDG_DATA_HOME'] = 'relative-must-not-be-used'
                helper = prefix / 'tools/pspc_desktop.py'
                command = [sys.executable, '-I', helper]
                entry = data / 'applications' / ID
                saved = data / 'pspc/ms0/keep.bin'
                saved.parent.mkdir(parents=True)
                saved.write_bytes(b'do-not-touch-user-data')
                config = home / 'config/pspc/pspc.ini'
                config.parent.mkdir(parents=True)
                config.write_text('host_network=0\nhost_sfco=0\nhost_adhoc=0\nhost_gamespy=0\nnetwork=0\n')
                run(command + ['install'], env, work)
                original = entry.read_bytes()
                run(command + ['install'], env, work)
                assert entry.read_bytes() == original
                run(['desktop-file-validate', entry], env, work)
                headless = env.copy()
                headless.pop('DISPLAY', None)
                headless.pop('WAYLAND_DISPLAY', None)
                # GIO really spawns the installed executable; this deliberate no-display path exits promptly.
                output = run(['gio', 'launch', entry], headless, work)
                assert 'Cannot open the display; --list works without a display.' in output, output
                if args.gui and mode == 'explicit':
                    gui_launch(entry, prefix, env, work)
                run(command + ['remove'], env, work)
                run(command + ['remove'], env, work)
                assert not entry.exists() and saved.read_bytes() == b'do-not-touch-user-data'
                entry.write_text('[Desktop Entry]\nName=Unmanaged\n')
                foreign = entry.read_bytes()
                run(command + ['install'], env, work, expected=1)
                run(command + ['remove'], env, work, expected=1)
                assert entry.read_bytes() == foreign
                entry.unlink()
                target = home / 'foreign.desktop'
                target.write_bytes(foreign)
                entry.symlink_to(target)
                run(command + ['install'], env, work, expected=1)
                run(command + ['remove'], env, work, expected=1)
                assert entry.is_symlink() and target.read_bytes() == foreign
                entry.unlink()
            # Moving/upgrading replaces a managed entry. An obsolete installation cannot remove it.
            run(command + ['install'], env, work)
            old_prefix = prefix
            prefix = work / 'moved λ'
            old_prefix.rename(prefix)
            helper = prefix / 'tools/pspc_desktop.py'
            run([sys.executable, '-I', helper, 'install'], env, work)
            updated = entry.read_bytes()
            old_helper = old_prefix / 'tools/pspc_desktop.py'
            old_helper.parent.mkdir(parents=True)
            shutil.copy2(helper, old_helper)
            run([sys.executable, '-I', old_helper, 'remove'], env, work, expected=1)
            assert entry.read_bytes() == updated
            entry.write_bytes(updated + b'# user edit\n')
            run([sys.executable, '-I', helper, 'remove'], env, work, expected=1)
            assert entry.read_bytes() == updated + b'# user edit\n'
            entry.write_bytes(updated)
            run([sys.executable, '-I', helper, 'remove'], env, work)
            assert not entry.exists() and saved.read_bytes() == b'do-not-touch-user-data'
            invalid = dict(env, HOME='relative', XDG_DATA_HOME='relative')
            run([sys.executable, '-I', helper, 'install'], invalid, work, expected=1)
            assert not (work / 'relative-must-not-be-used').exists()
            invalid_helper = work / 'unsupported=path/tools/pspc_desktop.py'
            invalid_helper.parent.mkdir(parents=True)
            shutil.copy2(helper, invalid_helper)
            output = run([sys.executable, '-I', invalid_helper, 'install'], env, work, expected=1)
            assert 'without = or control characters' in output and not entry.exists()
            assert fixture['snapshot'](prefix) == before, 'desktop integration modified the installation'
        finally:
            fixture['writable'](prefix)
    print('Desktop registration/validation/GIO activation, XDG defaults, quoting, move/update and safe removal passed.')


if __name__ == '__main__':
    main()
