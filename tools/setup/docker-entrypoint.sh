#!/usr/bin/env bash
# The container's start (X-package). With no arguments it runs what the installer's two services
# run: pmd in standby, and the website on port 47080. With arguments it runs them instead, so
# `docker run IMAGE pm status` or `docker run IMAGE bash` work as expected.

set -euo pipefail
[ "$#" -gt 0 ] && exec "$@"

/opt/paramesh/bin/pmd --standby &
exec python3 /opt/paramesh/tools/playground/server.py --public
