#!/usr/bin/env bash
# HyperCat glibc-floor build + verify. Builds the relocatable Release bundle inside an Ubuntu 22.04
# container (glibc 2.35 — the lowest CLEAN, supported glibc with the SYS_landlock_* macros the confinement
# code needs) so the binary runs forward-compatibly on any glibc >= 2.35 distro, then proves it: loads the
# 2.35-linked binary on other distros (AlmaLinux 9 [the RHEL family], Ubuntu 24.04, Fedora, Arch) with only
# the runtime packages present. Podman-first. x86_64 only for v1 (aarch64 is a later --arch flip).
#
# Usage: tools/floor-build.sh            # build the floor bundle + run-on-newer verification
#        HC_FLOOR_SKIP_VERIFY=1 tools/floor-build.sh   # just build the bundle
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"
ENGINE="${HC_CONTAINER_ENGINE:-$(command -v podman || command -v docker || true)}"
[ -n "$ENGINE" ] || { echo "error: no podman or docker found" >&2; exit 1; }
echo "engine: $ENGINE"

FLOOR_GLIBC="${HC_FLOOR_GLIBC:-2.35}"
VERSION="${HC_VERSION:-0.1.2}"
ARCH="$(uname -m)"
BUNDLE="HyperCat-${VERSION}-linux-${ARCH}"
TARBALL="${BUNDLE}-glibc${FLOOR_GLIBC}.tar.gz"
DIST="${HC_PACKAGE_OUT:-$PROJECT_DIR/dist}"   # honor a dist subdirectory, like tools/package.sh
mkdir -p "$DIST"

echo "== build the floor image (ubuntu:22.04, glibc ${FLOOR_GLIBC}) =="
"$ENGINE" build -f docker/Dockerfile.floor -t hypercat-floor:build .

echo "== build + floor-assert the bundle in-container (dist/ bind-mounted) =="
"$ENGINE" run --rm -v "$DIST:/src/dist:Z" hypercat-floor:build
[ -f "$DIST/$TARBALL" ] || { echo "error: floor bundle not produced: $DIST/$TARBALL" >&2; exit 1; }
echo "floor bundle: $DIST/$TARBALL"

[ "${HC_FLOOR_SKIP_VERIFY:-0}" = "1" ] && { echo "(verify skipped)"; exit 0; }

# ---- run-on-newer: the 2.31 binary must load on newer-glibc distros with runtime deps only --------
verify() { # $1=image  $2=runtime-install-cmd
  echo; echo "== run-on-newer: $1 =="
  "$ENGINE" run --rm -v "$DIST:/dist:ro" "$1" bash -c '
    set -e
    '"$2"'
    cd /tmp && tar xzf "/dist/'"$TARBALL"'" && cd "'"$BUNDLE"'"
    echo "--- ldd hypercat ---"; ldd ./hypercat || true
    if ldd ./hypercat 2>&1 | grep -i "not found"; then echo "FAIL: unresolved soname"; exit 2; fi
    out="$(timeout 25 ./hypercat --screenshot /tmp/s.ppm 2>&1 || true)"
    echo "$out" | head -15
    if echo "$out" | grep -qiE "version .GLIBC_[0-9.]+. not found"; then echo "FAIL: glibc mismatch"; exit 3; fi
    echo "OK: floor binary loads on $(grep -m1 PRETTY_NAME /etc/os-release | cut -d= -f2- | tr -d \"\\\"\")"
  '
}

# Runtime sonames the floor binary needs: libcurl.so.4, libglfw.so.3, libopenal.so.1, libOpenGL.so.0
# (GLVND), libX11.so.6, + libsecret-1.so.0 / libglib-2.0.so.0 / libgobject-2.0.so.0 (the OS keychain backend).
# NO cJSON — it's statically linked into the floor binary (see Dockerfile.floor).
APT='export DEBIAN_FRONTEND=noninteractive; apt-get update >/dev/null && apt-get install -y --no-install-recommends libcurl4t64 libglfw3 libopenal1 libopengl0 libglx0 libx11-6 libsecret-1-0 libglib2.0-0t64 >/dev/null'
# --allowerasing: RHEL/Alma container bases ship libcurl-minimal, which conflicts with the full libcurl;
# this swaps it (a no-op on a normal install that already has full libcurl). The flag goes AFTER `install`
# so it works on BOTH dnf4 (Alma 9) and dnf5 (Fedora 41 rejects a global-position --allowerasing).
DNF='dnf -y install --allowerasing libcurl glfw openal-soft libglvnd-glx libglvnd-opengl libX11 libxcb libsecret glib2 >/dev/null'
DNF_EL='dnf -y install epel-release >/dev/null 2>&1 && dnf -y install --allowerasing libcurl glfw openal-soft libglvnd-glx libglvnd-opengl libX11 libxcb libsecret glib2 >/dev/null'
PAC='pacman -Sy --noconfirm curl glfw openal libglvnd libx11 libxcb libsecret glib2 >/dev/null'

results=()
# the RHEL family (Alma/Rocky 9 — "2.34" glibc backports the 2.35 symbols) — runtime packages only:
verify docker.io/library/almalinux:9      "$DNF_EL" && results+=("PASS  almalinux:9 (dnf+EPEL, RHEL 9 family)") || results+=("FAIL  almalinux:9")
# forward-compat on newer-glibc distros across package managers:
verify docker.io/library/ubuntu:24.04     "$APT" && results+=("PASS  ubuntu:24.04 (apt, glibc 2.39)") || results+=("FAIL  ubuntu:24.04")
verify docker.io/library/fedora:41        "$DNF" && results+=("PASS  fedora:41 (dnf)")     || results+=("FAIL  fedora:41")
verify docker.io/library/archlinux:latest "$PAC" && results+=("PASS  archlinux (pacman)")  || results+=("FAIL  archlinux")

echo; echo "===== floor verification ====="
echo "  bundle: $DIST/$TARBALL  (built on glibc ${FLOOR_GLIBC})"
for r in "${results[@]}"; do echo "  $r"; done
