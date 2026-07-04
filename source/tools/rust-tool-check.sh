#!/usr/bin/env bash
# rust-tool-check.sh — the CI gate for the Rust Tool SDK path (the one SDK language not verified by the default
# `ctest` run, since this dev box has no Rust toolchain). It proves two things on a runner that DOES have cargo:
#   1. the `hypercat-tool` crate BUILDS + LINKS against the SDK's fat static lib (the hc_tool_confine FFI resolves);
#   2. the compiled `reverse` example RUNS as a confined NATIVE tool — it self-confines via the FFI, checks in over
#      the bus, and serves an invoke — exercised through the real toolhost gate (test_toolhost's native-Rust case).
#
# SAFE ANYWHERE: with no `cargo` on PATH it SKIPS (exit 0), so it is harmless to run on this box or in a minimal
# CI image. Wire it into CI on a runner that has cargo + internet (the crate pulls serde_json from crates.io).
#
# A minimal CI job (GitHub Actions shown; adapt to any runner):
#   jobs:
#     rust-sdk:
#       runs-on: ubuntu-latest                 # glibc >= 2.35
#       steps:
#         - uses: actions/checkout@v4
#         - run: sudo apt-get update && sudo apt-get install -y cmake ninja-build gcc g++
#         - uses: dtolnay/rust-toolchain@stable
#         - run: tools/rust-tool-check.sh
#
# Until this gate is green on a real toolchain, treat the Rust path as "provided source, not yet verified".
set -euo pipefail

ROOT="${HC_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT"

if ! command -v cargo >/dev/null 2>&1; then
    echo "rust-tool-check: cargo not found on PATH — SKIP (install a Rust toolchain to verify the Rust SDK path)"
    exit 0
fi

CRATE="libs/hc_tool_sdk/rust/hypercat-tool"
[ -f "$CRATE/Cargo.toml" ] || { echo "rust-tool-check: ERROR no crate at $CRATE" >&2; exit 1; }

echo "rust-tool-check: [1/4] build the SDK fat static lib"
# shellcheck source=docker/common/build-fat-lib.sh
source "$ROOT/docker/common/build-fat-lib.sh"
build_fat_lib "build-sdk" "$ROOT" # sets FAT_LIB
# the crate's build.rs links `-lhc_tool_sdk` -> expose the fat lib under exactly that name on a search path
LIBDIR="$ROOT/build-sdk/rustlib"
mkdir -p "$LIBDIR"
cp "$FAT_LIB" "$LIBDIR/libhc_tool_sdk.a"

echo "rust-tool-check: [2/4] cargo build the reverse example against the fat lib"
( cd "$CRATE" && HC_TOOL_SDK_LIB_DIR="$LIBDIR" cargo build --release --example reverse )
REVBIN="$ROOT/$CRATE/target/release/examples/reverse"
[ -x "$REVBIN" ] || { echo "rust-tool-check: ERROR reverse example not built at $REVBIN" >&2; exit 1; }

echo "rust-tool-check: [3/4] build the toolhost gate (release-clean dev build; no test gates needed)"
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug >/dev/null
cmake --build build --target test_toolhost hc_tool_launch

echo "rust-tool-check: [4/4] run the native-Rust end-to-end (launch -> self-confine -> checkin -> invoke)"
HC_RUST_REVERSE_BIN="$REVBIN" ASAN_OPTIONS=detect_leaks=0 ctest --test-dir build -R '^toolhost$' --output-on-failure

echo "rust-tool-check: OK — the Rust crate built + a native Rust tool ran confined end-to-end"
