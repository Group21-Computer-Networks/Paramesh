#!/usr/bin/env bash
# The ParaMesh installer (X-setup). tools/setup/package.sh appends a tar.gz of a Release build
# below the __PAYLOAD__ line and calls the result paramesh-setup-<version>.sh: one file to share.
#
#   bash paramesh-setup-<version>.sh               install, or upgrade, and start pmd
#   bash paramesh-setup-<version>.sh --no-service  install only; start pmd yourself
#   bash paramesh-setup-<version>.sh --uninstall   stop pmd and remove everything installed
#
# Everything goes to /opt/paramesh, the same path on every node: a job's workers run the
# launcher's program from the same absolute path, and check that it is the same file. pmd runs
# as a systemd user service in standby; join the pool from its dashboard,
# http://127.0.0.1:47002/. Needs sudo for /opt/paramesh and /usr/local.

set -euo pipefail

PREFIX=/opt/paramesh
UNIT="$HOME/.config/systemd/user/paramesh.service"
SITE_UNIT="$HOME/.config/systemd/user/paramesh-site.service"
SERVICE=1
say() { printf 'paramesh: %s\n' "$*"; }
die() { say "$*" >&2; exit 1; }

case "${1:-}" in
    "") ;;
    --no-service) SERVICE=0 ;;
    --uninstall)
        systemctl --user disable --now paramesh-site.service paramesh.service 2>/dev/null || true
        rm -f "$UNIT" "$SITE_UNIT"
        systemctl --user daemon-reload 2>/dev/null || true
        sudo rm -rf "$PREFIX" "$PREFIX.old"
        sudo rm -f /usr/local/bin/pmd /usr/local/bin/pm /usr/local/bin/pmrun \
            /usr/local/lib/pkgconfig/paramesh.pc
        say "removed. Your ledger and node ID stay in ~/.local/state/paramesh."
        exit 0
        ;;
    -h | --help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) die "unknown option $1 (try --help)" ;;
esac

# ---- can this machine be a node?
[ "$(uname -s)" = Linux ] || die "ParaMesh runs on Linux only (it needs userfaultfd)."
built_for="@ARCH@"
[ "$(uname -m)" = "$built_for" ] || die "this package is for $built_for, this machine is $(uname -m)."
read -r major minor _ <<<"$(uname -r | tr '.-' '  ')"
if [ "$major" -lt 5 ] || { [ "$major" -eq 5 ] && [ "$minor" -lt 19 ]; }; then
    die "Linux 5.19 or later is needed; this is $(uname -r)."
fi

# ---- unpack beside the old install, then swap, so a failure leaves the old one in place
line=$(awk '/^__PAYLOAD__$/ {print NR + 1; exit}' "$0")
[ -n "$line" ] || die "this file has no payload: make it with tools/setup/package.sh."
say "installing to $PREFIX (sudo may ask for your password)"
sudo rm -rf "$PREFIX.new"
sudo mkdir -p "$PREFIX.new"
tail -n +"$line" "$0" | sudo tar -xz -C "$PREFIX.new"
sudo chown -R "$(id -u):$(id -g)" "$PREFIX.new/apps"  # your programs go here, on every node
if [ -d "$PREFIX" ]; then
    sudo rm -rf "$PREFIX.old"
    sudo mv "$PREFIX" "$PREFIX.old"
fi
sudo mv "$PREFIX.new" "$PREFIX"
sudo mkdir -p /usr/local/bin /usr/local/lib/pkgconfig
for tool in pmd pm pmrun; do sudo ln -sf "$PREFIX/bin/$tool" /usr/local/bin/$tool; done
sudo ln -sf "$PREFIX/lib/pkgconfig/paramesh.pc" /usr/local/lib/pkgconfig/paramesh.pc

# ---- pmd, in standby, at every login
if [ "$SERVICE" -eq 1 ] && command -v systemctl >/dev/null && systemctl --user show-environment >/dev/null 2>&1; then
    mkdir -p "$(dirname "$UNIT")"
    cat >"$UNIT" <<EOF
[Unit]
Description=ParaMesh daemon (pmd): join the pool from http://127.0.0.1:47002/
After=network-online.target

[Service]
ExecStart=$PREFIX/bin/pmd --standby
Restart=on-failure

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable paramesh.service >/dev/null 2>&1
    systemctl --user restart paramesh.service
    for _ in $(seq 1 50); do  # health check: the status API answers
        curl -fs http://127.0.0.1:47002/v1/peers >/dev/null 2>&1 && break
        sleep 0.2
    done
    curl -fs http://127.0.0.1:47002/v1/peers >/dev/null 2>&1 ||
        die "pmd did not start; see: journalctl --user -u paramesh.service"
    say "pmd is running in standby (systemctl --user status paramesh)."
    cat >"$SITE_UNIT" <<EOF
[Unit]
Description=ParaMesh website and playground: http://127.0.0.1:47080/
After=paramesh.service

[Service]
ExecStart=/usr/bin/python3 $PREFIX/tools/playground/server.py --public
Restart=on-failure

[Install]
WantedBy=default.target
EOF
    systemctl --user daemon-reload
    systemctl --user enable paramesh-site.service >/dev/null 2>&1
    systemctl --user restart paramesh-site.service
    say "the website is running: http://127.0.0.1:47080/ (systemctl --user status paramesh-site)."
    command -v g++ >/dev/null || say "to compile for the playground on this node: sudo apt install g++"
else
    say "no systemd user session: start pmd yourself with: pmd --standby"
fi

cat <<EOF

Done. Next:
  1. Open http://127.0.0.1:47080/dashboard. To give your cores, enter an invite code from a
     node in the pool. The first node just joins, and shows the code for the others.
     The website is open to the LAN: see /opt/paramesh/share/doc/paramesh/PLAYGROUND.md.
  2. Build a program once:   cc -O2 app.c \$(pkg-config --cflags --libs paramesh) -o app
     Copy the same file to $PREFIX/apps/ on every node, then from that directory:
                             pmrun -n 3 $PREFIX/apps/app
     Or try a bundled one:   cd $PREFIX/apps && pmrun -n 2 $PREFIX/apps/matmul 512
  3. A firewall must let the pool's nodes reach each other: UDP 47000, TCP 47001, and the
     TCP ports jobs open, e.g. with ufw:  sudo ufw allow from 192.168.1.0/24
API reference: $PREFIX/share/doc/paramesh/API.md. Undo: bash $0 --uninstall
EOF
exit 0
__PAYLOAD__
