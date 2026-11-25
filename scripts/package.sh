#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
set -eu
version=$(cat VERSION)
out=$(realpath -m "${1:-dist}")
mkdir -p "$out"
stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
install -Dm755 build/BFS.so "$stage/System/Filesystems/BFS.so"
install -Dm644 preview/Filesystem-BFS.pdf "$stage/System/Resources/Documentation/Filesystem-BFS.pdf"
mkdir -p "$stage/DEBIAN"
cat > "$stage/DEBIAN/control" <<EOF
Package: infiltrator-filesystem-bfs
Version: $version
Architecture: amd64
Maintainer: Infiltrator Projects
Description: Native SCO UnixWare Boot File System Common V2 module
EOF
dpkg-deb --build --root-owner-group "$stage" "$out/infiltrator-filesystem-bfs_${version}_amd64.deb"
