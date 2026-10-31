#!/bin/sh
# Copy the MPM v2 code into a checkout of the MULTI-Module repo
# (DIY-Multiprotocol-TX-Module, its platforms branch), where the protocol
# files are Multiprotocol/*.ino themselves:
#   src/    all but src/mpm/upstream (the C copies of the protocol files);
#           the MULTI-Module's src/mpm/sketch is kept
#   lib/    TI, Segger RTT, pyserial, littlefs's sources and licence; the
#           MULTI-Module's lib/stm32f1 is kept
#   tools/  all but the radio bench (tools/bench) and what is mpm-v2's
#   tests/  the host tests
# from mpm-v2's HEAD (commit first), files gone here removed there. Then
# `git status` there shows what changed; the version (project() in
# CMakeLists.txt here, in the MULTI-Module's) is checked, not copied.
#
#   tools/multi_sync.sh ~/Src/DIY-Multiprotocol-TX-Module

set -e

here=$(cd "$(dirname "$0")/.." && pwd)
dest=${1:?usage: multi_sync.sh <MULTI-Module checkout>}
dest=$(cd "$dest" && pwd)
if [ ! -f "$dest/Multiprotocol/Multiprotocol.ino" ]; then
    echo "$dest: not a MULTI-Module checkout" >&2
    exit 1
fi
if [ -n "$(git -C "$here" status --porcelain -- src lib tools tests)" ]; then
    echo "mpm-v2: uncommitted changes in src, lib, tools or tests (HEAD is copied)" >&2
fi

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git -C "$here" archive HEAD src lib tools tests | tar -x -C "$tmp"
# littlefs is a submodule here: its sources only
mkdir -p "$tmp/lib/littlefs"
for f in lfs.c lfs.h lfs_util.c lfs_util.h LICENSE.md; do
    cp "$here/lib/littlefs/$f" "$tmp/lib/littlefs/"
done

# copy <dir> [rsync filters]: <dir>/ from HEAD to the MULTI-Module, what is
# excluded neither copied nor removed there
copy() {
    d=$1
    shift
    mkdir -p "$dest/$d"
    rsync -a --delete --exclude __pycache__ --exclude build/ "$@" "$tmp/$d/" "$dest/$d/"
}
copy src --exclude /mpm/upstream/ --exclude /mpm/sketch/
copy lib --exclude /stm32f1/
copy tools --exclude /bench/ --exclude /pyproject.toml --exclude /uv.lock \
    --exclude '/test_*.py' --exclude /multi_sync.sh
copy tests

version() {
    sed -n 's/^ *project(MPMv2 VERSION \([0-9.]*\).*/\1/p' "$1" | head -1
}
v=$(version "$here/CMakeLists.txt")
mv=$(version "$dest/CMakeLists.txt")
if [ "$v" != "$mv" ]; then
    echo "version: mpm-v2 $v, MULTI-Module $mv (its CMakeLists.txt)" >&2
fi

echo "mpm-v2 $(git -C "$here" log -1 --format='%h %s') copied to $dest:"
git -C "$dest" status --short -- src lib tools tests
