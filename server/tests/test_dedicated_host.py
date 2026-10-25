# SPDX-License-Identifier: GPL-2.0-or-later
"""deploy/swss/dedicated-host.sh starts pspc as a session: with the account typed on the game's keyboards, in order,
a profile made first on a new memory stick, and nothing between the server and pspc (the server signals its child only)."""

import json
import os
import stat
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, '..', 'deploy', 'swss', 'dedicated-host.sh')

# A pspc that writes down how it was started; the profile exists afterwards when SAVE_PROFILE is set, as the game saves one.
STUB = """#!/bin/sh
echo "$$ $XDG_CONFIG_HOME $*" >> "$DEDICATED/runs.txt"
if [ -n "${SAVE_PROFILE:-}" ]; then
	mkdir -p "$DEDICATED/ms0/PSP/SAVEDATA/ULUS993900000"
	: > "$DEDICATED/ms0/PSP/SAVEDATA/ULUS993900000/DATA.BIN"
fi
exit 124
"""


@unittest.skipIf(sys.platform == 'win32', 'a shell script for the Linux server')
class DedicatedHostTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dir = os.path.join(tmp.name, 'dedicated')
        os.makedirs(self.dir)
        pspc = os.path.join(tmp.name, 'pspc')
        with open(pspc, 'w') as f:
            f.write(STUB)
        os.chmod(pspc, os.stat(pspc).st_mode | stat.S_IXUSR)
        sessions = os.path.join(tmp.name, 'sessions.json')
        with open(sessions, 'w') as f:
            json.dump({'accounts': [
                {'nick': 'Other', 'email': 'o@example.com', 'password': 'x', 'namespace': 1},
                {'nick': 'Ded-Conquest', 'email': 'ded@example.com', 'password': 'pa ss|w', 'namespace': 1}]}, f)
        self.env = dict(os.environ, PSPC=pspc, ISO='/games/SWSS.iso', DEDICATED=self.dir, SESSIONS=sessions, HOURS='1')
        self.env.pop('SAVE_PROFILE', None)
        self.env.pop('UDP_PORT', None)
        self.env.pop('PLAYLIST', None)

    def start(self, nick='Ded-Conquest'):
        return subprocess.Popen(['sh', SCRIPT, nick], env=self.env, stderr=subprocess.PIPE, text=True)

    def run_script(self, nick='Ded-Conquest'):
        p = self.start(nick)
        _, stderr = p.communicate(timeout=60)
        return p, stderr

    def runs(self):
        """The pspc runs so far: pid, XDG_CONFIG_HOME, the rest of the command line."""
        with open(os.path.join(self.dir, 'runs.txt')) as f:
            return [line.split(' ', 2) for line in f.read().splitlines()]

    def make_profile(self):
        d = os.path.join(self.dir, 'ms0', 'PSP', 'SAVEDATA', 'ULUS993900000')
        os.makedirs(d)
        open(os.path.join(d, 'DATA.BIN'), 'w').close()

    def test_the_account_goes_to_the_keyboards_in_order_and_the_game_runs_until_the_end(self):
        self.make_profile()
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        (_, config, args), = self.runs()
        self.assertEqual(config, os.path.join(self.dir, 'xdg-config'))
        self.assertIn('--headless --renderer none', args)
        self.assertIn('--memstick %s/ms0' % self.dir, args)
        self.assertIn('--osk-text Ded-Conquest|ded@example.com|pa ss|w|SWSS Conquest --exit-vblank 216000', args)
        presses = [part.split()[0] for part in args.split(' --press ')[1:]]
        self.assertEqual(presses[0], '2500:START:6')
        self.assertIn('14200:START:6', presses)
        self.assertEqual(presses[-1], '215900:CROSS:6')

    def test_a_new_memory_stick_gets_its_profile_first(self):
        self.env['SAVE_PROFILE'] = '1'
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        (setup_pid, _, setup), (main_pid, _, main) = self.runs()
        self.assertIn('--osk-text Dedicated --exit-vblank 7000', setup)
        self.assertIn('--osk-text Ded-Conquest|ded@example.com|pa ss|w|SWSS Conquest', main)
        self.assertNotEqual(setup_pid, main_pid)

    def test_a_given_udp_port_is_bound_instead_of_the_games_own(self):
        # The game asks for 3658: a session that forwards another port has pspc bind that one instead.
        self.make_profile()
        self.env['UDP_PORT'] = '3659'
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        (_, _, args), = self.runs()
        self.assertIn('--set game_port=3659', args)
        self.assertIn('--set firmware=4.01', args)

    def test_without_a_udp_port_the_game_keeps_the_port_it_asks_for(self):
        self.make_profile()
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        (_, _, args), = self.runs()
        self.assertNotIn('game_port', args)

    def test_a_udp_port_that_is_not_a_port_is_refused(self):
        self.make_profile()
        for port in ('abc', '0', '65536'):
            with self.subTest(port=port):
                self.env['UDP_PORT'] = port
                p, stderr = self.run_script()
                self.assertEqual(p.returncode, 1, stderr)
                self.assertIn('UDP_PORT must be', stderr)
        self.assertFalse(os.path.exists(os.path.join(self.dir, 'runs.txt')))

    def test_a_profile_that_was_not_saved_stops_the_script_before_the_game_starts(self):
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 1)
        self.assertIn('profile was not saved', stderr)
        self.assertEqual(len(self.runs()), 1)

    def presses(self, args):
        return [part.split()[0] for part in args.split(' --press ')[1:]]

    def test_a_playlist_replaces_the_random_button_and_starts_after_it(self):
        # The game's driver adds an entry with four CROSS steps (location, mode, era, area) on the Game Type screen, which
        # is ready 1400 VBlanks after Continue (11000); 0.0.0.0 is the first planet, its first mode, prequel, ground.
        self.make_profile()
        self.env['PLAYLIST'] = '0.0.0.0'
        self.env['SAVE_PROFILE'] = '1'
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        _, _, args = self.runs()[-1]
        presses = self.presses(args)
        self.assertNotIn('11600:SQUARE:6', presses)
        self.assertEqual([x for x in presses if x.startswith(('13300:', '14000:', '14800:', '15600:'))],
                         ['13300:CROSS:6', '14000:CROSS:6', '14800:CROSS:6', '15600:CROSS:6'])
        self.assertIn('16400:START:6', presses)
        self.assertIn('18600:CROSS:6', presses)
        self.assertEqual(presses[-1], '215600:CROSS:6')

    def test_a_playlist_entry_presses_down_to_its_location_mode_era_and_area(self):
        # 22.4.0.0: Mustafar (the 23rd planet code), Heroes vs Villains (its fifth mode), prequel, ground. Each step is a
        # DOWN and then a CROSS, the location's DOWNs 40 VBlanks apart and the mode's 60, as the game's driver does.
        self.make_profile()
        self.env['PLAYLIST'] = '22.4.0.0'
        self.env['SAVE_PROFILE'] = '1'
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        _, _, args = self.runs()[-1]
        presses = self.presses(args)
        self.assertEqual([x for x in presses if x.endswith('DOWN:6') and int(x.split(':')[0]) >= 13000],
                         ['%d:DOWN:6' % (13000 + 40 * i) for i in range(22)]
                         + ['%d:DOWN:6' % (14680 + 60 * i) for i in range(4)])
        self.assertEqual([x for x in presses if x.startswith(('14180:', '15120:', '15920:', '16720:', '17520:'))],
                         ['14180:CROSS:6', '15120:CROSS:6', '15920:CROSS:6', '16720:CROSS:6', '17520:START:6'])

    def test_a_playlist_makes_a_new_profile_so_the_playlist_starts_empty(self):
        # The game keeps its playlist in the profile: a session that is given one deletes the profile and the game makes
        # a new one, whose playlist is empty, before the run that fills it.
        self.make_profile()
        self.env['PLAYLIST'] = '0.0.0.0'
        self.env['SAVE_PROFILE'] = '1'
        p, stderr = self.run_script()
        self.assertEqual(p.returncode, 124, stderr)
        (setup_pid, _, setup), (main_pid, _, main) = self.runs()
        self.assertIn('--exit-vblank 7000', setup)
        self.assertIn('--osk-text Ded-Conquest|ded@example.com|pa ss|w|SWSS Conquest', main)
        self.assertNotEqual(setup_pid, main_pid)

    def test_a_playlist_that_is_not_indices_is_refused(self):
        self.make_profile()
        for playlist in ('35.0.0.0', '0.5.0.0', '0.0.3.0', '0.0.0.2', '0.0.0', '22.4.0.0.x', 'x', '22.04.0.0'):
            with self.subTest(playlist=playlist):
                self.env['PLAYLIST'] = playlist
                p, stderr = self.run_script()
                self.assertEqual(p.returncode, 1, stderr)
                self.assertIn('PLAYLIST', stderr)
        self.assertFalse(os.path.exists(os.path.join(self.dir, 'runs.txt')))

    def test_pspc_replaces_the_script(self):
        # The server signals the process it started: a pspc left running by a shell would outlive the server's stop.
        self.make_profile()
        p, _ = self.run_script()
        (pid, _, _), = self.runs()
        self.assertEqual(int(pid), p.pid)

    def test_an_account_the_sessions_file_does_not_have_is_refused(self):
        self.make_profile()
        p, stderr = self.run_script('Nobody')
        self.assertNotEqual(p.returncode, 0)
        self.assertFalse(os.path.exists(os.path.join(self.dir, 'runs.txt')))


if __name__ == '__main__':
    unittest.main()
