# HyperCat C++ host app modules (bus, orchestrator, policy, reasoning, ui, ...). Mirrors the
# libs/ pattern: each module is added only once it carries a CMakeLists.txt, so the host grows
# step by step and an incomplete module is skipped rather than breaking configure.

file(GLOB _hc_app_dirs RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/*)
list(SORT _hc_app_dirs)
foreach(_m ${_hc_app_dirs})
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${_m}/CMakeLists.txt)
    add_subdirectory(${_m})
    message(STATUS "HyperCat: + app ${_m}")
  endif()
endforeach()

# The host shell executable: ties the modules together (broker + supervisor + orchestrator + ui +
# host_bridge — the host-side bus adapters: the token stream + the tool-authorization gate). Thin
# wiring + the render loop; spawns agentd at runtime (AGENTD_PATH = the just-built worker).
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/main.cpp)
  find_package(Threads REQUIRED)
  # host_notify needs GIO for GDBus (desktop notifications + their action replies). glib2 is already a
  # runtime dependency via libsecret, so this adds no new package -- only a link. FIND-OR-SKIP, like the
  # keychain: without it the notifier compiles to nothing and approvals behave exactly as before.
  pkg_check_modules(GIO gio-2.0)
  add_executable(hypercat main.cpp host_services.cpp host_conductor.cpp project_session.cpp text_diff.cpp
                 skill_store.cpp exe_path.cpp host_notify.cpp)
  target_link_libraries(hypercat
    PRIVATE hc::ui hc::orchestrator hc::supervisor hc::bus hc::json hc::fs hc::host_bridge hc::sandbox
            hc::store hc::artifacts hc::memory hc::pty hc::consolidation hc::host_storage hc::planner
            hc::conductor hc::reasoning hc::sysstat hc::settings hc::roles hc::secrets hc::policy hc::fleet
            hc::projects hc::prompt_defang hc::conductor_prompt hc::audio hc::toolhost Threads::Threads
            hc_warnings hc_sanitizers)
  if(GIO_FOUND)
    target_compile_definitions(hypercat PRIVATE HC_WITH_NOTIFY)
    target_include_directories(hypercat SYSTEM PRIVATE ${GIO_INCLUDE_DIRS})
    target_link_directories(hypercat PRIVATE ${GIO_LIBRARY_DIRS})
    target_link_libraries(hypercat PRIVATE ${GIO_LIBRARIES})
    message(STATUS "HyperCat: desktop notifications ON (gio ${GIO_VERSION})")
  else()
    message(STATUS "HyperCat: gio-2.0 not found -- approval notifications disabled")
  endif()
  add_dependencies(hypercat agentd hc_audio_helper hc_tool_launch)
  target_compile_definitions(hypercat PRIVATE AGENTD_PATH="$<TARGET_FILE:agentd>")
  # W-Audio.1: the confined audio probe helper — host_services spawns it per library file so the untrusted audio
  # decoders run in a separate hc_confine'd address space, never in the host. Path injected at configure (like
  # AGENTD_PATH); the include dir carries the host<->helper wire contract (audio_helper_wire.h).
  target_compile_definitions(hypercat PRIVATE AUDIO_PROBE_HELPER_PATH="$<TARGET_FILE:hc_audio_helper>")
  # The managed-runtime tool launcher (the non-C SDK path): for a tool whose manifest declares runtime:managed,
  # the ToolHost spawns this to jail then exec the interpreter. Path injected at configure (like AGENTD_PATH);
  # resolved relative to the host binary at runtime via exe_path so a relocated bundle finds it.
  target_compile_definitions(hypercat PRIVATE HC_TOOL_LAUNCH_PATH="$<TARGET_FILE:hc_tool_launch>")
  target_include_directories(hypercat PRIVATE ${CMAKE_SOURCE_DIR}/audio_helper/include)
  message(STATUS "HyperCat: + host shell (hypercat)")

  if(HC_BUILD_TESTS)
    # the P11 line diff (pure) — unit-tested directly against adversarial inputs
    add_executable(test_text_diff tests/test_text_diff.cpp text_diff.cpp)
    target_include_directories(test_text_diff PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_text_diff PRIVATE hc::ui hc_warnings hc_sanitizers)
    add_test(NAME text_diff COMMAND test_text_diff)

    # W6 P6.1: the Skills catalog — the pure frontmatter parser + the JAILED hc_sandbox scan (tmp-dir fixture)
    add_executable(test_skill_store tests/test_skill_store.cpp skill_store.cpp)
    target_include_directories(test_skill_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                               ${CMAKE_CURRENT_SOURCE_DIR}/prompt_defang/include)
    target_link_libraries(test_skill_store PRIVATE hc::sandbox hc::prompt_defang hc_warnings hc_sanitizers)
    add_test(NAME skill_store COMMAND test_skill_store)

    # Packaging: the pure sibling-helper resolver (relocatable-bundle path resolution)
    add_executable(test_exe_path tests/test_exe_path.cpp exe_path.cpp)
    target_include_directories(test_exe_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_exe_path PRIVATE hc_warnings hc_sanitizers)
    add_test(NAME exe_path COMMAND test_exe_path)
  endif()
endif()
