#!/usr/bin/env bash
# Distro-agnostic build + test, run INSIDE a matrix container (the source is COPY'd to /src by the
# Dockerfile). Proves HyperCat configures, compiles clean under -Wall -Wextra -Wpedantic, and passes
# ctest on this distro. The matrix's job is cross-distro BUILD + functional coverage; sanitizers are
# OFF here (the host dev tree runs the full ASan/UBSan/LSan pass) because the ASan runtime is packaged
# inconsistently across distros (e.g. Fedora ships no libasan by default) and that variance is friction,
# not signal. The Release flags + the glibc-floor binary are covered by tools/floor-build.sh.
#
# cmake_minimum_required(VERSION 3.20) makes `cmake -B` fail fast with a clear message on a too-old
# CMake (e.g. RHEL-8-era), so no explicit version assert is needed here.
set -euo pipefail
cd /src

echo "== toolchain =="
cmake --version | head -1
{ cc --version || gcc --version; } 2>/dev/null | head -1 || true
echo

BUILD=build-matrix
rm -rf "$BUILD"
cmake -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DHC_BUILD_TESTS=ON -DHC_SANITIZERS=OFF
cmake --build "$BUILD"
ctest --test-dir "$BUILD" --output-on-failure --timeout 180
