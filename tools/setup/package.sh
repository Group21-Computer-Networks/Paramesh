#!/usr/bin/env bash
# Makes the one file to share (X-setup): a Release build, packed below tools/setup/install.sh.
#
#   tools/setup/package.sh [output directory]      (default build/dist)
#
# Writes paramesh-setup-<version>.sh. Anyone runs it with bash to install ParaMesh at
# /opt/paramesh. Build it once and give the same file to every node: the bundled programs in
# /opt/paramesh/apps then have the same hash everywhere, which a job's workers check. Built
# on this machine's distribution, it suits machines with the same or a newer one.

set -euo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
out="$(mkdir -p "${1:-$root/build/dist}" && cd "${1:-$root/build/dist}" && pwd)"
build="$root/build/package"
stage="$(mktemp -d)"
trap 'rm -rf "$stage"' EXIT

cmake -S "$root" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$build"

mkdir -p "$stage"/{bin,lib/pkgconfig,include,apps,tools/dashboard,share/doc/paramesh}
cp "$build/src/pmd/pmd" "$build/src/tools/pm" "$build/src/tools/pmrun" "$stage/bin/"
cp "$root/include/paramesh.h" "$stage/include/"
cp "$root/tools/dashboard/index.html" "$stage/tools/dashboard/"  # where pmd looks for it
mkdir -p "$stage/tools/playground" "$stage/tools/site"  # the website and the C playground
cp "$root/tools/playground/server.py" "$root/tools/playground/translate.py" "$stage/tools/playground/"
cp "$root"/tools/site/* "$stage/tools/site/"
for app in "$build"/apps/*/; do
    name="$(basename "$app")"
    [ -x "$app/$name" ] && cp "$app/$name" "$stage/apps/"
done
cp "$root"/docs/API.md "$root"/docs/PLAYGROUND.md "$stage/share/doc/paramesh/" 2>/dev/null || true

# The library's eight archives, in link order, as one: -lparamesh.
mri="CREATE $stage/lib/libparamesh.a"
for module in lib coh mem store rt net wire platform; do
    mri="$mri"$'\n'"ADDLIB $build/src/$module/libparamesh_$module.a"
done
printf '%s\nSAVE\nEND\n' "$mri" | ar -M
ranlib "$stage/lib/libparamesh.a"

version="$(git -C "$root" describe --always --dirty 2>/dev/null || echo unknown)"
cat >"$stage/lib/pkgconfig/paramesh.pc" <<EOF
prefix=/opt/paramesh
Name: paramesh
Description: Distributed shared memory for a pool of Linux machines
Version: $version
Cflags: -I\${prefix}/include
Libs: -L\${prefix}/lib -lparamesh -lstdc++ -lm -lpthread
EOF

file="$out/paramesh-setup-$version.sh"
sed "s/@ARCH@/$(uname -m)/" "$root/tools/setup/install.sh" >"$file"
chmod 755 "$stage"  # mktemp made it private, and tar would carry that to /opt/paramesh
tar -czf - --owner=0 --group=0 -C "$stage" . >>"$file"
chmod +x "$file"
echo "$file ($(du -h "$file" | cut -f1))"
