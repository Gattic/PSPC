#!/bin/sh
# Installs the public SWSS (GameSpy) server for the current user, without root, from this checkout:
#
#   server/deploy/swss/install.sh            # into ~/swss-server (PREFIX=/other/place to choose)
#   MODE=tunnel server/deploy/swss/install.sh # behind a Cloudflare Tunnel instead of forwarded ports
#   HOST=swss.example.org ...                 # the domain players use (default swss.pspc.online)
#   MAX_PLAYER_HOSTS=5 ...                    # games players may host at once, direct mode (default 5)
#
# - $PREFIX/app/pspc_gamespy   the server's code (copied: the service doesn't run from the checkout)
# - $PREFIX/data               its database, status file and logs: private to this user
# - $PREFIX/bin/swss-admin     the administration commands (accounts, ban, passwd, ...) on its database
# - $PREFIX/cloudflared        the tunnel's token (token.env), when there is one
# - ~/.config/systemd/user     swss-server.service and swss-cloudflared.service
#
# MODE=direct (the default) is for forwarded ports: the services listen on every address and HOST must be a
# DNS-only record (not Cloudflare-proxied) that leads here. MODE=tunnel is for no open ports at all.
# Run it again to update the code. It starts the server; in tunnel mode the tunnel starts once token.env
# exists (see README.md in this folder).
set -eu

PREFIX=${PREFIX:-$HOME/swss-server}
MODE=${MODE:-direct}
HOST=${HOST:-swss.pspc.online}
MAX_PLAYER_HOSTS=${MAX_PLAYER_HOSTS:-5}
case "$MODE" in
	direct) SWSS_MODE="--bind 0.0.0.0 --public-host $HOST --max-player-hosts $MAX_PLAYER_HOSTS" ;;
	tunnel) SWSS_MODE="--bind 127.0.0.1 --tunnel --behind-proxy" ;;
	*) echo "MODE is direct or tunnel, not $MODE." >&2; exit 1 ;;
esac
HERE=$(cd "$(dirname "$0")" && pwd)
SERVER=$(cd "$HERE/../.." && pwd)
PYTHON=$(command -v python3) || { echo "Python 3 is needed (3.9 or later)." >&2; exit 1; }
"$PYTHON" -c 'import sys; sys.exit(sys.version_info < (3, 9))' || { echo "Python 3.9 or later is needed." >&2; exit 1; }
UNITS=${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user

# Direct mode tells players $HOST's address: a record that is proxied by Cloudflare (orange cloud), private or someone
# else's would send them to the wrong place, and the server would still start. SKIP_DNS_CHECK=1 to go on anyway.
if [ "$MODE" = direct ] && [ "${SKIP_DNS_CHECK:-0}" != 1 ]; then
	"$PYTHON" "$SERVER/tests/live_check.py" --host "$HOST" --dns-only --here || {
		echo "Fix the DNS record first (docs: server/deploy/swss/README.md, step 2), or SKIP_DNS_CHECK=1 to install anyway." >&2
		exit 1
	}
fi

umask 077
mkdir -p "$PREFIX/app/pspc_gamespy" "$PREFIX/data" "$PREFIX/bin" "$PREFIX/cloudflared" "$UNITS"
chmod 700 "$PREFIX" "$PREFIX/data" "$PREFIX/cloudflared"

# The code: replace the .py files, so a module that has gone doesn't linger.
find "$PREFIX/app/pspc_gamespy" -maxdepth 1 -name '*.py' -delete
cp "$SERVER"/pspc_gamespy/*.py "$PREFIX/app/pspc_gamespy/"
chmod 755 "$PREFIX/app" "$PREFIX/app/pspc_gamespy"
chmod 644 "$PREFIX/app/pspc_gamespy"/*.py

echo "SWSS_MODE=$SWSS_MODE" > "$PREFIX/data/mode.env"
for unit in swss-server.service swss-cloudflared.service; do
	sed -e "s|@PREFIX@|$PREFIX|g" -e "s|@PYTHON@|$PYTHON|g" "$HERE/$unit" > "$UNITS/$unit"
done
cat > "$PREFIX/bin/swss-admin" <<ADMIN
#!/bin/sh
# The server's administration commands on its database: swss-admin accounts | ban NAME | unban NAME | passwd NAME | games | stats
exec env PYTHONPATH="$PREFIX/app" "$PYTHON" -B -m pspc_gamespy --db "$PREFIX/data/gamespy.sqlite" "\$@"
ADMIN
chmod 755 "$PREFIX/bin/swss-admin"
cp "$HERE/dedicated-host.sh" "$PREFIX/bin/dedicated-host.sh"
chmod 755 "$PREFIX/bin/dedicated-host.sh"
[ -e "$PREFIX/data/gamespy-sessions.json" ] || cp "$HERE/gamespy-sessions.example.json" "$PREFIX/data/gamespy-sessions.example.json"

systemctl --user daemon-reload
systemctl --user enable swss-server.service
systemctl --user restart swss-server.service
echo "The server is running (systemctl --user status swss-server; log: journalctl --user -u swss-server)."

if [ "$MODE" = direct ]; then
	systemctl --user disable --now swss-cloudflared.service 2>/dev/null || true
	echo "Forward these ports on the router to this machine: TCP 29900, 29901, 28910, 29920 and 80 (to 8080 here);"
	echo "UDP 27900 and 27901; and the UDP port(s) of the dedicated game servers. $HOST must point at the router's public"
	echo "address with a DNS-only record. Without it the server stops at start with 'can't look up the public host'."
elif [ -x "$PREFIX/bin/cloudflared" ] && [ -s "$PREFIX/cloudflared/token.env" ]; then
	systemctl --user enable swss-cloudflared.service
	systemctl --user restart swss-cloudflared.service
	echo "The tunnel is running (systemctl --user status swss-cloudflared)."
else
	echo "No tunnel yet: put cloudflared in $PREFIX/bin and the tunnel's token in $PREFIX/cloudflared/token.env, then run this again."
fi
if [ "$(loginctl show-user "$USER" -p Linger --value 2>/dev/null)" != yes ]; then
	echo "To keep running after you log out: loginctl enable-linger $USER (may need root)."
fi
