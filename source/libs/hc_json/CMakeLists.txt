# hc_json — JSON wrapper over cJSON. The ONLY module that links cJSON; everything else depends on
# hc_json (Docs/Plan_HyperCat/02 + 14). cJSON is a PRIVATE link dependency so its headers never leak.
#
# Acquisition is FIND-OR-FETCH (cross-distro): the system library where it is packaged (every big-4
# Linux family ships libcjson.pc) — byte-identical to before — else an exact-pinned FetchContent
# fallback, mirroring hc_audio's openal-soft pattern (in-policy per Docs/Plan_HyperCat/12; not a new
# dependency, just a second way to acquire the same one). src/hc_json.c keeps `#include <cjson/cJSON.h>`
# in BOTH paths: the fetched upstream tree puts cJSON.h at its ROOT, so a configure-time shim header
# (below) re-exposes it under the cjson/ prefix — a build-file bridge, never a source edit.

# HC_JSON_FORCE_FETCH: skip the system probe and ALWAYS build the pinned cJSON statically. Off by default
# (normal builds prefer the packaged system lib). The Tool SDK release build sets it ON so the redistributable
# libhc_tool_sdk.a bakes cJSON in (self-contained) regardless of whether the build host packages libcjson.
option(HC_JSON_FORCE_FETCH "Always static-fetch the pinned cJSON (skip system libcjson) — for the self-contained Tool SDK" OFF)
if(NOT HC_JSON_FORCE_FETCH)
    pkg_check_modules(CJSON IMPORTED_TARGET libcjson) # system cJSON via pkg-config (the big-4 path; no REQUIRED)
endif()
if(CJSON_FOUND)
    set(HC_JSON_CJSON_LIB PkgConfig::CJSON)
    set(HC_JSON_CJSON_FETCHED FALSE)
else()
    # Fallback: exact-pinned upstream cJSON, static + minimal (no tests/utils, no -Werror flags forced
    # onto our tree). Not exercised where a system libcjson exists; the safety net for a distro that
    # doesn't package it. Needs network at configure time (as the ImGui/openal-soft fetches already do).
    message(STATUS "hc_json: system libcjson not found — fetching pinned cJSON")
    include(FetchContent)
    FetchContent_Declare(cjson
        GIT_REPOSITORY https://github.com/DaveGamble/cJSON.git
        GIT_TAG        v1.7.18 # exact-pinned (reproducible); bump deliberately
        GIT_SHALLOW    TRUE)
    set(ENABLE_CJSON_TEST OFF CACHE BOOL "" FORCE)            # don't build cJSON's own tests
    set(ENABLE_CJSON_UTILS OFF CACHE BOOL "" FORCE)           # we use only the core
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)            # static, matching our static-lib core
    set(ENABLE_CUSTOM_COMPILER_FLAGS OFF CACHE BOOL "" FORCE) # don't let cJSON force its own warning flags
    set(ENABLE_TARGET_EXPORT OFF CACHE BOOL "" FORCE)         # no install/export under FetchContent
    FetchContent_MakeAvailable(cjson)
    set(HC_JSON_CJSON_LIB cjson) # upstream's target name
    set(HC_JSON_CJSON_FETCHED TRUE)
endif()

add_library(hc_json STATIC src/hc_json.c)
add_library(hc::json ALIAS hc_json)

target_include_directories(hc_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_json PRIVATE ${HC_JSON_CJSON_LIB} hc_warnings hc_sanitizers)

# Include-dir bridge for the FETCHED case only (the system case already provides <prefix>/include/cjson/).
# FetchContent populates ${cjson_SOURCE_DIR} (a "<name>-src" dir) with cJSON.h at its ROOT, exposed to
# consumers as <cJSON.h> — but our source includes <cjson/cJSON.h>. Copy the pinned header into a
# build-tree shim dir under a cjson/ prefix and add that as a SYSTEM include, so the existing include
# line resolves unchanged. SYSTEM keeps cJSON's header out of our -Wall -Wextra -Wpedantic.
if(HC_JSON_CJSON_FETCHED)
    file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/cjson_shim/cjson)
    configure_file(${cjson_SOURCE_DIR}/cJSON.h
                   ${CMAKE_CURRENT_BINARY_DIR}/cjson_shim/cjson/cJSON.h COPYONLY)
    target_include_directories(hc_json SYSTEM PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/cjson_shim)
endif()

if(HC_BUILD_TESTS)
    add_executable(test_hc_json tests/test_hc_json.c)
    target_link_libraries(test_hc_json PRIVATE hc_json hc_warnings hc_sanitizers)
    add_test(NAME hc_json COMMAND test_hc_json)
endif()
