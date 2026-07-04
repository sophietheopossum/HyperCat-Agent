# HyperCat cross-distro build matrix

Local container recipes (podman-first, docker fallback) that prove HyperCat builds + passes its tests
across the **big-4 glibc Linux families**, and that build a **glibc-floor binary** that runs on most
of them from one tarball. No GitHub CI — these run on your machine.

**Zero C/C++ source changes** underpin this: the code already guards every platform/kernel/arch
difference at runtime (`#if defined(__linux__)`, `#ifdef __NR_*`, Landlock ABI-masking, the font and
`/proc/self/exe` fallbacks). The only build-file change that widens coverage is `libs/hc_json/
CMakeLists.txt` — a find-or-fetch for cJSON so a distro that doesn't package it still builds.

## Quick start

```bash
tools/build-matrix.sh                 # the whole matrix (docker/matrix.env): build + ctest per distro
tools/build-matrix.sh fedora          # only rows whose tag contains "fedora"
tools/floor-build.sh                  # build the glibc-2.31 floor binary + verify it on newer distros
HC_CONTAINER_ENGINE=docker tools/build-matrix.sh
```

## What's here

| File | Role |
|---|---|
| `matrix.env` | the `IMAGE\|DOCKERFILE\|TAGSUFFIX` matrix (add a release = add a line) |
| `common/build-and-test.sh` | distro-agnostic: `cmake` (Debug+tests) → build → `ctest` (run inside each image) |
| `common/floor-package.sh` | runs in the floor image: `tools/package.sh` + the glibc-ceiling assertion |
| `Dockerfile.debian` | apt family — `ARG BASE=ubuntu:22.04\|ubuntu:24.04\|debian:12\|…` |
| `Dockerfile.fedora` | dnf — Fedora proper (`ARG BASE=fedora:41\|42`) |
| `Dockerfile.enterprise` | dnf + EPEL + CRB — Rocky/Alma/RHEL 9 (cjson/glfw/openal live in EPEL) |
| `Dockerfile.arch` | pacman — Arch/Manjaro (rolling; no split `-dev` packages) |
| `Dockerfile.suse` | zypper — openSUSE Leap/Tumbleweed |
| `Dockerfile.nocjson` | a Debian base with **no** `libcjson-dev` — proves the cJSON FetchContent fallback |
| `Dockerfile.floor` | ubuntu:22.04 (glibc 2.35, lowest clean glibc with `SYS_landlock_*`) — floor builder |

## Build dependencies per family

Compiler, **cmake ≥ 3.20**, ninja, pkg-config, git, **ca-certificates** (the ImGui FetchContent clone
is over HTTPS), plus the `-dev`/`-devel` headers for curl/cjson/glfw/openal/Mesa-GL/X11. Let the
distro's `glfw*` dev package pull its own X11/Wayland chain — don't hand-list it.

| | apt (Debian/Ubuntu) | dnf (Fedora) | dnf+EPEL (Rocky/Alma 9) | pacman (Arch) | zypper (openSUSE) |
|---|---|---|---|---|---|
| toolchain | `build-essential cmake ninja-build pkg-config git ca-certificates` | `gcc gcc-c++ cmake ninja-build pkgconf-pkg-config git-core ca-certificates` | + `epel-release` + CRB enabled | `base-devel cmake ninja git` | `gcc gcc-c++ cmake ninja pkg-config git-core ca-certificates` |
| curl | `libcurl4-openssl-dev` | `libcurl-devel` | `libcurl-devel` | `curl` | `libcurl-devel` |
| cJSON | `libcjson-dev` | `cjson-devel` | `cjson-devel` (EPEL) | `cjson` | `libcjson-devel` |
| GLFW | `libglfw3-dev` | `glfw-devel` | `glfw-devel` (EPEL) | `glfw` | `libglfw3-devel` |
| OpenAL | `libopenal-dev` | `openal-soft-devel` | `openal-soft-devel` (EPEL) | `openal` | `openal-soft-devel` |
| libsecret (OS keychain) | `libsecret-1-dev` | `libsecret-devel` | `libsecret-devel` | `libsecret` | `libsecret-devel` |
| OpenGL | `libgl1-mesa-dev` | `mesa-libGL-devel` | `mesa-libGL-devel` (CRB) | `mesa` | `Mesa-libGL-devel` |
| X11 | `libx11-dev` | `libX11-devel` | `libX11-devel` (CRB) | `libx11` | `libX11-devel` |

(Runtime — what end-users install — is in `packaging/install.sh`, which is family-aware. Preview it on
any distro with `./install.sh --dry-run`.)

## Coverage + bounds (the "extent")

- **Source build:** essentially every modern glibc distro. Lower bound = cmake ≥ 3.20 (excludes
  RHEL-8-era) + the deps being available (cJSON falls back to a fetch if unpackaged).
- **Floor binary:** glibc ≥ 2.35, x86_64, from one tarball — Ubuntu 22.04+, Debian 12+, Fedora 36+, Arch,
  openSUSE Leap 15.4+, AND RHEL/Alma/Rocky 9.x (their "2.34" glibc backports the GLIBC_2.35 symbols — the
  run-on-newer AlmaLinux 9 check confirms it). 2.35 is the floor because it's the lowest clean, supported
  glibc with the `SYS_landlock_*` macros the confinement code calls. Static libstdc++/libgcc removes the
  C++ ABI variable; the per-distro installer supplies the runtime sonames.
- **Not v1:** musl/Alpine (separate libc ABI), glibc < 2.34 (Ubuntu 20.04 / Debian 11 / RHEL-8-era — the
  confinement code won't compile without a `__has_include`/`SYS_landlock_*`-fallback source guard), aarch64
  binaries (source is ready; the build lane is a later `--arch` flip), AppImage/.deb/.rpm, macOS.

## Gotchas

1. **Podman-first.** This repo's machine has podman, not docker. The drivers autodetect; override with
   `HC_CONTAINER_ENGINE`.
2. **The matrix runs `--security-opt seccomp=unconfined`** so the `hc_confine`/`hc_exec` tests can
   install their own seccomp filters under rootless podman — a *test-environment* allowance only, never
   how the shipped product runs.
3. **LSan off in containers** (`ASAN_OPTIONS=detect_leaks=0`): leak detection uses ptrace-style ops that
   rootless containers block. ASan+UBSan stay on; the host dev tree runs full LSan.
4. **cJSON spread:** EPEL-only on RHEL/Rocky/Alma 9 — `Dockerfile.enterprise` enables EPEL+CRB first.
   The `Dockerfile.nocjson` row proves the fetch fallback covers any gap.
5. **Rolling rows (Arch, Tumbleweed) are canaries**, not gates — a red there usually means upstream
   ImGui/GLFW/compiler moved. The pinned releases (Ubuntu/Debian/Rocky/Leap) are the authoritative gate.
6. **Headless:** `ui_smoke` self-skips with no display and the rest are pure-logic or spawn `agentd` —
   so `ctest` is green with no X server. The matrix proves build+link+logic, not GL rendering.
