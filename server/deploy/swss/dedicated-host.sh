#!/bin/sh
# A dedicated SWSS game: PSPC without a window, a screen or a GPU, logged in to the server as one of the sessions file's
# accounts, hosting a Conquest match. The game has no server mode, so the menus are driven by timed button presses (the
# menus' timing is in VBlanks, which pspc counts itself, so a slow machine doesn't change it). The server runs this as a
# session (gamespy-sessions.json, see gamespy-sessions.example.json); it stops after HOURS hours, the session starts it again.
#
#   dedicated-host.sh NICK
#
# Environment:
#   PSPC      the pspc binary (required)
#   ISO       the SWSS disc image (required)
#   DEDICATED the folder with the instance's own xdg-config, xdg-data, xdg-cache and ms0 (default ~/swss-server/dedicated)
#   SESSIONS  the sessions file the account NICK is read from (default ~/swss-server/data/gamespy-sessions.json)
#   GAME_NAME the name the game is listed under (default "SWSS Conquest")
#   UDP_PORT  the UDP port the game binds its own (3658) to instead, so the router can forward it (default: 3658
#             while it is free, otherwise a port that changes at each start)
#   HOURS     how long to run (default 6)
set -eu
NICK=${1:?usage: dedicated-host.sh NICK}
: "${PSPC:?set PSPC to the pspc binary}" "${ISO:?set ISO to the SWSS disc image}"
DEDICATED=${DEDICATED:-$HOME/swss-server/dedicated}
SESSIONS=${SESSIONS:-$HOME/swss-server/data/gamespy-sessions.json}
GAME_NAME=${GAME_NAME:-SWSS Conquest}
UDP_PORT=${UDP_PORT:-}
HOURS=${HOURS:-6}
case $UDP_PORT in
''|*[!0-9]*) [ -z "$UDP_PORT" ] || { echo "UDP_PORT must be a port number, not '$UDP_PORT'" >&2; exit 1; } ;;
esac
if [ -n "$UDP_PORT" ] && { [ "$UDP_PORT" -lt 1 ] || [ "$UDP_PORT" -gt 65535 ]; }; then
	echo "UDP_PORT must be 1-65535, not '$UDP_PORT'" >&2; exit 1
fi
PROFILE=$DEDICATED/ms0/PSP/SAVEDATA/ULUS993900000/DATA.BIN

# run_pspc VBLANKS OSK PRESS...: pspc until that VBlank, its keyboards returning the OSK text (a|b|c: one for each in turn),
# each PRESS (VBLANK:BUTTON) held for 6 VBlanks from that VBlank. It replaces this shell, which the server stops with a signal.
run_pspc() {
	vblanks=$1 osk=$2
	shift 2
	for press in "$@"; do shift; set -- "$@" --press "$press:6"; done
	if [ -n "$UDP_PORT" ]; then set -- --set "game_port=$UDP_PORT" "$@"; fi
	exec env XDG_CONFIG_HOME="$DEDICATED/xdg-config" XDG_DATA_HOME="$DEDICATED/xdg-data" XDG_CACHE_HOME="$DEDICATED/xdg-cache" \
		"$PSPC" "$ISO" --headless --renderer none --memstick "$DEDICATED/ms0" --set firmware=4.01 \
		--osk-text "$osk" --exit-vblank "$vblanks" --log info "$@"
}

# A new memory stick has no profile: the game asks whether to load one (no), for the profile's name, the difficulty (the
# default) and whether to save it (yes). The main run below starts from a saved one.
if [ ! -e "$PROFILE" ]; then
	(run_pspc 7000 Dedicated 2500:START 3100:CROSS 3500:DOWN 3700:CROSS 4400:CROSS 4700:DOWN 4900:CROSS 5400:CROSS 6000:CROSS) || true
	[ -e "$PROFILE" ] || { echo "the game's profile was not saved on $DEDICATED/ms0" >&2; exit 1; }
fi

# The game's keyboards ask for, in turn: the nick, the e-mail address and the password, then the game's name. The password
# is on this process's command line, as readable to this machine's users as the sessions file's account is to its owner.
OSK=$(python3 -c '
import json, sys
accounts = json.load(open(sys.argv[1]))["accounts"]
a = next(a for a in accounts if a["nick"] == sys.argv[2])
print("|".join((a["nick"], a["email"], a["password"], sys.argv[3])))' "$SESSIONS" "$NICK" "$GAME_NAME")

# Start, skip the intro, load the profile, Multiplayer, Infrastructure, sign in, Create, name the game, pick the playlist
# (square: random), load the first map (start); then a cross every 500 VBlanks takes the team and the spawn point, in
# each round after the first too.
END=$((HOURS * 3600 * 60))
set -- 2500:START 3100:CROSS 3500:CROSS 4400:CROSS 5000:DOWN 5200:CROSS 5800:CROSS 6400:CROSS 6900:CROSS 7100:DOWN \
	7300:CROSS 7500:DOWN 7700:CROSS 7900:DOWN 8100:CROSS 9300:DOWN 9500:CROSS 10200:CROSS 10800:DOWN 11000:CROSS \
	11600:SQUARE 14200:START
for v in $(seq 16400 500 "$END"); do set -- "$@" "$v:CROSS"; done
run_pspc "$END" "$OSK" "$@"
