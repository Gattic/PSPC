# SPDX-License-Identifier: GPL-2.0-or-later
"""PSPC Network: a stand-in for the PlayStation Network for games running in PSPC.

Accounts with passwords, sign-in sessions, NP tickets in Sony's layout signed by this server, and
friend lists, over a small JSON API. Game revival servers check a player's ticket with it. Standard
library only (Python 3.9+).
"""

__version__ = '1.0'
