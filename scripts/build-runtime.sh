#!/usr/bin/env bash
# Builds the browser runtime for the Spider-Man (2000) PC port and assembles
# the deployable site in dist/.
#
# The runtime is Boxedwine (GPL-2.0) compiled to WebAssembly with its WASM JIT,
# plus the Boxedwine Wine 11 web filesystem, which contains the patched WineD3D
# that turns DirectDraw/Direct3D into WebGL. The original, unmodified
# SpideyPC.exe runs on top of it. No game code is rebuilt or reimplemented.
#
# No game files are used or copied by this script. The player supplies them
# from their own copy at runtime, in the browser.
#
# Requirements: Linux or WSL, git, python3, curl, sha256sum, GNU make.
# Emscripten is installed into $WORK/emsdk if not already on PATH.

set -euo pipefail

# Pinned inputs. Change these only deliberately and record why in README.md.
BOXEDWINE_REPO=https://github.com/danoon2/Boxedwine.git
BOXEDWINE_COMMIT=8ab37e7b79737467674b40fe0675396b3d90a469   # master, 2026-10-25 (26R2 development)
EMSDK_VERSION=6.0.13
FS_NAME=TinyCore15Wine11.0-web.zip
FS_URL=https://boxedwine.org/v2/23/$FS_NAME
FS_SHA256=4d505092956dd78e36712dac8111c252e5d1b859ce2373fbe7c80242bb65ad47   # webgl_filesystems_v23.json "web-v23"

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${WORK:-$ROOT/build}
OUT=${OUT:-$ROOT/dist}
# Space-separated Boxedwine make targets. jit = single-threaded WASM JIT (no
# special headers needed). multiThreadedJit needs cross-origin isolation,
# which the site provides through coi-serviceworker.
TARGETS=${TARGETS:-"jit multiThreadedJit"}
JOBS=${JOBS:-$(nproc)}

declare -A TARGET_DIR=( [jit]=Jit [multiThreadedJit]=MultiThreadedJit )
declare -A SITE_DIR=( [jit]=st [multiThreadedJit]=mt )

mkdir -p "$WORK" "$OUT"

# --- Emscripten -------------------------------------------------------------
if ! command -v emcc >/dev/null 2>&1; then
    if [ ! -d "$WORK/emsdk" ]; then
        git clone --depth 1 https://github.com/emscripten-core/emsdk.git "$WORK/emsdk"
    fi
    "$WORK/emsdk/emsdk" install "$EMSDK_VERSION"
    "$WORK/emsdk/emsdk" activate "$EMSDK_VERSION"
    # shellcheck disable=SC1091
    source "$WORK/emsdk/emsdk_env.sh"
fi
echo "emcc: $(emcc --version | head -1)"

# --- Boxedwine source at the pinned commit -----------------------------------
SRC="$WORK/Boxedwine"
if [ ! -d "$SRC/.git" ]; then
    git init -q "$SRC"
    git -C "$SRC" remote add origin "$BOXEDWINE_REPO"
fi
if [ "$(git -C "$SRC" rev-parse HEAD 2>/dev/null || true)" != "$BOXEDWINE_COMMIT" ]; then
    git -C "$SRC" fetch -q --depth 1 origin "$BOXEDWINE_COMMIT"
    git -C "$SRC" checkout -q --force "$BOXEDWINE_COMMIT"
fi

# --- Build each runtime ------------------------------------------------------
for target in $TARGETS; do
    dir=${TARGET_DIR[$target]:?unknown target $target}
    echo "== building Boxedwine target '$target' with $JOBS jobs"
    make -C "$SRC/project/emscripten" "$target" -j"$JOBS"
    dest="$OUT/runtime/${SITE_DIR[$target]}"
    rm -rf "$dest"
    mkdir -p "$dest"
    cp -r "$SRC/project/emscripten/Build/$dir/." "$dest/"
done

# --- Wine filesystem (shared by both runtimes) --------------------------------
FS="$WORK/$FS_NAME"
if [ ! -f "$FS" ] || ! echo "$FS_SHA256  $FS" | sha256sum -c --status; then
    curl -fSL --retry 3 -o "$FS.part" "$FS_URL"
    mv "$FS.part" "$FS"
fi
echo "$FS_SHA256  $FS" | sha256sum -c
for target in $TARGETS; do
    cp "$FS" "$OUT/runtime/${SITE_DIR[$target]}/$FS_NAME"
done

# --- Cross-origin isolation for the multi-threaded runtime -------------------
# GitHub Pages cannot send COOP/COEP headers. coi-serviceworker (MIT) adds them
# from a service worker so SharedArrayBuffer is available to the pthread build.
if [ -d "$OUT/runtime/mt" ]; then
    COI_VERSION=0.1.7
    curl -fsSL --retry 3 -o "$OUT/runtime/mt/coi-serviceworker.js" \
        "https://cdn.jsdelivr.net/npm/coi-serviceworker@$COI_VERSION/coi-serviceworker.js"
    if ! grep -q coi-serviceworker.js "$OUT/runtime/mt/boxedwine.html"; then
        sed -i 's#<head>#<head><script src="coi-serviceworker.js"></script>#' "$OUT/runtime/mt/boxedwine.html"
    fi
fi

# --- Site ------------------------------------------------------------------
cp -r "$ROOT/site/." "$OUT/"
cp "$ROOT/LICENSES.md" "$OUT/LICENSES.md"
cat > "$OUT/build-info.json" <<EOF
{
  "boxedwineCommit": "$BOXEDWINE_COMMIT",
  "emscripten": "$(emcc --version | head -1 | sed 's/"/\\"/g')",
  "filesystem": "$FS_NAME",
  "filesystemSha256": "$FS_SHA256",
  "targets": "$TARGETS",
  "builtAt": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
EOF

# Safety net: refuse to emit a site that contains game data.
if find "$OUT" -iname '*.pkr' -o -iname 'SpideyPC.exe' -o -iname 'texture.dat' -o -iname 'binkw32.dll' | grep -q .; then
    echo "error: game files found in $OUT; they must never be deployed" >&2
    exit 1
fi

echo "site assembled in $OUT"
