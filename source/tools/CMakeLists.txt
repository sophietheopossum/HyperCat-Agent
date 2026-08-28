# tools — DEV-TIME utilities, NOT part of the shipped app. Nothing here is linked by hc_image or any
# app/agentd target (verified: these are standalone add_executable()s). They exist so build-time asset
# conversion (PNG -> QOI) keeps the runtime dependency surface at zero new image libraries.
#
# png2qoi vendors tools/third_party/stb_image.h (public domain) for PNG decode ONLY in this tool. stb is
# warning-noisy under -Wall -Wextra -Wpedantic, so this target does NOT inherit hc_warnings; instead it
# sets a LOCAL, scoped relaxation (the lib/app warning policy is untouched; WI-5). The
# vendored header dir is marked SYSTEM so its diagnostics are suppressed at the include boundary too.

add_executable(png2qoi png2qoi.c)
target_include_directories(png2qoi SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
# stb_image's HDR->LDR helper references pow() even under STBI_ONLY_PNG; link libm (tool-only).
target_link_libraries(png2qoi PRIVATE m)

# Scoped, tool-only warning relaxation for the single-header stb_image.h implementation. We keep -Wall
# -Wextra on (so OUR tool code is still checked) and drop only the pedantic/unused/sign noise stb trips.
target_compile_options(png2qoi PRIVATE
  -Wall -Wextra
  -Wno-unused-parameter
  -Wno-unused-but-set-variable
  -Wno-sign-compare
  -Wno-implicit-fallthrough)

# Deliberately NO sanitizers and NO hc_image link: this is an offline, single-shot converter that the
# build invokes (or a developer runs by hand); it shares no code with the shipped decoder.

# qoi_pack — the manifest-driven spritesheet packer (Conductor P5-S0). Same dev-only posture as png2qoi:
# vendors stb_image.h for PNG decode, scoped warning relaxation, no sanitizers, no hc_image/app link. It
# reads app/ui/assets/sprites.manifest + the source PNGs and emits the committed app/ui/assets/sprite_assets.h.
add_executable(qoi_pack qoi_pack.c)
target_include_directories(qoi_pack SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(qoi_pack PRIVATE m)
target_compile_options(qoi_pack PRIVATE
  -Wall -Wextra
  -Wno-unused-parameter
  -Wno-unused-but-set-variable
  -Wno-sign-compare
  -Wno-implicit-fallthrough)

# genlock — compute a third-party tool's manifest.lock headlessly (dev/test "approval"; the UI normally writes it
# on consent-enable). Links the ToolHost (for hcapp::tool_lock_hex) so the lock matches the launch-time verify
# byte-for-byte. Dev-only, never shipped (not referenced by hypercat/agentd/the installer).
if(TARGET hc::toolhost)
  find_package(Threads REQUIRED)
  add_executable(genlock genlock.cpp)
  target_link_libraries(genlock PRIVATE hc::toolhost hc::bus hc::tooling hc::json hc::fs hc::hash
                        Threads::Threads hc_warnings hc_sanitizers)
endif()

# hc_toolbench — per-call tool-schema validity as a chain lengthens, across providers. Dev-time
# only; nothing in app/ or agentd/ links it. Drives hc_agent with deterministic stub tools, so a run
# touches no network beyond the model call. Sanitizers KEPT (unlike png2qoi): this tool's output is
# DATA someone draws conclusions from, and next to LLM latency the overhead is unmeasurable.
add_executable(hc_toolbench hc_toolbench.c)
target_link_libraries(hc_toolbench PRIVATE hc_agent hc::json hc::http hc_warnings hc_sanitizers)
