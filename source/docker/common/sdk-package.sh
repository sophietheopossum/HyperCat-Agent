#!/usr/bin/env bash
# sdk-package.sh — build the redistributable HyperCat Tool SDK bundle. Run from the repo ROOT (the SDK
# container's WORKDIR, or natively for dev). Mirrors tools/package.sh's staging idiom, but for the SDK:
#   - Release, NO sanitizers (an ASan .a would force the author's tool to link ASan).
#   - HC_JSON_FORCE_FETCH=ON so cJSON is built STATIC and baked into the lib (self-contained, no libcjson.so dep).
#   - Combine the SDK lib + its deps (transport/json/confine) + static cJSON into ONE fat libhc_tool_sdk.a.
# Outputs <HC_PACKAGE_OUT>/hypercat-tool-sdk-<SDK_VERSION>-linux-<arch>.tar.gz + .sha256.
set -euo pipefail

ROOT="${HC_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}"
cd "$ROOT"

SDK_VERSION="${SDK_VERSION:-1.0}"
BUILD_DIR="${SDK_BUILD_DIR:-build-sdk}"
OUT_DIR="${HC_PACKAGE_OUT:-dist}"
ARCH="$(uname -m)"
SDKDIR="libs/hc_tool_sdk"
BUNDLE="hypercat-tool-sdk-${SDK_VERSION}-linux-${ARCH}"
STAGE="$OUT_DIR/$BUNDLE"

# Build the fat lib (the SDK lib + transport/json/confine + static cJSON, combined into one archive). The
# configure + combine + sanity is factored into build-fat-lib.sh so THIS packager and the Rust CI gate
# (tools/rust-tool-check.sh) share ONE source of truth for the lib set — they cannot drift.
# shellcheck source=docker/common/build-fat-lib.sh
source "$ROOT/docker/common/build-fat-lib.sh"
build_fat_lib "$BUILD_DIR" "$ROOT"
FAT="$FAT_LIB"

# Stage the bundle.
echo "sdk-package: staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/include" "$STAGE/lib" "$STAGE/examples/hello_tool" "$STAGE/docs"
install -m 0644 "$SDKDIR/include/hc_tool.h"                 "$STAGE/include/hc_tool.h"
install -m 0644 "$FAT"                                      "$STAGE/lib/libhc_tool_sdk.a"
install -m 0644 "$SDKDIR/examples/hello_tool/main.c"        "$STAGE/examples/hello_tool/main.c"
install -m 0644 "$SDKDIR/examples/hello_tool/manifest.json" "$STAGE/examples/hello_tool/manifest.json"
install -m 0644 "$SDKDIR/docs/AUTHORING.md"                 "$STAGE/docs/AUTHORING.md"
install -m 0644 "$SDKDIR/docs/BUILDING.md"                  "$STAGE/docs/BUILDING.md"
install -m 0644 "$SDKDIR/docs/PROTOCOL.md"                  "$STAGE/docs/PROTOCOL.md"
install -m 0644 "$SDKDIR/LICENSE"                           "$STAGE/LICENSE"
install -m 0644 "$SDKDIR/NOTICE"                            "$STAGE/NOTICE"
install -m 0644 "$SDKDIR/THIRD_PARTY.txt"                   "$STAGE/THIRD_PARTY.txt"
install -m 0644 "$SDKDIR/README.txt"                        "$STAGE/README.txt"
install -m 0644 "$SDKDIR/VERSION"                           "$STAGE/VERSION"

# the non-C path: the Python helper (managed runtime) + the Rust crate (native runtime) + their examples. The
# Python helper is pure-stdlib (no build); the Rust crate is source (the author builds it with cargo against the
# fat lib above — see docs/BUILDING.md). The managed launcher itself ships with the APP bundle, not here.
mkdir -p "$STAGE/python" "$STAGE/examples/wordcount_tool_py" \
         "$STAGE/rust/hypercat-tool/src" "$STAGE/rust/hypercat-tool/examples"
install -m 0644 "$SDKDIR/python/hypercat_tool.py"               "$STAGE/python/hypercat_tool.py"
install -m 0644 "$SDKDIR/examples/wordcount_tool_py/tool.py"        "$STAGE/examples/wordcount_tool_py/tool.py"
install -m 0644 "$SDKDIR/examples/wordcount_tool_py/manifest.json" "$STAGE/examples/wordcount_tool_py/manifest.json"
install -m 0644 "$SDKDIR/rust/hypercat-tool/Cargo.toml"            "$STAGE/rust/hypercat-tool/Cargo.toml"
install -m 0644 "$SDKDIR/rust/hypercat-tool/build.rs"             "$STAGE/rust/hypercat-tool/build.rs"
install -m 0644 "$SDKDIR/rust/hypercat-tool/src/lib.rs"           "$STAGE/rust/hypercat-tool/src/lib.rs"
install -m 0644 "$SDKDIR/rust/hypercat-tool/examples/reverse.rs"          "$STAGE/rust/hypercat-tool/examples/reverse.rs"
install -m 0644 "$SDKDIR/rust/hypercat-tool/examples/reverse.manifest.json" "$STAGE/rust/hypercat-tool/examples/reverse.manifest.json"

echo "sdk-package: tarball + sha256"
tar -czf "$OUT_DIR/${BUNDLE}.tar.gz" -C "$OUT_DIR" "$BUNDLE"
( cd "$OUT_DIR" && sha256sum "${BUNDLE}.tar.gz" > "${BUNDLE}.tar.gz.sha256" )
echo "sdk-package: done -> $OUT_DIR/${BUNDLE}.tar.gz"
