#!/usr/bin/env bash
# HyperCat cross-distro build matrix — configure + build + ctest HyperCat inside one container per
# big-4 Linux family, proving source-build coverage. Podman-first (docker as fallback). No GitHub CI.
#
# Usage:
#   tools/build-matrix.sh                 # the whole matrix (docker/matrix.env)
#   tools/build-matrix.sh fedora          # only rows whose TAGSUFFIX contains "fedora"
#   HC_CONTAINER_ENGINE=docker tools/build-matrix.sh
#
# Each row: build the family image (deps + COPY source) then run docker/common/build-and-test.sh in it.
# seccomp=unconfined lets the hc_confine/hc_exec tests install their own seccomp filters under rootless
# podman (a test-environment allowance only — never how the shipped product runs).
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_DIR"

ENGINE="${HC_CONTAINER_ENGINE:-}"
[ -n "$ENGINE" ] || ENGINE="$(command -v podman || command -v docker || true)"
[ -n "$ENGINE" ] || { echo "error: no podman or docker found" >&2; exit 1; }
echo "engine: $ENGINE"

MATRIX="${HC_MATRIX_FILE:-docker/matrix.env}"
FILTER="${1:-}"

results=()
fail=0
trim() { printf '%s' "$1" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'; }
while IFS='|' read -r IMAGE DOCKERFILE SUFFIX; do
  IMAGE="$(trim "$IMAGE")"
  case "${IMAGE:-}" in ''|\#*) continue ;; esac
  DOCKERFILE="$(trim "$DOCKERFILE")"
  SUFFIX="$(trim "$SUFFIX")"
  [ -n "$FILTER" ] && case "$SUFFIX" in *"$FILTER"*) : ;; *) continue ;; esac

  tag="hypercat-ci:$SUFFIX"
  echo
  echo "=================================================================="
  echo "[$SUFFIX]  build image  ($IMAGE  via  $DOCKERFILE)"
  echo "=================================================================="
  if ! "$ENGINE" build --build-arg BASE="$IMAGE" -f "$DOCKERFILE" -t "$tag" . ; then
    echo "[$SUFFIX] IMAGE BUILD FAILED"
    results+=("FAIL(image)  $SUFFIX  ($IMAGE)"); fail=1; continue
  fi
  echo "------------------------------------------------------------------"
  echo "[$SUFFIX]  configure + build + ctest"
  echo "------------------------------------------------------------------"
  if "$ENGINE" run --rm --security-opt seccomp=unconfined "$tag"; then
    results+=("PASS         $SUFFIX  ($IMAGE)")
  else
    echo "[$SUFFIX] BUILD/TEST FAILED"
    results+=("FAIL(test)   $SUFFIX  ($IMAGE)"); fail=1
  fi
done < "$MATRIX"

echo
echo "===================== matrix summary ====================="
for r in "${results[@]}"; do echo "  $r"; done
echo "=========================================================="
[ "$fail" -eq 0 ] && echo "ALL GREEN" || echo "SOME ROWS FAILED"
exit "$fail"
