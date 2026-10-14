#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Opt-in GTK workflow check on a real X11/AT-SPI desktop.

Uses one disposable candidate installation, synthetic ELF/settings/plugins and a
loopback-only account server, a read-only relocated payload and isolated XDG writes.
Never targets windows belonging to another PID.
Run with --build-dir and --evidence-dir; needs PyGObject, Atspi 2 and Gdk 3.
"""

import argparse
import ctypes as c
import json
import os
from pathlib import Path
import runpy
import shutil
import signal
import socket
import struct
import subprocess
import tempfile
import time

# PyGObject may initialize GDK while loading its overrides.
os.environ["GDK_BACKEND"] = "x11"
import gi

gi.require_version("Atspi", "2.0")
gi.require_version("Gdk", "3.0")
gi.require_version("GdkX11", "3.0")
from gi.repository import Atspi, Gdk, GdkX11, GLib

ROOT = Path(__file__).resolve().parents[2]


def walk(node):
    yield node
    for i in range(node.get_child_count()):
        child = node.get_child_at_index(i)
        if child:
            yield from walk(child)


def owned_window(pid, title, *, close=False):
    """Find the owned X11 window; optionally send it WM_DELETE_WINDOW directly."""
    x = c.CDLL("libX11.so.6")
    D, W = c.c_void_p, c.c_ulong
    x.XOpenDisplay.argtypes = [c.c_char_p]
    x.XOpenDisplay.restype = D
    x.XDefaultRootWindow.argtypes = [D]
    x.XDefaultRootWindow.restype = W
    x.XInternAtom.argtypes = [D, c.c_char_p, c.c_int]
    x.XInternAtom.restype = W
    x.XQueryTree.argtypes = [
        D,
        W,
        c.POINTER(W),
        c.POINTER(W),
        c.POINTER(c.POINTER(W)),
        c.POINTER(c.c_uint),
    ]
    x.XGetWindowProperty.argtypes = [
        D,
        W,
        W,
        c.c_long,
        c.c_long,
        c.c_int,
        W,
        c.POINTER(W),
        c.POINTER(c.c_int),
        c.POINTER(W),
        c.POINTER(W),
        c.POINTER(c.POINTER(c.c_ubyte)),
    ]
    x.XFetchName.argtypes = [D, W, c.POINTER(c.c_char_p)]
    x.XFree.argtypes = [c.c_void_p]
    x.XFlush.argtypes = [D]
    x.XCloseDisplay.argtypes = [D]

    class Data(c.Union):
        _fields_ = [("bytes", c.c_char * 20), ("longs", c.c_long * 5)]

    class Client(c.Structure):
        _fields_ = [
            ("type", c.c_int),
            ("serial", W),
            ("send_event", c.c_int),
            ("display", D),
            ("window", W),
            ("message_type", W),
            ("format", c.c_int),
            ("data", Data),
        ]

    class Event(c.Union):
        _fields_ = [("client", Client), ("pad", c.c_long * 24)]

    x.XSendEvent.argtypes = [D, W, c.c_int, c.c_long, c.POINTER(Event)]
    d = x.XOpenDisplay(None)
    assert d
    pending = [x.XDefaultRootWindow(d)]
    atom = x.XInternAtom(d, b"_NET_WM_PID", 0)
    try:
        while pending:
            w = pending.pop()
            kind, count, after, fmt = W(), W(), W(), c.c_int()
            data = c.POINTER(c.c_ubyte)()
            x.XGetWindowProperty(
                d,
                w,
                atom,
                0,
                1,
                0,
                0,
                c.byref(kind),
                c.byref(fmt),
                c.byref(count),
                c.byref(after),
                c.byref(data),
            )
            owned = bool(
                data
                and fmt.value == 32
                and count.value
                and c.cast(data, c.POINTER(W))[0] == pid
            )
            if data:
                x.XFree(data)
            name = c.c_char_p()
            x.XFetchName(d, w, c.byref(name))
            matches = owned and name.value and name.value.decode() == title
            if name:
                x.XFree(name)
            if matches:
                if not close:
                    return w
                event = Event()
                event.client = Client(
                    33, 0, 1, d, w, x.XInternAtom(d, b"WM_PROTOCOLS", 0), 32, Data()
                )
                event.client.data.longs[0] = x.XInternAtom(d, b"WM_DELETE_WINDOW", 0)
                assert x.XSendEvent(d, w, 0, 0, c.byref(event))
                x.XFlush(d)
                return
            root, parent, n = W(), W(), c.c_uint()
            children = c.POINTER(W)()
            if x.XQueryTree(
                d, w, c.byref(root), c.byref(parent), c.byref(children), c.byref(n)
            ):
                pending.extend(children[i] for i in range(n.value))
                if children:
                    x.XFree(children)
        raise AssertionError("Owned main window not found")
    finally:
        x.XCloseDisplay(d)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--evidence-dir", type=Path, required=True)
    args = parser.parse_args()
    build = args.build_dir.resolve()
    evidence = args.evidence_dir.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    assert os.environ.get("DISPLAY"), "This opt-in check requires X11"
    with tempfile.TemporaryDirectory(prefix="candidate-", dir=evidence) as temporary:
        candidate = Path(temporary)
        fixture = runpy.run_path(str(Path(__file__).with_name("install_fixture.py")))
        staged = candidate / "stage"
        fixture["stage"](build, staged)
        binary = candidate / "relocated λ"
        staged.rename(binary)
        games = candidate / "games λ"
        games.mkdir()
        make_elf = runpy.run_path(str(ROOT / "tests/aot/test_pipeline.py"))["make_elf"]
        game = games / "synthetic λ.elf"
        # A bounded test-owned process will run this yielding native back edge until Stop game.
        game.write_bytes(make_elf(struct.pack("<2I", 0x1000FFFF, 0)))
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        home = candidate / "home"
        home.mkdir()
        config = home / "config/pspc"
        data = home / "data/pspc"
        cache = home / "cache/pspc"
        state = home / "state/pspc"
        config.mkdir(parents=True)
        (config / "pspc.ini").write_text(
            f"games_dir={games}\nhost_network_port={port}\nhost_network_lan=0\nrenderer=software\naot=1\nmute=1\nplugins=0\nnetwork=0\n"
        )
        plugins = data / "plugins/seplugins"
        plugins.mkdir(parents=True)
        (plugins / "game.txt").write_text("ms0:/seplugins/example.prx 1\n")
        before = fixture["snapshot"](binary)
        fixture["readonly"](binary)

        def move_tool(source, target):
            # Inject missing-tool failures, but keep the payload sealed while the app runs.
            binary.chmod(binary.stat().st_mode | 0o200)
            try:
                source.rename(target)
            finally:
                binary.chmod(binary.stat().st_mode & ~0o222)
        env = dict(
            os.environ,
            HOME=str(home),
            XDG_CONFIG_HOME=str(home / "config"),
            XDG_DATA_HOME=str(home / "data"),
            XDG_CACHE_HOME=str(home / "cache"),
            XDG_STATE_HOME=str(home / "state"),
            LC_ALL="C.UTF-8",
            GDK_BACKEND="x11",
            SDL_VIDEODRIVER="x11",
            GSETTINGS_BACKEND="memory",
            NO_AT_BRIDGE="0",
            PYTHONDONTWRITEBYTECODE="1",
        )
        with (evidence / "gtk-ui.log").open("w") as log:
            process = subprocess.Popen(
                [str(binary / "pspc-launcher"), str(games)],
                cwd=binary,
                env=env,
                stdout=log,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
            app = None

            def wait(predicate, seconds=10):
                deadline = time.monotonic() + seconds
                last = None
                while time.monotonic() < deadline:
                    assert process.poll() is None, (
                        f"launcher exited: {process.returncode}"
                    )
                    try:
                        result = predicate()
                        if result:
                            return result
                    except GLib.Error as error:
                        last = (
                            error  # Accessible objects can disappear as dialogs close.
                        )
                    time.sleep(0.05)
                raise AssertionError(f"UI deadline; last accessibility error: {last}")

            def nodes():
                context = GLib.MainContext.default()
                while context.pending():
                    context.iteration(False)
                return list(walk(app))

            def find(name, role=None):
                return next(
                    (
                        n
                        for n in nodes()
                        if n.get_name() == name
                        and (role is None or n.get_role_name() == role)
                    ),
                    None,
                )

            def click(name):
                node = wait(lambda: find(name, "button"))
                assert Atspi.Action.do_action(node, 0), name
                time.sleep(0.1)

            def text(name, value):
                node = wait(lambda: find(name, "text") or find(name, "password text"))
                assert Atspi.EditableText.set_text_contents(node, value), name

            def contains(value):
                for node in nodes():
                    if value in node.get_name():
                        return True
                    if node.get_role_name() in ("text", "label"):
                        if value in Atspi.Text.get_text(node, 0, -1):
                            return True
                return False

            try:
                desktop = Atspi.get_desktop(0)
                app = wait(
                    lambda: next(
                        (
                            desktop.get_child_at_index(i)
                            for i in range(desktop.get_child_count())
                            if desktop.get_child_at_index(i).get_process_id()
                            == process.pid
                        ),
                        None,
                    )
                )
                main_window = wait(
                    lambda: next(
                        (n for n in nodes() if n.get_role_name() == "frame"), None
                    )
                )
                main_title = main_window.get_name()
                wait(lambda: contains("synthetic λ"))
                click("Settings / Input")
                text("renderer", "opengl")
                text("render scale", "3")
                click("Save")
                saved = dict(
                    line.split("=", 1)
                    for line in (config / "pspc.ini").read_text().splitlines()
                    if "=" in line
                )
                saved = {key.strip(): value.strip() for key, value in saved.items()}
                assert saved["renderer"] == "opengl" and saved["render_scale"] == "3"
                click("Plugins")
                wait(lambda: contains("game.txt"))
                click("Save")
                decrypt = binary / "pspc-decrypt"
                disabled_decrypt = binary / "pspc-decrypt.disabled"
                move_tool(decrypt, disabled_decrypt)
                click("Install firmware")
                wait(lambda: contains("Firmware installation needs pspc-decrypt"))
                click("Close")
                move_tool(disabled_decrypt, decrypt)
                click("Install firmware")
                wait(lambda: contains("Sony PSP 4.01 update EBOOT.PBP"))
                click("Cancel")
                assert not (data / "flash0").exists()
                assert not list(cache.glob("firmware-*"))
                assert "example.prx 1" in (plugins / "game.txt").read_text()
                click("Online / Servers")
                click("Start")

                def listening():
                    try:
                        with socket.create_connection(("127.0.0.1", port), timeout=0.1):
                            return True
                    except OSError:
                        return False

                wait(listening)
                click("Status / log")
                wait(lambda: contains("State: running"))
                text("Redirect disc ID", "TEST00001")
                text("Service domain", "test.invalid")
                text("Target host (empty removes)", "127.0.0.1")
                click("Save route")
                assert (
                    json.loads((config / "infrastructure.json").read_text())["games"][
                        0
                    ]["domains"]["test.invalid"]
                    == "127.0.0.1"
                )
                click("Run admin")
                wait(lambda: contains("usage:"))
                click("Close")
                click("Account / Friends")
                text("Server URL", f"http://127.0.0.1:{port}")
                text("Online ID", "SyntheticPlayer")
                text("Password", "fixture-password-123")
                click("Register")
                wait(lambda: contains("Completed."))
                click("Refresh friends")
                wait(lambda: contains("No friends or requests."))
                click("Sign out")
                wait(lambda: contains("Completed."))
                click("Close")
                click("Play")
                wait(lambda: contains("Native code is missing"))
                click("Cancel")
                recomp = binary / "pspc-recomp"
                disabled_recomp = binary / "pspc-recomp.disabled"
                move_tool(recomp, disabled_recomp)
                click("Play")
                click("Build and play")
                build_log = state / "logs/native-build.log"
                wait(
                    lambda: (
                        build_log.exists() and b"--- exit " in build_log.read_bytes()
                    ),
                    seconds=30,
                )
                assert b"--- exit 0" not in build_log.read_bytes()
                wait(lambda: contains("AOT installation is incomplete"))
                assert not (state / "logs/runtime.log").exists(), (
                    "Failed build launched a runtime"
                )
                assert not list(cache.glob("native-work-*"))
                shutil.copy2(build_log, evidence / "gtk-native-build-failure.log")
                move_tool(disabled_recomp, recomp)
                click("Play")
                click("Build and play")
                wait(lambda: (cache / "aot/games.json").exists(), seconds=60)
                wait(lambda: contains("NATIVE"), seconds=20)
                index = json.loads((cache / "aot/games.json").read_text())
                module = index["games"][str(game)]["modules"][0]["sha1"]
                assert (cache / "aot" / f"{module}.so").is_file()
                runtime_log = state / "logs/runtime.log"
                wait(
                    lambda: (
                        runtime_log.exists()
                        and b"[opengl]" in runtime_log.read_bytes()
                        and b"[AOT] ELF: 1 native functions" in runtime_log.read_bytes()
                    ),
                    seconds=20,
                )

                def runtime_pid():
                    children = (
                        Path(f"/proc/{process.pid}/task/{process.pid}/children")
                        .read_text()
                        .split()
                    )
                    for child in children:
                        try:
                            if os.readlink(f"/proc/{child}/exe") == str(
                                binary / "pspc"
                            ):
                                return int(child)
                        except FileNotFoundError:
                            pass
                    return None

                running_pid = runtime_pid()
                assert running_pid
                completed_builds = build_log.read_bytes().count(b"--- exit 0 ---")
                try:
                    click("Rebuild native")
                    wait(
                        lambda: (
                            build_log.read_bytes().count(b"--- exit 0 ---")
                            > completed_builds
                        ),
                        seconds=60,
                    )
                    assert runtime_pid() == running_pid, (
                        "Rebuilding killed or replaced the running native game"
                    )
                    assert b"fatal host signal" not in runtime_log.read_bytes()
                finally:
                    shutil.copy2(build_log, evidence / "gtk-live-rebuild-build.log")
                    shutil.copy2(runtime_log, evidence / "gtk-live-rebuild-runtime.log")
                click("Stop game")
                wait(
                    lambda: (
                        not runtime_pid() and b"--- exit " in runtime_log.read_bytes()
                    )
                )
                assert not list(cache.glob("native-work-*"))
                assert (
                    b"Exited with code 0 after 0 interpreted instructions, 0 bad memory accesses"
                    in runtime_log.read_bytes()
                )
                shutil.copy2(runtime_log, evidence / "gtk-synthetic-runtime.log")
                Gdk.init([])
                window = GdkX11.X11Window.foreign_new_for_display(
                    Gdk.Display.get_default(), owned_window(process.pid, main_title)
                )
                image = Gdk.pixbuf_get_from_window(
                    window, 0, 0, window.get_width(), window.get_height()
                )
                assert image and len(set(image.get_pixels())) > 8, (
                    "Blank window capture"
                )
                image.savev(str(evidence / "gtk-ui.png"), "png", [], [])
                owned_window(process.pid, main_title, close=True)
                click("Yes")  # The owned account server is still running.
                assert process.wait(timeout=8) == 0
                assert not listening(), "server leaked after closing the launcher"
                fallback_env = dict(
                    env, MESA_EXTENSION_OVERRIDE="-GL_ARB_fragment_shader_interlock"
                )
                fallback_env.pop("PSPC_RENDERER_REQUIRE", None)
                with (evidence / "opengl-runtime-fallback.log").open(
                    "w"
                ) as fallback_log:
                    result = subprocess.run(
                        [
                            str(binary / "pspc"),
                            "--config",
                            "none",
                            "--headless",
                            "--no-plugins",
                            "--set",
                            "network=0",
                            "--renderer",
                            "opengl",
                            "--timeout",
                            "1",
                            str(game),
                        ],
                        cwd=binary,
                        env=fallback_env,
                        stdout=fallback_log,
                        stderr=subprocess.STDOUT,
                        timeout=15,
                    )
                fallback = (evidence / "opengl-runtime-fallback.log").read_text()
                assert result.returncode == 124, (
                    fallback
                )  # The synthetic self-loop exits via --timeout.
                assert (
                    "OpenGL renderer unavailable" in fallback
                    and "using software" in fallback
                ), fallback
                assert "[AOT] ELF: 1 native functions" in fallback, fallback
                assert "0 bad memory accesses" in fallback, fallback
                assert fixture["snapshot"](binary) == before, "GTK/runtime wrote into the installed payload"
                assert (data / "servers/pspc_network.sqlite").is_file()
                assert (state / "servers/pspc_network.log").is_file()
                print(
                    "Read-only relocated GTK UI: XDG settings, plugins, local account/server/admin/routing, build-before-play, live native rebuild, native startup/stop and owned close passed"
                )
            except Exception:
                if process.poll() is None:
                    with (evidence / "gtk-ui-stack.txt").open("w") as stack:
                        subprocess.run(
                            [
                                "gdb",
                                "-nx",
                                "-batch",
                                "-iex",
                                "set debuginfod enabled off",
                                "-ex",
                                "thread apply all bt",
                                "-p",
                                str(process.pid),
                            ],
                            stdout=stack,
                            stderr=subprocess.STDOUT,
                            timeout=15,
                        )
                if app and process.poll() is None:
                    with (evidence / "gtk-ui-tree.txt").open("w") as tree:
                        for node in nodes():
                            try:
                                tree.write(
                                    f"{node.get_role_name()}: {node.get_name()}\n"
                                )
                            except GLib.Error as error:
                                tree.write(f"accessibility error: {error}\n")
                                break
                raise
            finally:
                if process.poll() is None:
                    # Kill only process groups whose leaders are current children of this fixture.
                    children_file = Path(
                        f"/proc/{process.pid}/task/{process.pid}/children"
                    )
                    children = (
                        children_file.read_text().split()
                        if children_file.exists()
                        else []
                    )
                    for child in children:
                        try:
                            if os.getpgid(int(child)) == int(child):
                                os.killpg(int(child), signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    process.kill()
                    process.wait(timeout=5)
                fixture["writable"](binary)


if __name__ == "__main__":
    main()
