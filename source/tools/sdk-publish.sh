#!/usr/bin/env bash
# sdk-publish.sh — assemble the STANDALONE, Apache-2.0 HyperCat Tool SDK as its OWN directory/repo, kept separate
# from the HyperCat app so the two are never conflated. It builds the redistributable bundle
# (docker/common/sdk-package.sh) and syncs its contents into the SDK repo dir, then regenerates the repo's
# front-matter (README.md + .gitignore). HyperCat stays the DEVELOPMENT source of truth — the SDK is built from
# its C libs and verified by its gates; this script regenerates the published repo from it, so the SDK repo is
# fully reproducible and never hand-edited (edit in HyperCat under libs/hc_tool_sdk, then re-publish).
#
# The published repo contains ONLY Apache-2.0 SDK material: the public header, a prebuilt self-contained fat
# static lib (no HyperCat source), the Python helper, the Rust crate, examples, docs, and the licence files.
#
# Usage:  tools/sdk-publish.sh [target-dir]      (default: a sibling ../hypercat-tool-sdk of the HyperCat root)
# Then:   cd <target-dir> && git add -A && git commit     (the first run also `git init`s it if it is not a repo)
set -euo pipefail

ROOT="${HC_PROJECT_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
cd "$ROOT"

TARGET="${1:-$(cd "$ROOT/.." && pwd)/hypercat-tool-sdk}"
SDK_VERSION="${SDK_VERSION:-1.0}"
ARCH="$(uname -m)"
BUNDLE="hypercat-tool-sdk-${SDK_VERSION}-linux-${ARCH}"
STAGED="dist/$BUNDLE"

echo "sdk-publish: building the redistributable bundle (reuses docker/common/sdk-package.sh)"
HC_PACKAGE_OUT="dist" SDK_VERSION="$SDK_VERSION" bash docker/common/sdk-package.sh
[ -d "$STAGED" ] || { echo "sdk-publish: ERROR staged bundle $STAGED not found" >&2; exit 1; }

echo "sdk-publish: syncing the SDK into $TARGET"
mkdir -p "$TARGET"
# Regenerate the GENERATED subtrees (clear stale first), then copy the freshly-staged bundle. The repo's own
# git metadata is under .git/, which is never touched here.
for d in include lib python rust examples docs; do rm -rf "${TARGET:?}/$d"; done
rm -f "$TARGET"/LICENSE "$TARGET"/NOTICE "$TARGET"/THIRD_PARTY.txt "$TARGET"/README.txt "$TARGET"/VERSION
cp -a "$STAGED/." "$TARGET/"
# the repo's front page is README.md (written below); the bundle's plain-text README.txt is for the tarball form,
# redundant in a repo — drop it here so there is one canonical README.
rm -f "$TARGET/README.txt"

echo "sdk-publish: writing the repo front-matter (README.md + .gitignore)"
cat > "$TARGET/.gitignore" <<'GITIGNORE'
# Rust build output (the crate is shipped as source; authors build it)
target/
**/target/
Cargo.lock
# stray object files / OS cruft
*.o
.DS_Store
GITIGNORE

cat > "$TARGET/README.md" <<'README'
# HyperCat Tool SDK

Build your own tools for HyperCat — in C, C++, Python, or Rust — and HyperCat runs them as confined,
operator-approved subprocesses. This is the SDK only, licensed Apache-2.0. HyperCat itself (the application) is
separate software and is not part of this repository.

A tool is a small program that speaks a tiny line-framed JSON protocol over a Unix-domain socket: it checks in,
gets confined to a least-privilege kernel sandbox (Landlock + seccomp), and answers tool calls. The SDK does the
protocol and the sandbox for you; you write one function and register it.

## What is inside

```
include/hc_tool.h          the C/C++ header (the public ABI)
lib/libhc_tool_sdk.a       a self-contained static library (links with libc only; cJSON baked in)
python/hypercat_tool.py    the Python helper (pure standard library; no build step)
rust/hypercat-tool/        the Rust crate (links lib/ via the hc_tool_confine FFI)
examples/                  a worked example in each language
docs/AUTHORING.md          what to write
docs/BUILDING.md           how to compile, link, and install your tool
docs/PROTOCOL.md           the wire + manifest contract (write a tool in any language)
LICENSE, NOTICE, THIRD_PARTY.txt
```

## Quick start

- **C / C++:** include `hc_tool.h`, link `lib/libhc_tool_sdk.a`, call `hc_tool_main()`. See
  `examples/hello_tool/` and `docs/BUILDING.md`.
- **Python:** copy `python/hypercat_tool.py` next to your script and call `hypercat_tool.serve({...})`. See
  `examples/wordcount_tool_py/`.
- **Rust:** add the `rust/hypercat-tool` crate, point it at `lib/`, and call `hypercat_tool::serve(&[...])`. See
  `rust/hypercat-tool/examples/reverse.rs`.

## Platform

Linux x86-64, glibc 2.35 or newer (the static lib is built at that floor). Tool confinement is Linux-only;
HyperCat refuses to run a tool it cannot confine.

## Licence

Apache-2.0 (`LICENSE`). The bundled cJSON is MIT, preserved in `NOTICE` / `THIRD_PARTY.txt`.
README

echo "sdk-publish: done -> $TARGET"
echo "sdk-publish: next: cd '$TARGET' && git init (first time) && git add -A && git commit"
