#!/bin/bash
#
# vm-pr-build.sh -- build the Cog VM of one pull request branch, and make an
# app that scripts/vmrun.sh can start by its path.
#
#   scripts/upstream-pr/vm-pr-build.sh <pharo-vm work tree> <name>
#
# Makes (under $PHARO_WORK, default /tmp/pharo-work):
#   cogbuild-pr-<name>/                  the CMake build tree
#   apps/<name>/Pharo.app                a copy of the stock Pharo 15 VM app with
#                                        the VM core and the socket plugin of the
#                                        build (a local Cog build has no freetype,
#                                        cairo or libgit2, so the app keeps the
#                                        stock third-party libraries)
# The VM for vmrun.sh is then $PHARO_WORK/apps/<name>/Pharo.app/Contents/MacOS/Pharo
#
# The code generation of the build starts a Pharo VM, so the build goes
# through `vmrun.sh cogbuild <dir>` and its lock.  Iceberg must clone the
# VMMaker dependencies over HTTPS (ICEBERG_DEFAULT_REMOTE=httpsUrl): over SSH
# the vmmaker step stops with an authentication error.
set -u
wt=${1:?usage: vm-pr-build.sh <pharo-vm work tree> <name>}
name=${2:?name}
REPO=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
WORK=${PHARO_WORK:-/tmp/pharo-work}
STOCK_APP=/tmp/harness15/pharo-vm/Pharo.app
bdir=$WORK/cogbuild-pr-$name
app=$WORK/apps/$name/Pharo.app

[ -d "$wt/smalltalksrc" ] || { echo "vm-pr-build: $wt is not a pharo-vm tree" >&2; exit 2; }
[ -x "$STOCK_APP/Contents/MacOS/Pharo" ] || { echo "vm-pr-build: no stock app $STOCK_APP" >&2; exit 2; }

# A new tree each time: the VMMaker image of a build tree keeps the Slang code
# of its first build, so a tree reused for other sources generates old code.
rm -rf "$bdir"
cmake -S "$wt" -B "$bdir" -DGENERATE_PHARO_VM="$STOCK_APP/Contents/MacOS/Pharo" \
      -DICEBERG_DEFAULT_REMOTE=httpsUrl > "$bdir.configure.log" 2>&1 \
    || { echo "vm-pr-build: configure failed ($bdir.configure.log)" >&2; exit 1; }
# The vmmaker step clones the VMMaker dependencies with libgit2 inside the stock
# VM.  On 2026-11-23 six of eleven such loads stopped with a heap-corruption
# abort in the TLS handshake of libgit2 (nanov2_guard_corruption_detected in
# SSLRecordWriteInternal or SSLAllocBuffer, from a libgit2 clone or
# fetch).  The cause is not known.  A failed try is started again with the clones of a good build.
ok=0
for try in 1 2 3 4 5; do
    if "$REPO/scripts/vmrun.sh" cogbuild "$bdir"; then ok=1; break; fi
    grep -a -q 'vmmaker-build\] Error' "$bdir/build-last.log" || break
    echo "vm-pr-build: the vmmaker step failed (try $try of 5)"
    cp "$bdir/build-last.log" "$bdir/build-failed-$try.log"
    # The abort leaves a half-made clone, and the next try then stops with
    # ENOTFOUND on it.  Put complete clones in its place (the clones of the
    # dependencies of a build that worked, $WORK/vmmaker-iceberg-seed).
    # Iceberg still fetches each repository, and a fetch can abort in the
    # same way (measured), so a try can fail again.
    ice=$bdir/build/vmmaker/image/pharo-local/iceberg
    rm -rf "$ice"
    if [ -d "$WORK/vmmaker-iceberg-seed" ]; then
        mkdir -p "$ice" && cp -R "$WORK/vmmaker-iceberg-seed/." "$ice/"
    fi
done
[ $ok = 1 ] || exit 1

src=$bdir/build/vm/Debug/Pharo.app/Contents/MacOS/Plugins
rm -rf "$WORK/apps/$name"
mkdir -p "$WORK/apps/$name"
cp -R "$STOCK_APP" "$app" || exit 1
cp "$src/libPharoVMCore.dylib" "$src/libSocketPlugin.dylib" "$app/Contents/MacOS/Plugins/" || exit 1
echo "commit: $(git -C "$wt" rev-parse --short HEAD) $(git -C "$wt" rev-parse --abbrev-ref HEAD)"
echo "VM: $app/Contents/MacOS/Pharo"
