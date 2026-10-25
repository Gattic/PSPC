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
#   PLAYLIST  the entries the playlist is filled with instead of the random button (square), as LOCATION.MODE.ERA.AREA,
#             comma separated: 22.4.0.0 is Heroes vs Villains on Mustafar, prequel, ground. The game's own indices:
#             location is the planet's place among the disc's planet codes in sorted order (35 of them on the disc of
#             2026-10-24: 0 ALDR, 5 CRSS, 9 ENDR, 10 ENDS, 12 HOTH, 22 MSTR, 31 VJUN, 34 YAVS); mode counts the planet's
#             allowed game types (0 Conquest, 1 one-flag CTF, 2 two-flag CTF, 3 hero CTF, 4 Heroes vs Villains); era is
#             0 prequel, 1 classic, 2 campaign; area is the place among the areas offered (0 ground, 1 space where the
#             planet offers both). The game keeps its playlist in the profile, so a session that has PLAYLIST starts
#             from a new profile (whose playlist is empty) and adds each entry; without it the session presses square
#             and the game fills the playlist at random, as it always has (the Conquest sessions do).
#   HOURS     how long to run (default 6)
set -eu
NICK=${1:?usage: dedicated-host.sh NICK}
: "${PSPC:?set PSPC to the pspc binary}" "${ISO:?set ISO to the SWSS disc image}"
DEDICATED=${DEDICATED:-$HOME/swss-server/dedicated}
SESSIONS=${SESSIONS:-$HOME/swss-server/data/gamespy-sessions.json}
GAME_NAME=${GAME_NAME:-SWSS Conquest}
UDP_PORT=${UDP_PORT:-}
PLAYLIST=${PLAYLIST:-}
HOURS=${HOURS:-6}
case $UDP_PORT in
''|*[!0-9]*) [ -z "$UDP_PORT" ] || { echo "UDP_PORT must be a port number, not '$UDP_PORT'" >&2; exit 1; } ;;
esac
if [ -n "$UDP_PORT" ] && { [ "$UDP_PORT" -lt 1 ] || [ "$UDP_PORT" -gt 65535 ]; }; then
	echo "UDP_PORT must be 1-65535, not '$UDP_PORT'" >&2; exit 1
fi
# LOCATION 0-34 (the 35 planet codes of the disc of 2026-10-24), MODE 0-4, ERA 0-2, AREA 0-1, no leading zeros.
case $PLAYLIST in
''|*[!0-9.,]*) [ -z "$PLAYLIST" ] || { echo "PLAYLIST must be LOCATION.MODE.ERA.AREA entries separated by commas (22.4.0.0), not '$PLAYLIST'" >&2; exit 1; } ;;
esac
if [ -n "$PLAYLIST" ]; then
	printf '%s' "$PLAYLIST" | grep -qE '^(([0-9]|[12][0-9]|3[0-4])\.[0-4]\.[0-2]\.[01])(,(([0-9]|[12][0-9]|3[0-4])\.[0-4]\.[0-2]\.[01]))*$' || {
		echo "PLAYLIST entry out of range in '$PLAYLIST': LOCATION 0-34, MODE 0-4, ERA 0-2, AREA 0-1" >&2; exit 1; }
fi
PROFILE=$DEDICATED/ms0/PSP/SAVEDATA/ULUS993900000/DATA.BIN

# playlist_presses VBLANK ENTRY...: the presses that add each entry to the playlist the way the game's own driver does it
# (a DOWN per step, then CROSS, for location, mode, era and area), leaving the location cursor at the top after each entry.
# Sets PLAYLIST_PRESSES (VBLANK:BUTTON ...) and PLAYLIST_END, the VBlank after the last entry.
playlist_presses() {
	v=$1; shift
	presses=
	for entry in "$@"; do
		location=${entry%%.*}; rest=${entry#*.}
		mode=${rest%%.*}; rest=${rest#*.}
		era=${rest%%.*}; area=${rest#*.}
		i=0
		while [ "$i" -lt "$location" ]; do presses="$presses $v:DOWN"; v=$((v + 40)); i=$((i + 1)); done
		v=$((v + 300)); presses="$presses $v:CROSS"; v=$((v + 500))
		i=0
		while [ "$i" -lt "$mode" ]; do presses="$presses $v:DOWN"; v=$((v + 60)); i=$((i + 1)); done
		v=$((v + 200)); presses="$presses $v:CROSS"; v=$((v + 500))
		i=0
		while [ "$i" -lt "$era" ]; do presses="$presses $v:DOWN"; v=$((v + 40)); i=$((i + 1)); done
		v=$((v + 300)); presses="$presses $v:CROSS"; v=$((v + 500))
		i=0
		while [ "$i" -lt "$area" ]; do presses="$presses $v:DOWN"; v=$((v + 40)); i=$((i + 1)); done
		v=$((v + 300)); presses="$presses $v:CROSS"
		v=$((v + 800))
	done
	PLAYLIST_PRESSES=$presses
	PLAYLIST_END=$v
}

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
# default) and whether to save it (yes). The main run below starts from a saved one. A session that is given its playlist
# deletes the profile first: the game keeps the playlist in it, so a new one starts empty and the run below fills it.
if [ -n "$PLAYLIST" ] && [ -e "$PROFILE" ]; then
	rm "$PROFILE"
fi
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

# Start, skip the intro, load the profile, Multiplayer, Infrastructure, sign in, Create, name the game, then the playlist:
# square lets the game fill it at random, PLAYLIST adds its entries with presses from 13000 (2000 VBlanks after Continue,
# where the game's own driver waits 1400); then load the first map (start); then a cross every 500 VBlanks takes the
# team and the spawn point, in each round after the first too.
END=$((HOURS * 3600 * 60))
set -- 2500:START 3100:CROSS 3500:CROSS 4400:CROSS 5000:DOWN 5200:CROSS 5800:CROSS 6400:CROSS 6900:CROSS 7100:DOWN \
	7300:CROSS 7500:DOWN 7700:CROSS 7900:DOWN 8100:CROSS 9300:DOWN 9500:CROSS 10200:CROSS 10800:DOWN 11000:CROSS
if [ -n "$PLAYLIST" ]; then
	# shellcheck disable=SC2086  # PLAYLIST_PRESSES is a list of presses, split on purpose
	playlist_presses 13000 $(printf '%s' "$PLAYLIST" | tr ',' ' ')
	set -- "$@" $PLAYLIST_PRESSES
	start=$PLAYLIST_END
else
	set -- "$@" 11600:SQUARE
	start=14200
fi
set -- "$@" "$start:START"
for v in $(seq $((start + 2200)) 500 "$END"); do set -- "$@" "$v:CROSS"; done
run_pspc "$END" "$OSK" "$@"
