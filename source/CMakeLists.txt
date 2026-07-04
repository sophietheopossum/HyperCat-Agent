# HyperCat — top-level build.
#
# Modular by construction: each libs/hc_* is its own STATIC target, added only if it
# carries a CMakeLists.txt, so the build grows step by step (see Docs/Plan_HyperCat/15-roadmap.md)
# and an accidental dependency cycle fails to configure (Docs/Plan_HyperCat/14-engineering-policies.md).

cmake_minimum_required(VERSION 3.20)
project(HyperCat VERSION 0.1.2 LANGUAGES C CXX)

# ---- Language standards ----
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# ---- Options ----
option(HC_WITH_LLAMA "Build the optional embedded llama.cpp inference backend" OFF)
option(HC_WITH_KEYCHAIN "Use the OS keychain (libsecret) as the persistent secret store when present" ON)
option(HC_BUILD_TESTS "Build unit and integration tests" ON)
option(HC_SANITIZERS "Enable ASan/UBSan in Debug builds" ON)
# Test-only gate bypasses (e.g. HC_AUTO_APPROVE, the headless live-validation auto-approver). OFF by
# default so a RELEASE/shipped binary CANNOT contain the operator-gate bypass — it compiles out entirely.
# Enable only on a dev/CI build that needs headless end-to-end validation. NEVER set this for a release.
option(HC_ENABLE_TEST_GATES "Compile in test-only gate bypasses (NEVER in a release build)" OFF)
# HC_SDK_ONLY: configure ONLY the Tool SDK lib + its dependency closure (hc_transport/hc_json/hc_confine/
# hc_tool_sdk), skipping the app, the other libs, and the top-level PkgConfig requirement. Used by the
# redistributable SDK build (docker/common/sdk-package.sh) so the SDK builder image needs only a C toolchain —
# no curl/glfw/openal/pkg-config. Pair with HC_JSON_FORCE_FETCH=ON so cJSON is static (no system libcjson probe).
option(HC_SDK_ONLY "Configure only the Tool SDK lib subtree (for the redistributable SDK build)" OFF)

# ---- Shared compile-flag interface targets ----
add_library(hc_warnings INTERFACE)
target_compile_options(hc_warnings INTERFACE -Wall -Wextra -Wpedantic)

add_library(hc_sanitizers INTERFACE)
if(HC_SANITIZERS)
  target_compile_options(hc_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined;-fno-omit-frame-pointer>)
  target_link_options(hc_sanitizers INTERFACE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
endif()

# ---- Pinned third-party dependencies ----
# Skipped in the SDK-only configure: the top-level PkgConfig isn't needed there (the SDK's only external dep is
# cJSON, and HC_JSON_FORCE_FETCH builds it from source — no pkg-config probe).
if(NOT HC_SDK_ONLY)
  include(cmake/dependencies.cmake)
endif()

# ---- Tests ----
if(HC_BUILD_TESTS)
  enable_testing()
endif()

if(HC_SDK_ONLY)
  # The redistributable Tool SDK: configure ONLY the SDK lib + its dependency closure. Nothing else (the app,
  # the other libs, their curl/glfw/openal finds) is reached, so the SDK builder needs only a C toolchain.
  foreach(_lib hc_transport hc_json hc_confine hc_tool_sdk)
    add_subdirectory(libs/${_lib})
    message(STATUS "HyperCat SDK: + lib ${_lib}")
  endforeach()
else()
  # ---- C core: add each libs/* that is ready (has a CMakeLists.txt) ----
  file(GLOB _hc_lib_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR}/libs ${CMAKE_CURRENT_SOURCE_DIR}/libs/*)
  list(SORT _hc_lib_dirs)
  foreach(_lib ${_hc_lib_dirs})
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/libs/${_lib}/CMakeLists.txt)
      add_subdirectory(libs/${_lib})
      message(STATUS "HyperCat: + lib ${_lib}")
    endif()
  endforeach()

  # ---- C++ host app, agent worker, the confined audio helper, integration tests, dev-time tools (when present) ----
  # `tools` holds offline dev utilities (e.g. png2qoi for WI-5 asset conversion); it links no app/lib target.
  # `audio_helper` is a runtime sibling of agentd/: the short-lived hc_confine'd audio decode/probe child (W-Audio).
  foreach(_sub app agentd audio_helper tool_launch tests tools)
    if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_sub}/CMakeLists.txt)
      add_subdirectory(${_sub})
    endif()
  endforeach()
endif()
