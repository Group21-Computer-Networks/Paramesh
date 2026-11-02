#!/usr/bin/env bash
# The one-line install (X-release). Published beside each release as install.sh:
#
#   curl -fsSL https://github.com/Group21-Computer-Networks/Paramesh/releases/latest/download/install.sh | bash
#
# Fetches the release's installer, checks its SHA-256, keeps it for --uninstall, and runs it. The
# same command updates. Arguments after `bash -s --` reach the installer. PARAMESH_RELEASE_URL
# points it at another place for a test.

set -euo pipefail

url="${PARAMESH_RELEASE_URL:-https://github.com/Group21-Computer-Networks/Paramesh/releases/latest/download}"
keep="${XDG_DATA_HOME:-$HOME/.local/share}/paramesh"
say() { printf 'paramesh: %s\n' "$*"; }

command -v curl >/dev/null || { say "this needs curl: sudo apt install curl" >&2; exit 1; }
mkdir -p "$keep"
work="$(mktemp -d "$keep/download.XXXXXX")"
trap 'rm -rf "$work"' EXIT

say "downloading the installer"
curl -fsSL "$url/paramesh-setup.sh" -o "$work/paramesh-setup.sh"
curl -fsSL "$url/paramesh-setup.sh.sha256" -o "$work/paramesh-setup.sh.sha256"
(cd "$work" && sha256sum --quiet -c paramesh-setup.sh.sha256) || {
    say "the download does not match its checksum: nothing was installed" >&2
    exit 1
}
mv "$work/paramesh-setup.sh" "$keep/paramesh-setup.sh"
bash "$keep/paramesh-setup.sh" "$@" </dev/null  # sudo asks on the terminal itself
