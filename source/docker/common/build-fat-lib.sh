#!/usr/bin/env bash
# build-fat-lib — configure an SDK-only Release build and combine the SDK's FAT static lib: the SDK lib plus its
# transport/json/confine deps plus static cJSON, all in ONE libhc_tool_sdk_fat.a, so an author (or the Rust CI
# gate) links a single -lhc_tool_sdk with no further HyperCat archives. SOURCED by docker/common/sdk-package.sh
# (which ships the fat lib) and tools/rust-tool-check.sh (which links the Rust crate against it) — ONE source of
# truth for the lib set, so the two cannot drift.
#
# Usage:  source docker/common/build-fat-lib.sh ; build_fat_lib <build_dir> [repo_root]
# On success sets FAT_LIB to the produced archive and returns 0; prints a reason and returns non-zero on error.
# Requires: cmake + ninja + ar + nm (ranlib optional). HC_JSON_FORCE_FETCH=ON is set here so cJSON is static
# (a system libcjson would leave the lib non-self-contained).

build_fat_lib() {
    local build_dir="$1"
    local root="${2:-$PWD}"
    if [ -z "$build_dir" ]; then
        echo "build-fat-lib: usage: build_fat_lib <build_dir> [repo_root]" >&2
        return 2
    fi

    echo "build-fat-lib: configuring $build_dir (Release, no sanitizers, SDK-only, static cJSON)"
    cmake -B "$build_dir" -S "$root" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DHC_BUILD_TESTS=OFF -DHC_SANITIZERS=OFF -DHC_ENABLE_TEST_GATES=OFF \
        -DHC_SDK_ONLY=ON -DHC_JSON_FORCE_FETCH=ON >/dev/null || return 1
    cmake --build "$build_dir" --target hc_tool_sdk || return 1

    # the four HyperCat archives (known paths) + the static cJSON (its FetchContent subdir name varies — glob it)
    local archives=(
        "$build_dir/libs/hc_tool_sdk/libhc_tool_sdk.a"
        "$build_dir/libs/hc_transport/libhc_transport.a"
        "$build_dir/libs/hc_json/libhc_json.a"
        "$build_dir/libs/hc_confine/libhc_confine.a"
    )
    local a
    for a in "${archives[@]}"; do
        [ -f "$a" ] || { echo "build-fat-lib: ERROR missing archive $a" >&2; return 1; }
    done
    local cjson
    cjson="$(find "$build_dir/_deps" -name 'libcjson.a' -print -quit 2>/dev/null || true)"
    if [ -z "$cjson" ]; then
        echo "build-fat-lib: ERROR static cJSON not found under $build_dir/_deps — the build linked a SYSTEM" >&2
        echo "               libcjson, so the lib would NOT be self-contained (need HC_JSON_FORCE_FETCH=ON)." >&2
        return 1
    fi

    FAT_LIB="$build_dir/libhc_tool_sdk_fat.a"
    rm -f "$FAT_LIB"
    {
        echo "create $FAT_LIB"
        for a in "${archives[@]}" "$cjson"; do echo "addlib $a"; done
        echo "save"
        echo "end"
    } | ar -M || return 1
    ranlib "$FAT_LIB" 2>/dev/null || true

    # Sanity: the public entry point + a cJSON symbol must both be present. Capture nm ONCE and match in-shell
    # (piping into `grep -q` would SIGPIPE nm on an early match and, under pipefail, read as a failure).
    local syms
    syms="$(nm "$FAT_LIB" 2>/dev/null)" || true
    [[ "$syms" == *" T hc_tool_main"* ]] || { echo "build-fat-lib: ERROR hc_tool_main not in the fat lib" >&2; return 1; }
    [[ "$syms" == *"cJSON_Parse"* ]]     || { echo "build-fat-lib: ERROR cJSON not baked into the fat lib" >&2; return 1; }
    echo "build-fat-lib: OK -> $FAT_LIB"
    return 0
}
