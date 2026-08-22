#!/bin/bash
# fetch-x86-libs.sh — stage x86_64 host libraries for the x86_64 VM build.
#
#   scripts/fetch-x86-libs.sh [outdir] [formula...]
#
# WHY THIS EXISTS
#
# This Mac is Apple Silicon with an arm64-only Homebrew at /opt/homebrew.  The
# x86_64 VM build runs under Rosetta and dlopen()s host libraries through
# Pharo's FFI -- and there are no x86_64 builds of libcairo or libgit2 here.
# Measured cost on SUnit batch 1-50: arm64 774/774 clean, x86_64 746/774 with
# all 26 deltas being SymbolNotFoundError on cairo_* lookups.  Across a full
# sweep it was ~260 of x86's ~287 errors, and libgit2's absence blocks every
# x86_64 Metacello package load.
#
# The alternative is a second, Intel-prefix Homebrew at /usr/local.  This
# script avoids that: it pulls the x86_64 bottles straight from Homebrew's
# ghcr.io registry, rewrites their install names to @loader_path, and stages
# them flat in one directory.  Nothing is installed and /usr/local is not
# touched.
#
# WHY STAGING NEXT TO THE BINARY IS ENOUGH
#
# src/vm/FFI.cpp:getLibSearchPaths() puts the executable's OWN directory first
# in the FFI search order, ahead of Homebrew.  So dylibs beside
# build-x86/test_load_image are found by a bare-name lookup.  Same mechanism
# the TFUFFI fixture libs already rely on.
#
# WHY BOTTLES AND NOT A SOURCE BUILD
#
# cairographics.org was unreachable on 2026-08-22, which is also why
# scripts/build-third-party.sh cannot currently get past pixman.  Bottles come
# from ghcr.io and are unaffected.  They are also already-built universal-free
# x86_64 binaries, so no cross-compile toolchain is involved.
set -euo pipefail

OUT=${1:-build-x86}
shift || true
ROOTS=("$@")
[ ${#ROOTS[@]} -gt 0 ] || ROOTS=(cairo libgit2)

# x86_64 macOS bottle tags, newest first.  There is no x86_64 bottle for
# macOS 26+; these target older SDKs and load fine under Rosetta on a newer
# host, which is the whole point of picking a foreign-arch bottle.
TAGS=(sonoma ventura)

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

log() { printf '[x86libs] %s\n' "$*"; }

# Formula name -> ghcr repository path.  Homebrew maps '@' to '/' so that
# openssl@3 lives at homebrew/core/openssl/3.
repo_for() { printf 'homebrew/core/%s' "${1//@//}"; }

token_for() {
  curl -fsS --max-time 60 \
    "https://ghcr.io/token?service=ghcr.io&scope=repository:$(repo_for "$1"):pull" \
  | python3 -c 'import sys,json;print(json.load(sys.stdin)["token"])'
}

# macOS ships bash 3.2, which has no associative arrays.  A space-delimited
# string is the portable stand-in; the closure is a few dozen names at most.
SEEN=" "
seen() { case "$SEEN" in *" $1 "*) return 0;; *) return 1;; esac; }

QUEUE=("${ROOTS[@]}")

while [ ${#QUEUE[@]} -gt 0 ]; do
  f="${QUEUE[0]}"; QUEUE=("${QUEUE[@]:1}")
  seen "$f" && continue
  SEEN="$SEEN$f "

  info=$(brew info --json=v2 "$f" 2>/dev/null) || { log "SKIP $f (no such formula)"; continue; }
  read -r version deps < <(python3 -c '
import sys,json
d=json.load(sys.stdin)["formulae"][0]
v=d["versions"]["stable"]
r=d.get("revision") or 0
print(f"{v}_{r}" if r else v, " ".join(d.get("dependencies",[])))
' <<<"$info")

  # Queue runtime dependencies; the closure is what makes the dylib loadable.
  for d in $deps; do seen "$d" || QUEUE+=("$d"); done

  tok=$(token_for "$f") || { log "SKIP $f (no ghcr token)"; continue; }
  idx=$(curl -fsS --max-time 120 -H "Authorization: Bearer $tok" \
        -H "Accept: application/vnd.oci.image.index.v1+json" \
        "https://ghcr.io/v2/$(repo_for "$f")/manifests/${version}" 2>/dev/null) \
    || { log "SKIP $f (no manifest for $version)"; continue; }

  digest=""
  for tag in "${TAGS[@]}"; do
    digest=$(python3 -c '
import sys,json
idx=json.load(sys.stdin); want=sys.argv[1]
for m in idx.get("manifests",[]):
    a=m.get("annotations",{})
    # The bottle tag lives in org.opencontainers.image.ref.name.  sh.brew.tag
    # is documented in places but is absent from the live index -- reading
    # only that one silently matched nothing and skipped every formula.
    if want in (a.get("sh.brew.tag"), a.get("org.opencontainers.image.ref.name")):
        print(m["digest"]); break
' "${version}.${tag}" <<<"$idx") && [ -n "$digest" ] && break
  done
  [ -n "$digest" ] || { log "SKIP $f (no x86_64 bottle)"; continue; }

  blob=$(curl -fsS --max-time 120 -H "Authorization: Bearer $tok" \
         -H "Accept: application/vnd.oci.image.manifest.v1+json" \
         "https://ghcr.io/v2/$(repo_for "$f")/manifests/$digest" \
       | python3 -c 'import sys,json;print(json.load(sys.stdin)["layers"][0]["digest"])')

  log "fetch $f $version"
  curl -fsSL --max-time 600 -H "Authorization: Bearer $tok" \
    "https://ghcr.io/v2/$(repo_for "$f")/blobs/$blob" -o "$WORK/$f.tar.gz"
  tar -xzf "$WORK/$f.tar.gz" -C "$WORK"
  rm -f "$WORK/$f.tar.gz"
done

# Flatten every x86_64 dylib into OUT.  Bottles unpack as <formula>/<version>/lib.
log "staging dylibs into $OUT"
found=0
while IFS= read -r dylib; do
  # Skip anything that is not actually x86_64 -- a bottle can carry scripts
  # and pkgconfig files named *.dylib is unlikely, but arch check is cheap
  # and a wrong-arch file here would produce the exact failure we are fixing.
  lipo -archs "$dylib" 2>/dev/null | grep -q x86_64 || continue
  cp -f "$dylib" "$OUT/$(basename "$dylib")"
  found=$((found+1))
done < <(find "$WORK" -type f -name '*.dylib')
log "staged $found dylibs"
[ "$found" -gt 0 ] || { log "nothing staged -- aborting before rewrite"; exit 1; }

# Rewrite install names so the graph resolves inside OUT rather than at the
# /usr/local paths the bottles were built for (which do not exist here).
# Bottles ship unrelocated, so paths appear as @@HOMEBREW_PREFIX@@/... ;
# an installed copy would say /usr/local/... .  Handle both.
log "rewriting install names to @loader_path"
cd "$OUT"
for lib in *.dylib; do
  chmod u+w "$lib"
  install_name_tool -id "@loader_path/$lib" "$lib" 2>/dev/null || true
  while IFS= read -r dep; do
    case "$dep" in
      @@HOMEBREW*|/usr/local/*|/opt/homebrew/*)
        install_name_tool -change "$dep" "@loader_path/$(basename "$dep")" "$lib" 2>/dev/null || true
        ;;
    esac
  done < <(otool -L "$lib" | tail -n +2 | awk '{print $1}')
  codesign --force --sign - "$lib" 2>/dev/null || true
done

# Recreate the unversioned/short soname aliases.  Homebrew ships these as
# symlinks inside the cellar (libgit2.dylib -> libgit2.1.9.7.dylib), but the
# bottle's links point at paths that do not exist in a flat staging dir, so
# only the fully-versioned file survives the copy.  Pharo's library finders
# ask for the SHORT name -- 'libgit2.dylib' -- so without these the staged
# x86_64 libgit2 is present and still not found.
log "creating soname aliases"
for lib in *.dylib; do
  stem=${lib%.dylib}
  while [ "$stem" != "${stem%.*}" ]; do
    stem=${stem%.*}
    case "$stem" in ""|lib) break;; esac
    [ -e "$stem.dylib" ] || ln -s "$lib" "$stem.dylib"
  done
done

log "done. unresolved non-system references, if any:"
for lib in *.dylib; do
  otool -L "$lib" | tail -n +2 | awk '{print $1}' \
    | grep -Ev '^(@loader_path|/usr/lib|/System)' | sed "s|^|  $lib -> |" || true
done | sort -u
