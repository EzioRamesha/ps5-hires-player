#!/usr/bin/env bash
#
# macOS/Linux port of build.ps1's build spec for BFplayer.
#
# Produces the same artifacts as build.ps1:
#   dist/bfplayer.elf                 - the stripped player ELF
#   dist/bfplayer-standalone.elf      - the standalone launcher (embeds the
#                                        gzipped player) -- this is the file
#                                        you send to the PS5
#   dist/BFplayer-standalone.zip      - the standalone package
#   dist/BFplayer-websrv.zip          - the websrv homebrew-folder package
#   dist/build-manifest.json / BFplayer/build-manifest.json
#
# Requirements (see .superpowers/sdd/build-setup-report.md for how these were
# obtained on this host):
#   - PS5 Payload SDK at $PS5_PAYLOAD_SDK (default: /opt/ps5-payload-sdk)
#   - A host LLVM with llvm-config on PATH, or $LLVM_CONFIG pointing at one
#     (needed by the SDK's bin/clang wrappers; on macOS: brew install llvm)
#   - PacBrew v0.37 target libraries at $PS5_PACBREW_HOME
#     (default: <parent of PS5_PAYLOAD_SDK>/pacbrew-v0.37/homebrew), containing
#     at minimum SDL2, SDL2_image, SDL2_ttf, FFmpeg 7.0.1, libass, tinyxml2
#     and their dependency chains, built for the x86_64-sie-ps5 target.
#
# Usage:
#   ./build.sh [Debug|Release]
#
# Environment overrides:
#   PS5_PAYLOAD_SDK      - path to the PS5 Payload SDK
#   PS5_PACBREW_HOME      - path to the pacbrew v0.37 "homebrew" prefix
#   SDL_VIDEO_OVERRIDE_SRC - path to an alternate SDL_ps5video.c.o (skips the
#                            hash check against vendor/SDL_ps5_backend)

set -euo pipefail

CONFIGURATION="${1:-Release}"
case "$CONFIGURATION" in
    Debug|Release) ;;
    *) echo "error: configuration must be 'Debug' or 'Release', got '$CONFIGURATION'" >&2; exit 1 ;;
esac

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

: "${PS5_PAYLOAD_SDK:=/opt/ps5-payload-sdk}"
if [[ ! -d "$PS5_PAYLOAD_SDK" ]]; then
    echo "error: PS5_PAYLOAD_SDK not found at $PS5_PAYLOAD_SDK" >&2
    exit 1
fi
PS5_PAYLOAD_SDK="$(cd "$PS5_PAYLOAD_SDK" && pwd)"
export PS5_PAYLOAD_SDK

if [[ -z "${PS5_PACBREW_HOME:-}" ]]; then
    PS5_PACBREW_HOME="$(dirname "$PS5_PAYLOAD_SDK")/pacbrew-v0.37/homebrew"
fi
if [[ ! -d "$PS5_PACBREW_HOME" ]]; then
    echo "error: PacBrew home not found at $PS5_PACBREW_HOME" >&2
    exit 1
fi
PACBREW_HOME="$(cd "$PS5_PACBREW_HOME" && pwd)"

# A host LLVM (with llvm-config) is required by the SDK's bin/clang wrappers.
if [[ -z "${LLVM_CONFIG:-}" ]]; then
    for candidate in "$(command -v brew >/dev/null 2>&1 && brew --prefix llvm 2>/dev/null)/bin/llvm-config"; do
        if [[ -x "$candidate" ]]; then
            export LLVM_CONFIG="$candidate"
            break
        fi
    done
fi
if [[ -n "${LLVM_CONFIG:-}" ]]; then
    export LLVM_CONFIG
fi

CC_PS5="$PS5_PAYLOAD_SDK/bin/prospero-clang"
CXX_PS5="$PS5_PAYLOAD_SDK/bin/prospero-clang++"
if ! "$CXX_PS5" --version >/dev/null 2>&1; then
    echo "error: $CXX_PS5 does not run. Install/point to a host LLVM (LLVM_CONFIG) and retry." >&2
    exit 1
fi

# --- SDL video override object -------------------------------------------
SDL_VIDEO_OVERRIDE_SRC="${SDL_VIDEO_OVERRIDE_SRC:-}"
if [[ -n "$SDL_VIDEO_OVERRIDE_SRC" ]]; then
    SDL_VIDEO_OVERRIDE_SRC="$(cd "$(dirname "$SDL_VIDEO_OVERRIDE_SRC")" && pwd)/$(basename "$SDL_VIDEO_OVERRIDE_SRC")"
else
    SDL_VIDEO_OVERRIDE_SRC="$PROJECT_ROOT/vendor/SDL_ps5_backend/lib/SDL_ps5video.c.o"
    hash_file="$SDL_VIDEO_OVERRIDE_SRC.sha256"
    if [[ ! -f "$SDL_VIDEO_OVERRIDE_SRC" || ! -f "$hash_file" ]]; then
        echo "error: Missing PS5 SDL video override. Run tools/rebuild-sdl-ps5-video.ps1." >&2
        exit 1
    fi
    expected_hash="$(awk '{print tolower($1)}' "$hash_file" | head -1)"
    actual_hash="$(shasum -a 256 "$SDL_VIDEO_OVERRIDE_SRC" | awk '{print tolower($1)}')"
    if [[ "$actual_hash" != "$expected_hash" ]]; then
        echo "error: PS5 SDL video override hash mismatch. Rebuild it before linking." >&2
        exit 1
    fi
fi

# --- PacBrew preflight -----------------------------------------------------
required_files=(
    "include/SDL2/SDL.h"
    "include/SDL2/SDL_image.h"
    "lib/libSDL2.a"
    "lib/libSDL2_image.a"
    "lib/libavformat.a"
    "lib/libass.a"
    "lib/libtinyxml2.a"
)
for item in "${required_files[@]}"; do
    if [[ ! -e "$PACBREW_HOME/$item" ]]; then
        echo "error: PacBrew v0.37 is incomplete: missing $item in $PACBREW_HOME" >&2
        exit 1
    fi
done
# NOTE: build.ps1 also requires include/kitchensink2/kitchensink.h in the
# pacbrew prefix, but BFplayer compiles its own vendored, modified
# kitchensink straight from vendor/SDL_kitchensink (see $cSources below) and
# that vendored include dir is put ahead of pacbrew's on the include path.
# We check the vendored copy here instead of requiring pacbrew to ship one.
if [[ ! -f "$PROJECT_ROOT/vendor/SDL_kitchensink/include/kitchensink2/kitchensink.h" ]]; then
    echo "error: vendored kitchensink2 headers missing: vendor/SDL_kitchensink/include/kitchensink2/kitchensink.h" >&2
    exit 1
fi

BUILD_DIR="$PROJECT_ROOT/build/ps5"
DIST_DIR="$PROJECT_ROOT/dist"
PACKAGE_ROOT="$PROJECT_ROOT/build/package"
PACKAGE_DIR="$PACKAGE_ROOT/BFplayer-standalone"
WEBSRV_PACKAGE_DIR="$PACKAGE_ROOT/BFplayer"
WEBSRV_VERIFY_DIR="$PACKAGE_ROOT/BFplayer-websrv-verify"
mkdir -p "$BUILD_DIR" "$DIST_DIR"

rm -rf "$PACKAGE_DIR" "$WEBSRV_PACKAGE_DIR" "$WEBSRV_VERIFY_DIR"

VERSION="$(tr -d '[:space:]' < "$PROJECT_ROOT/VERSION")"
VERSION_HEADER="$BUILD_DIR/bfplayer_version.h"
VERSION_HEADER_RELATIVE="build/ps5/bfplayer_version.h"
{
    printf '#pragma once\n'
    printf '#define BFPLAYER_VERSION "%s"\n' "$VERSION"
} > "$VERSION_HEADER"

echo "== BFplayer build ($CONFIGURATION) =="
echo "   PS5_PAYLOAD_SDK = $PS5_PAYLOAD_SDK"
echo "   PACBREW_HOME    = $PACBREW_HOME"
echo "   VERSION         = $VERSION"

COMMON_CXX=(
    -std=c++20 -Wall -Wextra -Wpedantic
    -DBFPLAYER_PS5=1
    -Ivendor/SDL_kitchensink/include
    "-I$PACBREW_HOME/include"
    "-I$PACBREW_HOME/include/SDL2"
    -Iinclude
    -include "$VERSION_HEADER_RELATIVE"
)
if [[ "$CONFIGURATION" == "Release" ]]; then
    COMMON_CXX+=(-O2 -DNDEBUG)
else
    COMMON_CXX+=(-O0 -g3)
fi

SOURCES=(
    src/main.cpp
    src/core/artwork.cpp
    src/core/bulk_import.cpp
    src/core/diagnostics.cpp
    src/core/dlna_protocol.cpp
    src/core/library_scanner.cpp
    src/core/library_view.cpp
    src/core/list_navigation.cpp
    src/core/media_sources.cpp
    src/core/player_settings.cpp
    src/core/playlist.cpp
    src/core/remote_control.cpp
    src/core/safe_read_file.cpp
    src/core/source_uri.cpp
    src/network/dlna_client.cpp
    src/core/library_database.cpp
    src/core/media_probe.cpp
    src/ui/library_ui.cpp
    src/ui/playback_osd.cpp
)

OBJECTS=()
for source in "${SOURCES[@]}"; do
    stem="$(basename "$source")"
    stem="${stem%.*}"
    object="build/ps5/$stem.o"
    echo "CXX  $source"
    "$CXX_PS5" "${COMMON_CXX[@]}" -c "$source" -o "$object"
    OBJECTS+=("$object")
done

CCOMMON=(
    -std=c17 -Wall -Wextra -Wpedantic
    -DBFPLAYER_PS5=1
    -DKIT_VERSION_MAJOR=2 -DKIT_VERSION_MINOR=0 -DKIT_VERSION_PATCH=0
    -Ivendor/SDL_kitchensink/include
    "-I$PACBREW_HOME/include"
    "-I$PACBREW_HOME/include/SDL2"
    -Iinclude
    -include "$VERSION_HEADER_RELATIVE"
)
if [[ "$CONFIGURATION" == "Release" ]]; then
    CCOMMON+=(-O2 -DNDEBUG)
else
    CCOMMON+=(-O0 -g3)
fi

CSOURCES=(
    src/playback/kitchensink_audio_clock.c
    src/playback/video_stubs.c
    vendor/SDL_kitchensink/src/kiterror.c
    vendor/SDL_kitchensink/src/kitformat.c
    vendor/SDL_kitchensink/src/kitlib.c
    vendor/SDL_kitchensink/src/kitplayer.c
    vendor/SDL_kitchensink/src/kitsource.c
    vendor/SDL_kitchensink/src/kitutils.c
    vendor/SDL_kitchensink/src/internal/kitdecoder.c
    vendor/SDL_kitchensink/src/internal/kitdecoderthread.c
    vendor/SDL_kitchensink/src/internal/kitdemuxer.c
    vendor/SDL_kitchensink/src/internal/kitdemuxerthread.c
    vendor/SDL_kitchensink/src/internal/kitlibstate.c
    vendor/SDL_kitchensink/src/internal/kitpacketbuffer.c
    vendor/SDL_kitchensink/src/internal/kittimer.c
    vendor/SDL_kitchensink/src/internal/libass.c
    vendor/SDL_kitchensink/src/internal/audio/kitaudio.c
    vendor/SDL_kitchensink/src/internal/audio/kitaudioutils.c
    vendor/SDL_kitchensink/src/internal/subtitle/kitatlas.c
    vendor/SDL_kitchensink/src/internal/subtitle/kitsubtitle.c
    vendor/SDL_kitchensink/src/internal/subtitle/kitsubtitlepacket.c
    vendor/SDL_kitchensink/src/internal/subtitle/renderers/kitsubass.c
    vendor/SDL_kitchensink/src/internal/subtitle/renderers/kitsubrenderer.c
    vendor/SDL_kitchensink/src/internal/utils/kithelpers.c
    vendor/SDL_kitchensink/src/internal/video/kitvideo.c
    vendor/SDL_kitchensink/src/internal/video/kitvideoutils.c
)

for source in "${CSOURCES[@]}"; do
    stem="$(basename "$source")"
    stem="${stem%.*}"
    object="build/ps5/$stem.o"
    echo "CC   $source"
    "$CC_PS5" "${CCOMMON[@]}" -c "$source" -o "$object"
    OBJECTS+=("$object")
done

SDL_VIDEO_OVERRIDE_OBJECT="build/ps5/SDL_ps5video.override.o"
cp -f "$SDL_VIDEO_OVERRIDE_SRC" "$PROJECT_ROOT/$SDL_VIDEO_OVERRIDE_OBJECT"
OBJECTS+=("$SDL_VIDEO_OVERRIDE_OBJECT")

UNSTRIPPED_ELF="build/ps5/bfplayer.unstripped.elf"
echo "LINK $UNSTRIPPED_ELF"
"$CC_PS5" -o "$UNSTRIPPED_ELF" "${OBJECTS[@]}" \
    "-L$PACBREW_HOME/lib" \
    -Wl,--start-group \
    -lSDL2_image -lSDL2_ttf -lSDL2 \
    -lavfilter -lavformat -lavcodec -lswscale -lswresample -lavutil \
    -lass -lfontconfig -lharfbuzz -lfribidi -lfreetype -lpng16 \
    -lbz2 -lz -llzma -lssl -lcrypto -liconv -lsamplerate -lexpat -ltinyxml2 -lsqlite3 \
    -lc++ -lc++abi -lunwind \
    -Wl,--end-group \
    -lSceSystemService -lSceUserService -lScePad -lSceAudioOut -lSceVideoOut \
    -lSceKeyboard -lSceImeDialog

ELF="$DIST_DIR/bfplayer.elf"
STRIP_TOOL="$PS5_PAYLOAD_SDK/bin/llvm-strip"
if [[ ! -x "$STRIP_TOOL" ]]; then
    STRIP_TOOL="$(command -v llvm-strip)"
fi
"$STRIP_TOOL" --strip-all -o "$ELF" "$UNSTRIPPED_ELF"

PLAYER_SIZE="$(stat -f%z "$ELF" 2>/dev/null || stat -c%s "$ELF")"
printf '#define BFPLAYER_PLAYER_UNCOMPRESSED_SIZE %sUL\n' "$PLAYER_SIZE" >> "$VERSION_HEADER"

COMPRESSED_PLAYER="$BUILD_DIR/bfplayer.elf.gz"
gzip -9 -n -c "$ELF" > "$COMPRESSED_PLAYER"

STANDALONE_COMMON=(
    -std=c17 -Wall -Wextra -Wpedantic -O2 -DNDEBUG
    -Iinclude -Isrc/launcher -Isrc/launcher/core
    "-I$PACBREW_HOME/include"
    -include "$VERSION_HEADER_RELATIVE"
)
STANDALONE_SOURCES=(
    src/launcher/standalone_launcher.c
    src/launcher/standalone_route.c
    src/launcher/core/pt.c
    src/launcher/core/elfldr.c
    src/launcher/core/hbldr.c
)
STANDALONE_OBJECTS=()
for source in "${STANDALONE_SOURCES[@]}"; do
    object_name="${source#src/launcher/}"
    object_name="$(echo "$object_name" | sed 's/[\/.]/_/g')"
    object="build/ps5/$object_name.o"
    echo "CC   $source"
    "$CC_PS5" "${STANDALONE_COMMON[@]}" -c "$source" -o "$object"
    STANDALONE_OBJECTS+=("$object")
done

STANDALONE="$DIST_DIR/bfplayer-standalone.elf"
echo "LINK $STANDALONE"
"$CC_PS5" -o "$STANDALONE" "${STANDALONE_OBJECTS[@]}" \
    -lkernel_sys -lSceSystemService -lSceUserService -lSceAppInstUtil \
    "-L$PACBREW_HOME/lib" -lz

HASH="$(shasum -a 256 "$ELF" | awk '{print $1}')"
STANDALONE_HASH="$(shasum -a 256 "$STANDALONE" | awk '{print $1}')"
printf '%s  bfplayer.elf\n' "$HASH" > "$DIST_DIR/bfplayer.sha256"
printf '%s  bfplayer-standalone.elf\n' "$STANDALONE_HASH" > "$DIST_DIR/bfplayer-standalone.sha256"

# --- standalone package ----------------------------------------------------
mkdir -p "$PACKAGE_DIR"
cp -f "$STANDALONE" "$PACKAGE_DIR/"
cp -f "$PROJECT_ROOT/LICENSE" "$PACKAGE_DIR/"
cp -f "$PROJECT_ROOT/THIRD_PARTY_NOTICES.md" "$PACKAGE_DIR/"
cp -f "$PROJECT_ROOT/docs/STANDALONE_LAUNCHER.md" "$PACKAGE_DIR/INSTALL.md"

COMPRESSED_BYTES="$(stat -f%z "$COMPRESSED_PLAYER" 2>/dev/null || stat -c%s "$COMPRESSED_PLAYER")"
BUILT_UTC="$(date -u +%Y-%m-%dT%H:%M:%S.000Z)"

python3 - "$DIST_DIR/build-manifest.json" "$PACKAGE_DIR" <<PYEOF
import hashlib, json, os, sys

manifest_path, package_dir = sys.argv[1], sys.argv[2]
payload_files = ["bfplayer-standalone.elf", "INSTALL.md", "LICENSE", "THIRD_PARTY_NOTICES.md"]
files = []
for rel in payload_files:
    p = os.path.join(package_dir, rel)
    data = open(p, "rb").read()
    files.append({
        "path": rel,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    })

manifest = {
    "name": "BFplayer",
    "version": "$VERSION",
    "target": "x86_64-sie-ps5",
    "package": "single standalone payload",
    "direct_tile": "PSMC00001 via loopback-only localhost:9040/launch",
    "websrv_required": False,
    "embedded_player": True,
    "embedded_player_compression": "gzip",
    "embedded_player_bytes": $PLAYER_SIZE,
    "embedded_player_compressed_bytes": $COMPRESSED_BYTES,
    "pacbrew": "v0.37",
    "ffmpeg": "7.0.1",
    "player_sha256": "$HASH",
    "standalone_sha256": "$STANDALONE_HASH",
    "built_utc": "$BUILT_UTC",
    "files": files,
}
with open(manifest_path, "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PYEOF
cp -f "$DIST_DIR/build-manifest.json" "$PACKAGE_DIR/"

ZIP="$DIST_DIR/BFplayer-standalone.zip"
rm -f "$ZIP"
(cd "$PACKAGE_ROOT" && zip -r -9 -X "$ZIP" "BFplayer-standalone" >/dev/null)
ZIP_HASH="$(shasum -a 256 "$ZIP" | awk '{print $1}')"
printf '%s  BFplayer-standalone.zip\n' "$ZIP_HASH" > "$ZIP.sha256"

# --- websrv package ---------------------------------------------------------
mkdir -p "$WEBSRV_PACKAGE_DIR/assets/fonts" "$WEBSRV_PACKAGE_DIR/sce_sys"
cp -f "$ELF" "$WEBSRV_PACKAGE_DIR/eboot.elf"
cp -f "$PROJECT_ROOT/assets/fonts/NotoSans-Regular.ttf" "$WEBSRV_PACKAGE_DIR/assets/fonts/"
cp -f "$PROJECT_ROOT/assets/fonts/OFL.txt" "$WEBSRV_PACKAGE_DIR/assets/fonts/"
cp -f "$PROJECT_ROOT/assets/icon0.png" "$WEBSRV_PACKAGE_DIR/sce_sys/icon0.png"
cp -f "$PROJECT_ROOT/docs/WEBSRV.md" "$WEBSRV_PACKAGE_DIR/INSTALL.md"
cp -f "$PROJECT_ROOT/LICENSE" "$WEBSRV_PACKAGE_DIR/"
cp -f "$PROJECT_ROOT/THIRD_PARTY_NOTICES.md" "$WEBSRV_PACKAGE_DIR/"

python3 - "$WEBSRV_PACKAGE_DIR" <<PYEOF
import hashlib, json, os, sys

package_dir = sys.argv[1]
payload_files = [
    "eboot.elf",
    "assets/fonts/NotoSans-Regular.ttf",
    "assets/fonts/OFL.txt",
    "sce_sys/icon0.png",
    "INSTALL.md",
    "LICENSE",
    "THIRD_PARTY_NOTICES.md",
]
files = []
for rel in payload_files:
    p = os.path.join(package_dir, rel)
    data = open(p, "rb").read()
    files.append({
        "path": rel,
        "bytes": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    })

manifest = {
    "name": "BFplayer",
    "version": "$VERSION",
    "target": "x86_64-sie-ps5",
    "package": "websrv homebrew folder",
    "install_directory": "BFplayer",
    "entrypoint": "BFplayer/eboot.elf",
    "websrv_required": True,
    "resident_bfplayer_launcher": False,
    "runtime_data": "/data/BFplayer",
    "pacbrew": "v0.37",
    "ffmpeg": "7.0.1",
    "player_sha256": "$HASH",
    "built_utc": "$BUILT_UTC",
    "files": files,
}
with open(os.path.join(package_dir, "build-manifest.json"), "w") as f:
    json.dump(manifest, f, indent=2)
    f.write("\n")
PYEOF

WEBSRV_ZIP="$DIST_DIR/BFplayer-websrv.zip"
rm -f "$WEBSRV_ZIP"
(cd "$PACKAGE_ROOT" && zip -r -9 -X "$WEBSRV_ZIP" "BFplayer" >/dev/null)
WEBSRV_ZIP_HASH="$(shasum -a 256 "$WEBSRV_ZIP" | awk '{print $1}')"
printf '%s  BFplayer-websrv.zip\n' "$WEBSRV_ZIP_HASH" > "$WEBSRV_ZIP.sha256"

echo
echo "== done =="
ls -la "$ELF" "$STANDALONE" "$ZIP" "$WEBSRV_ZIP"
echo "SHA256 (standalone): $STANDALONE_HASH"
echo "SHA256 (websrv zip): $WEBSRV_ZIP_HASH"
