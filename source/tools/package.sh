#!/usr/bin/env bash
# HyperCat packaging — build a relocatable Release bundle + tarball for an Ubuntu test release.
#
# Produces dist/HyperCat-<ver>-linux-<arch>/ (the three programs, flat, so they resolve each
# other as siblings via /proc/self/exe — see app/exe_path.hpp) plus the installer and the
# README/DISCLAIMER/THIRD_PARTY notices, then a .tar.gz + .sha256. Release binaries are
# stripped. Build on the OLDEST target OS you want to support (a 24.04 build needs 24.04's glibc;
# build in an ubuntu:22.04 container to also cover 22.04 — the installer is already version-aware).
#
# Usage: tools/package.sh
#   HC_VERSION=0.1.2  HC_RELEASE_BUILD=build-release  HC_PACKAGE_OUT=dist   (overridable)
set -euo pipefail

PROJECT_DIR="${HC_PROJECT_DIR:-$(cd "$(dirname "$0")/.." && pwd)}"
VERSION="${HC_VERSION:-0.1.2}"
ARCH="$(uname -m)"
BUILD_DIR="${HC_RELEASE_BUILD:-$PROJECT_DIR/build-release}"
OUT_DIR="${HC_PACKAGE_OUT:-$PROJECT_DIR/dist}"
BUNDLE="HyperCat-${VERSION}-linux-${ARCH}"
STAGE="$OUT_DIR/$BUNDLE"

echo "==> Release build ($BUILD_DIR)"
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
  cmake -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DHC_BUILD_TESTS=OFF -DHC_SANITIZERS=OFF -DHC_ENABLE_TEST_GATES=OFF \
    -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
fi
cmake --build "$BUILD_DIR"

HOST="$BUILD_DIR/app/hypercat"
WORKER="$BUILD_DIR/agentd/agentd"
AUDIO="$BUILD_DIR/audio_helper/hc_audio_helper"
# the managed-runtime tool launcher — a runtime sibling like agentd/hc_audio_helper, resolved next to the host
# via /proc/self/exe (exe_path.hpp). Ships with the app so a managed (non-C) third-party tool can be jailed+exec'd.
LAUNCH="$BUILD_DIR/tool_launch/hc_tool_launch"
for b in "$HOST" "$WORKER" "$AUDIO" "$LAUNCH"; do
  [ -x "$b" ] || { echo "package: missing build output $b" >&2; exit 1; }
done

echo "==> Stage $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE"
install -m 0755 "$HOST"   "$STAGE/hypercat"
install -m 0755 "$WORKER" "$STAGE/agentd"
install -m 0755 "$AUDIO"  "$STAGE/hc_audio_helper"
install -m 0755 "$LAUNCH" "$STAGE/hc_tool_launch"

echo "==> Strip (release: drop symbols)"
strip --strip-unneeded "$STAGE/hypercat" "$STAGE/agentd" "$STAGE/hc_audio_helper" "$STAGE/hc_tool_launch"

echo "==> Ship installer + notices + icon"
install -m 0755 "$PROJECT_DIR/packaging/install.sh"     "$STAGE/install.sh"
install -m 0644 "$PROJECT_DIR/packaging/README.txt"     "$STAGE/README.txt"
install -m 0644 "$PROJECT_DIR/packaging/MANUAL.md"      "$STAGE/MANUAL.md"
install -m 0644 "$PROJECT_DIR/packaging/LICENSE.txt"    "$STAGE/LICENSE.txt"
install -m 0644 "$PROJECT_DIR/CHANGELOG.md"             "$STAGE/CHANGELOG.md"
install -m 0644 "$PROJECT_DIR/packaging/DISCLAIMER.txt" "$STAGE/DISCLAIMER.txt"
install -m 0644 "$PROJECT_DIR/packaging/THIRD_PARTY.txt" "$STAGE/THIRD_PARTY.txt"
ICON_SRC="$PROJECT_DIR/Docs/resources/hypercat_mascot_128.png"
[ -r "$ICON_SRC" ] && install -m 0644 "$ICON_SRC" "$STAGE/hypercat.png"

echo "==> Tarball + checksum"
TARBALL="$OUT_DIR/${BUNDLE}.tar.gz"
tar -czf "$TARBALL" -C "$OUT_DIR" "$BUNDLE"
( cd "$OUT_DIR" && sha256sum "${BUNDLE}.tar.gz" > "${BUNDLE}.tar.gz.sha256" )

echo
echo "bundle:  $TARBALL"
echo "sha256:  $(cut -d' ' -f1 < "$TARBALL.sha256")"
echo "size:    $(du -h "$TARBALL" | cut -f1)"
echo "runtime .so deps (hypercat):"
ldd "$STAGE/hypercat" | awk '{print "  "$1}' | sort -u | head -60
