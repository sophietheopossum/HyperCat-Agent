# ui — the Dear ImGui (docking) shell + restrained-corporate theme (doc 10). C++ host module.
# GLFW is a system dep (pkg-config); Dear ImGui (docking) is pinned via FetchContent NEXT TO this
# target, so building just the core libraries never reaches the network (doc 12). The renderer is
# isolated behind ui_backend so a later macOS Metal path is a backend-file swap (locked decision 4).
# glad is intentionally NOT used — modern ImGui bundles its own GL loader (one fewer dependency;
# within the approved dep ceiling). Logged in EXPANSIONS.

pkg_check_modules(GLFW3 REQUIRED IMPORTED_TARGET glfw3)
find_package(OpenGL REQUIRED)

include(FetchContent)
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG        v1.92.8-docking # exact-pinned (docking branch)
  GIT_SHALLOW    TRUE)
FetchContent_MakeAvailable(imgui)

# Dear ImGui ships no CMakeLists — compile its core + the GLFW/OpenGL3 backends into a static lib.
# Third-party: it does NOT get our -Wall -Wextra -Wpedantic or sanitizers (built with defaults).
add_library(hc_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(hc_imgui SYSTEM PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(hc_imgui PUBLIC PkgConfig::GLFW3 OpenGL::GL)

# The HyperCat UI module: renderer seam (ui_backend) + theme (ui_theme) + panels + the UiApp loop.
# It renders a UiSnapshot and depends only on ImGui (the host adapts orchestrator/bus state into a
# snapshot) — so the UI stays decoupled and headless-testable.
add_library(hc_ui STATIC
  src/ui_backend.cpp
  src/ui_theme.cpp
  src/ui_copy.cpp
  src/ui_panels_common.cpp
  src/ui_panels_work.cpp
  src/ui_panels_io.cpp
  src/ui_panels_content.cpp
  src/ui_panels_files.cpp
  src/ui_panels_review.cpp
  src/ui_toasts.cpp
  src/ui_panels_settings.cpp
  src/ui_panels_models.cpp
  src/ui_panels_roles.cpp
  src/ui_panels_projects.cpp
  src/ui_panels_skills.cpp
  src/ui_panels_tools.cpp
  src/ui_panels_music.cpp
  src/ui_docview.cpp
  src/ui_texcache.cpp
  src/text_lexer.cpp
  src/ui_editor.cpp
  src/ui_markdown.cpp
  src/ui_graph.cpp
  src/ui_sprite.cpp
  src/ui_sprite_registry.cpp
  src/ui_mascot.cpp
  src/ui_conductor_chat.cpp
  src/ui_app.cpp)
add_library(hc::ui ALIAS hc_ui)
target_include_directories(hc_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_ui PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
# assets/ holds the GENERATED sprite_assets.h (embedded QOI catalog, packed by tools/qoi_pack from
# assets/sprites.manifest) — a data blob, not code. Treat it as a SYSTEM header so its generator-emitted
# banner (which legitimately contains paths like "resources/*.png") does not trip our
# -Wall -Wextra -Wpedantic, exactly as imgui/stb are treated.
target_include_directories(hc_ui SYSTEM PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/assets)

# Convenience dev target: regenerate the committed assets/sprite_assets.h from the manifest + source PNGs.
# The header stays COMMITTED (a fresh checkout builds offline with no codegen run); run this only after editing
# the manifest or the art:  cmake --build build --target regen_sprite_assets
if(TARGET qoi_pack)
  add_custom_target(regen_sprite_assets
    COMMAND qoi_pack ${CMAKE_CURRENT_SOURCE_DIR}/assets/sprites.manifest
                     ${CMAKE_CURRENT_SOURCE_DIR}/assets/sprite_assets.h
                     ${CMAKE_SOURCE_DIR}/Docs/resources
    DEPENDS qoi_pack
    COMMENT "Regenerating app/ui/assets/sprite_assets.h from sprites.manifest")
endif()
# hc::image = the in-house QOI decoder the sprite layer feeds bundled assets through (WI-5, zero new dep).
target_link_libraries(hc_ui PRIVATE hc_imgui hc::image hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  add_executable(test_imgui_link tests/test_imgui_link.cpp)
  target_link_libraries(test_imgui_link PRIVATE hc_imgui)
  add_test(NAME imgui_link COMMAND test_imgui_link)

  # headless smoke: hidden window, render frames, tear down. Skips (passes) if no display/GL.
  # Also unit-tests the pure dag_layout() (P10) — hence the src/ include for ui_panels.hpp.
  add_executable(test_ui_smoke tests/test_ui_smoke.cpp)
  target_include_directories(test_ui_smoke PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ui_smoke PRIVATE hc::ui hc_warnings hc_sanitizers)
  add_test(NAME ui_smoke COMMAND test_ui_smoke)
  set_tests_properties(ui_smoke PROPERTIES TIMEOUT 30)

  # WI-4 markdown: the pure bounded-parser gate (%s-safe, unterminated-fence-degrades, block parsing).
  add_executable(test_ui_markdown tests/test_ui_markdown.cpp)
  target_include_directories(test_ui_markdown PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ui_markdown PRIVATE hc::ui hc_warnings hc_sanitizers)
  add_test(NAME ui_markdown COMMAND test_ui_markdown)
  set_tests_properties(ui_markdown PROPERTIES TIMEOUT 30)

  # WI-5 sprites: the pure offline surface — uv_for grid math, the Animation/SpritePlayer state machine
  # (loop modes, seek, speed), and the mascot state classifier. No GL/ImGui context needed.
  add_executable(test_ui_sprite tests/test_ui_sprite.cpp)
  target_include_directories(test_ui_sprite PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  # hc_imgui brings the imgui.h include path (ImVec2/ImTextureRef the sprite headers reference); hc::ui
  # links it only PRIVATEly, so it is not propagated transitively to this test.
  target_link_libraries(test_ui_sprite PRIVATE hc::ui hc_imgui hc_warnings hc_sanitizers)
  add_test(NAME ui_sprite COMMAND test_ui_sprite)
  set_tests_properties(ui_sprite PROPERTIES TIMEOUT 30)

  # W4 P4.0c: the pure file-type classifier (magic-byte tiebreak; text/markdown/image/pdf/binary). No GL/ImGui.
  add_executable(test_ui_docview tests/test_ui_docview.cpp)
  target_include_directories(test_ui_docview PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ui_docview PRIVATE hc::ui hc_warnings hc_sanitizers)
  add_test(NAME ui_docview COMMAND test_ui_docview)
  set_tests_properties(ui_docview PROPERTIES TIMEOUT 30)

  # W4 P4.2: the OperatorTextureCache's PURE LruBudget core (eviction order, byte+count accounting). No GL —
  # hc_imgui supplies only the imgui.h include path the texcache header references (as for test_ui_sprite).
  add_executable(test_ui_texcache tests/test_ui_texcache.cpp)
  target_include_directories(test_ui_texcache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ui_texcache PRIVATE hc::ui hc_imgui hc_warnings hc_sanitizers)
  add_test(NAME ui_texcache COMMAND test_ui_texcache)
  set_tests_properties(ui_texcache PROPERTIES TIMEOUT 30)

  # W5 P5.1: the PURE source tokenizer (language pick + per-line lexing incl. the multi-line block-comment
  # carry). The test code calls no GL/ImGui; linking hc::ui transitively pulls GLFW/GL (so the loader needs
  # them present) but no GL symbol is reached — the same accepted precedent as test_ui_docview/test_ui_markdown.
  add_executable(test_text_lexer tests/test_text_lexer.cpp)
  target_include_directories(test_text_lexer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_text_lexer PRIVATE hc::ui hc_warnings hc_sanitizers)
  add_test(NAME text_lexer COMMAND test_text_lexer)
  set_tests_properties(text_lexer PROPERTIES TIMEOUT 30)

  # W5 P5.3: the Editor's edit-state logic (begin_edit/dirty/reconcile — the save-roundtrip vs external-change
  # reconciliation). Pure string bookkeeping; hc_imgui only for the imgui.h include path (as for test_ui_sprite).
  add_executable(test_ui_editor tests/test_ui_editor.cpp)
  target_include_directories(test_ui_editor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_ui_editor PRIVATE hc::ui hc_imgui hc_warnings hc_sanitizers)
  add_test(NAME ui_editor COMMAND test_ui_editor)
  set_tests_properties(ui_editor PROPERTIES TIMEOUT 30)

  # ui_preview: a dev screenshot tool (hidden window -> PPM) for sighted UI iteration. Not a test.
  add_executable(ui_preview tools/ui_preview.cpp)
  target_link_libraries(ui_preview PRIVATE hc::ui hc_warnings hc_sanitizers)
endif()
