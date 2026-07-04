#!/usr/bin/env bash
# Run INSIDE the debian:11 floor container: build the relocatable Release bundle via tools/package.sh
# (UNCHANGED — it already does Release + -static-libstdc++/-static-libgcc + strip + tarball/sha256),
# then ASSERT the binaries reference no glibc symbol newer than the floor, advertise the floor in the
# artifact name, and write a FLOOR.txt. dist/ is bind-mounted from the host by tools/floor-build.sh.
set -euo pipefail
cd /src

FLOOR_GLIBC="${HC_FLOOR_GLIBC:-2.35}" # ubuntu:22.04 == glibc 2.35 (lowest CLEAN glibc with SYS_landlock_*)
VERSION="${HC_VERSION:-0.1.2}"
ARCH="$(uname -m)"
BUNDLE="HyperCat-${VERSION}-linux-${ARCH}"
STAGE="dist/$BUNDLE"

echo "== cmake (pip-provided, portable) =="; cmake --version | head -1
tools/package.sh

echo
echo "== glibc floor assertion (<= GLIBC_${FLOOR_GLIBC}) =="
maxallowed="GLIBC_${FLOOR_GLIBC}"
worst=""
for b in hypercat agentd hc_audio_helper; do
  v="$(readelf -V "$STAGE/$b" 2>/dev/null | grep -oE 'GLIBC_[0-9]+\.[0-9]+' | sort -V | tail -1)"
  echo "  $b: max ${v:-none}"
  if [ -n "$v" ]; then
    worst="$(printf '%s\n%s\n' "${worst:-GLIBC_0.0}" "$v" | sort -V | tail -1)"
  fi
done
echo "  worst observed: ${worst:-none}   ceiling: $maxallowed"
if [ -n "$worst" ] && [ "$(printf '%s\n%s\n' "$worst" "$maxallowed" | sort -V | tail -1)" != "$maxallowed" ]; then
  echo "ERROR: a binary needs $worst (> $maxallowed) — it would NOT run on glibc ${FLOOR_GLIBC}." >&2
  exit 1
fi
echo "  OK — floor holds."

echo
echo "== advertise the floor in the artifact name =="
SRC="dist/${BUNDLE}.tar.gz"
DST="dist/${BUNDLE}-glibc${FLOOR_GLIBC}.tar.gz"
mv "$SRC" "$DST"
rm -f "${SRC}.sha256"
( cd dist && sha256sum "$(basename "$DST")" > "$(basename "$DST").sha256" )
pretty="$(grep -m1 '^PRETTY_NAME=' /etc/os-release | cut -d= -f2- | tr -d '"')"
cat > "dist/${BUNDLE}-glibc${FLOOR_GLIBC}.FLOOR.txt" <<EOF
HyperCat glibc-floor build
  built on:        ${pretty:-debian:11}
  glibc floor:     ${FLOOR_GLIBC}  (runs on any glibc >= ${FLOOR_GLIBC}, ${ARCH})
  max GLIBC symbol used: ${worst:-none}
  static:          libstdc++ + libgcc (embedded via tools/package.sh)
  runtime sonames: libcurl.so.4 libglfw.so.3 libopenal.so.1 GLVND(GL/GLX) libX11.so.6 libsecret-1.so.0 libglib-2.0.so.0 libgobject-2.0.so.0
  install the runtime deps with the bundled install.sh (family-aware: apt/dnf/pacman/zypper).
EOF
echo "floor bundle: $DST"
echo "  sha256: $(cut -d' ' -f1 < "${DST}.sha256")"
