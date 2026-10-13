#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""End-to-end tests of PSPC's network libraries.

Usage: net_selftest.py [--pspc PATH] [--timeout S] [--internet] [scenario ...]

Each scenario runs `pspc --net-selftest <name>` (src/runtime/hle/modules/net_selftest.h): a test
program that calls the network libraries like a game does, on guest threads with guest memory.
Helper servers a scenario needs (a local HTTP server, the ad hoc server, a second instance) are
started here and always stopped at the end. Everything talks over loopback unless --internet is
given (then the http scenario also fetches http://example.com/ and https://example.com/).

Scenarios: inet, http, adhoc, adhoc-gamemode, adhoc-dialog, adhoc-noserver, adhoc-offline. Exit status
0 when all selected scenarios pass.

adhoc / adhoc-gamemode start `pspc --adhoc-server` on a free loopback port and two instances (host
and client) against it, which find each other through the server and play over loopback
(127.0.0.<instance>); adhoc-offline checks network=0.
"""
import argparse
import http.server
import pathlib
import socket
import socketserver
import subprocess
import sys
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent


def run_pspc(args, scenario, extra=(), timeout=60):
    """Runs one selftest instance; returns (exit code, stdout, stderr)."""
    cmd = [args.pspc, '--config', 'none', '--headless', '--timeout', str(timeout), '--log', 'warn']
    if args.fast_time:
        cmd.append('--fast-time')
    cmd += list(extra) + ['--net-selftest', scenario]
    try:
        p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout + 15)
        return p.returncode, p.stdout.decode('utf-8', 'replace'), p.stderr.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b'').decode('utf-8', 'replace')
        return -1, out, 'host timeout'


def report(name, code, out, err, verbose):
    ok = code == 0 and 'passed (0 failures)' in out
    if verbose or not ok:
        sys.stdout.write(out)
        if not ok and err.strip():
            sys.stdout.write('--- stderr (tail) ---\n' + '\n'.join(err.strip().splitlines()[-15:]) + '\n')
    print('%s %s (exit %s)' % ('PASS' if ok else 'FAIL', name, code))
    return ok


# ---------------------------------------------------------------- inet

def scenario_inet(args):
    code, out, err = run_pspc(args, 'inet', timeout=args.timeout)
    return report('inet', code, out, err, args.verbose)


# ---------------------------------------------------------------- http

HTTP_BODY = b'Hello from the PSPC net selftest.\n'
BIG_BODY = bytes((i * 7) & 0xFF for i in range(200 * 1024))


class Handler(http.server.BaseHTTPRequestHandler):
    protocol_version = 'HTTP/1.1'

    def log_message(self, *a):
        pass

    def send_body(self, code, body, ctype='text/plain', extra=()):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        for k, v in extra:
            self.send_header(k, v)
        self.end_headers()
        if self.command != 'HEAD':
            self.wfile.write(body)

    def do_GET(self):
        if self.path == '/hello.txt':
            ua = self.headers.get('User-Agent', '')
            self.send_body(200, HTTP_BODY, extra=[('X-Selftest', 'yes'), ('X-Agent-Seen', ua)])
        elif self.path == '/redirect':
            self.send_response(302)
            self.send_header('Location', '/hello.txt')
            self.send_header('Content-Length', '0')
            self.end_headers()
        elif self.path == '/big':
            self.send_body(200, BIG_BODY, 'application/octet-stream')
        elif self.path == '/chunked':
            self.send_response(200)
            self.send_header('Transfer-Encoding', 'chunked')
            self.end_headers()
            for part in (b'chunk one, ', b'chunk two, ', b'end.'):
                self.wfile.write(b'%x\r\n%s\r\n' % (len(part), part))
            self.wfile.write(b'0\r\n\r\n')
        elif self.path == '/missing':
            self.send_body(404, b'not here')
        else:
            self.send_body(404, b'unknown')

    def do_HEAD(self):
        self.do_GET()

    def do_POST(self):
        n = int(self.headers.get('Content-Length', '0'))
        data = self.rfile.read(n)
        self.send_body(200, b'got ' + data)


class ThreadingServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True

    def handle_error(self, request, client_address):
        pass  # a client closing a keep-alive connection isn't an error here


def scenario_http(args):
    server = ThreadingServer(('127.0.0.1', 0), Handler)
    port = server.server_address[1]
    t = threading.Thread(target=server.serve_forever, daemon=True)
    t.start()
    try:
        spec = 'http:http://127.0.0.1:%d' % port
        if args.internet:
            spec += ',internet'
        code, out, err = run_pspc(args, spec, timeout=args.timeout)
        return report('http', code, out, err, args.verbose)
    finally:
        server.shutdown()
        server.server_close()


# ---------------------------------------------------------------- ad hoc

def start_adhoc_server(args):
    """Starts `pspc --adhoc-server` on a free loopback port; returns (process, port, log lines)."""
    lines = []
    p = subprocess.Popen([args.pspc, '--adhoc-server', '--adhoc-port', '0', '--adhoc-bind', '127.0.0.1'],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    port = [None]
    ready = threading.Event()

    def reader():
        for raw in p.stdout:
            line = raw.decode('utf-8', 'replace').rstrip()
            lines.append(line)
            if 'listening on TCP port' in line:
                port[0] = int(line.rsplit(' ', 1)[1])
                ready.set()
        ready.set()

    threading.Thread(target=reader, daemon=True).start()
    if not ready.wait(15) or port[0] is None:
        p.kill()
        raise RuntimeError('the ad hoc server did not start: ' + ' | '.join(lines))
    return p, port[0], lines


def start_pspc(args, scenario, extra=(), timeout=60):
    cmd = [args.pspc, '--config', 'none', '--headless', '--timeout', str(timeout), '--log', 'warn']
    if args.fast_time:
        cmd.append('--fast-time')
    cmd += list(extra) + ['--net-selftest', scenario]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def finish_pspc(p, timeout):
    try:
        out, err = p.communicate(timeout=timeout)
        return p.returncode, out.decode('utf-8', 'replace'), err.decode('utf-8', 'replace')
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return -1, out.decode('utf-8', 'replace'), 'host timeout\n' + err.decode('utf-8', 'replace')


def adhoc_pair(args, scenario):
    """Server + host + client; both instances must pass."""
    procs = []
    server = None
    try:
        server, port, server_log = start_adhoc_server(args)
        extra = ['--set', 'network=1', '--set', 'adhoc_server=127.0.0.1:%d' % port]
        procs.append(start_pspc(args, scenario + ':host', extra, args.timeout))
        time.sleep(0.5)
        procs.append(start_pspc(args, scenario + ':client', extra, args.timeout))
        results = [finish_pspc(p, args.timeout + 15) for p in procs]
        ok = True
        for role, (code, out, err) in zip(('host', 'client'), results):
            ok = report('%s %s' % (scenario, role), code, out, err, args.verbose) and ok
        if args.verbose or not ok:
            print('--- ad hoc server (port %d) ---\n%s' % (port, '\n'.join(server_log)))
        return ok
    finally:
        for p in procs + ([server] if server else []):
            if p.poll() is None:
                p.kill()
            p.wait()


def scenario_adhoc(args):
    return adhoc_pair(args, 'adhoc')


def scenario_adhoc_gamemode(args):
    return adhoc_pair(args, 'adhoc-gamemode')


def scenario_adhoc_dialog(args):
    server = None
    try:
        server, port, server_log = start_adhoc_server(args)
        extra = ['--set', 'network=1', '--set', 'adhoc_server=127.0.0.1:%d' % port]
        code, out, err = run_pspc(args, 'adhoc-dialog', extra=extra, timeout=args.timeout)
        ok = report('adhoc-dialog', code, out, err, args.verbose)
        if args.verbose or not ok:
            print('--- ad hoc server (port %d) ---\n%s' % (port, '\n'.join(server_log)))
        return ok
    finally:
        if server:
            server.kill()
            server.wait()


def scenario_adhoc_noserver(args):
    with socket.socket() as s:  # a loopback port nobody listens on
        s.bind(('127.0.0.1', 0))
        port = s.getsockname()[1]
    extra = ['--set', 'network=1', '--set', 'adhoc_server=127.0.0.1:%d' % port]
    code, out, err = run_pspc(args, 'adhoc-noserver', extra=extra, timeout=args.timeout)
    return report('adhoc-noserver', code, out, err, args.verbose)


def scenario_adhoc_offline(args):
    code, out, err = run_pspc(args, 'adhoc-offline', extra=['--set', 'network=0'], timeout=args.timeout)
    return report('adhoc-offline', code, out, err, args.verbose)


SCENARIOS = {
    'inet': scenario_inet,
    'http': scenario_http,
    'adhoc': scenario_adhoc,
    'adhoc-gamemode': scenario_adhoc_gamemode,
    'adhoc-dialog': scenario_adhoc_dialog,
    'adhoc-noserver': scenario_adhoc_noserver,
    'adhoc-offline': scenario_adhoc_offline,
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--pspc', default=str(ROOT / 'build' / 'RelWithDebInfo' / 'bin' / ('pspc.exe' if sys.platform == 'win32' else 'pspc')))
    ap.add_argument('--timeout', type=int, default=60)
    ap.add_argument('--fast-time', action='store_true', help='run pspc with --fast-time')
    ap.add_argument('--internet', action='store_true', help='also fetch pages from the internet (http)')
    ap.add_argument('--verbose', '-v', action='store_true')
    ap.add_argument('scenarios', nargs='*')
    args = ap.parse_args()
    args.pspc = str(pathlib.Path(args.pspc).resolve())
    names = args.scenarios or list(SCENARIOS)
    failed = 0
    for n in names:
        if n not in SCENARIOS:
            print('unknown scenario %s (have: %s)' % (n, ', '.join(SCENARIOS)))
            return 2
        start = time.time()
        if not SCENARIOS[n](args):
            failed += 1
        print('  %s took %.1fs' % (n, time.time() - start))
    print('%d / %d scenarios passed' % (len(names) - failed, len(names)))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
