#!/usr/bin/env bash
# HyperCat Tool SDK build + verify. Builds the redistributable SDK bundle inside an Ubuntu 22.04 container
# (glibc 2.35 floor — the prebuilt libhc_tool_sdk.a then links into a tool author's binary on any glibc >= 2.35),
# then PROVES it the way an outside developer would: in a CLEAN newer-distro container with ONLY a C compiler
# and the bundle (NO HyperCat source, NO libcjson), compile + link the example tool. Podman-first. x86_64 v1.
#
# Usage: tools/sdk-build.sh                       # build the SDK bundle + clean-room build verification
#        HC_SDK_SKIP_VERIFY=1 tools/sdk-build.sh   # just build the bundle
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"
ENGINE="${HC_CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
[ -n "$ENGINE" ] || { echo "error: no podman or docker found" >&2; exit 1; }
echo "engine: $ENGINE"

SDK_VERSION="${SDK_VERSION:-1.0}"
ARCH="$(uname -m)"
BUNDLE="hypercat-tool-sdk-${SDK_VERSION}-linux-${ARCH}"
TARBALL="${BUNDLE}.tar.gz"
DIST="${HC_PACKAGE_OUT:-$PROJECT_DIR/dist}"
mkdir -p "$DIST"

echo "== build the SDK image (ubuntu:22.04, glibc 2.35) =="
"$ENGINE" build -f docker/Dockerfile.sdk -t hypercat-sdk:build .

echo "== build the SDK bundle in-container (dist/ bind-mounted) =="
"$ENGINE" run --rm -e "SDK_VERSION=$SDK_VERSION" -v "$DIST:/src/dist:Z" hypercat-sdk:build
[ -f "$DIST/$TARBALL" ] || { echo "error: SDK bundle not produced: $DIST/$TARBALL" >&2; exit 1; }
echo "SDK bundle: $DIST/$TARBALL"
( cd "$DIST" && sha256sum -c "${TARBALL}.sha256" )

[ "${HC_SDK_SKIP_VERIFY:-0}" = "1" ] && { echo "(verify skipped)"; exit 0; }

# ---- clean-room build: an outsider, with ONLY a C compiler + the bundle, must be able to build a tool -------
verify() { # $1=image  $2=compiler-install-cmd
  echo; echo "== clean-room SDK build: $1 =="
  "$ENGINE" run --rm -v "$DIST:/dist:ro" "$1" bash -c '
    set -e
    '"$2"'
    cd /tmp && tar xzf "/dist/'"$TARBALL"'" && cd "'"$BUNDLE"'"
    echo "--- compile + link the example with ONLY the bundle (-lhc_tool_sdk -lpthread; no -lcjson) ---"
    cc examples/hello_tool/main.c -Iinclude -Llib -lhc_tool_sdk -lpthread -o /tmp/hello
    echo "--- ldd (must be libc/ld only — cJSON is static, no HyperCat runtime dep) ---"
    ldd /tmp/hello || true
    if ldd /tmp/hello 2>&1 | grep -iq "cjson\|not found"; then echo "FAIL: unexpected/unresolved dependency"; exit 2; fi
    rc=0; /tmp/hello >/tmp/o 2>&1 || rc=$?
    if [ "$rc" != "2" ]; then echo "FAIL: tool did not start as expected (rc=$rc)"; cat /tmp/o; exit 3; fi
    echo "OK: an outsider built + linked a tool from the bundle on $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2- | tr -d \"\\\"\")"
  '
}

# A C compiler ONLY — no HyperCat, no libcjson-dev:
APT='export DEBIAN_FRONTEND=noninteractive; apt-get update >/dev/null && apt-get install -y --no-install-recommends gcc libc6-dev >/dev/null'
DNF='dnf -y install gcc glibc-devel >/dev/null'

results=()
verify docker.io/library/ubuntu:24.04 "$APT" && results+=("PASS  ubuntu:24.04 (apt, glibc 2.39)")   || results+=("FAIL  ubuntu:24.04")
verify docker.io/library/almalinux:9  "$DNF" && results+=("PASS  almalinux:9 (dnf, RHEL 9 family)")  || results+=("FAIL  almalinux:9")

echo; echo "===== SDK clean-room verification ====="
echo "  bundle: $DIST/$TARBALL  (built on glibc 2.35)"
for r in "${results[@]}"; do echo "  $r"; done
