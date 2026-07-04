# hc_replay — deterministic OFFLINE replay of a recorded agent session (P13a/P03). Re-drives a recorded
# session through the SAME hc_agent loop via the backend seam (recorded outputs fed back) with MOCK tools
# (recorded results, no real side effects), and diffs the re-derived manifest against the recording with
# the hc_manifest oracle. Depends on hc::agent (seam + loop) + hc::manifest (read + diff) + hc::json;
# never the bus or UI. Ships a thin `hc-replay` CLI.

add_library(hc_replay STATIC src/replay.cpp)
add_library(hc::replay ALIAS hc_replay)
target_include_directories(hc_replay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_replay
  PUBLIC hc::agent hc::manifest
  PRIVATE hc::json hc_warnings hc_sanitizers)

# the CLI: replay a session dir + print the per-turn manifest diff (exit 0 = clean match)
add_executable(hc-replay hc_replay_main.cpp)
target_link_libraries(hc-replay PRIVATE hc::replay hc::fs hc::json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # gate: record a known offline session, replay it, assert the re-derived manifest matches exactly.
  add_executable(test_replay tests/test_replay.cpp)
  target_link_libraries(test_replay
    PRIVATE hc::replay hc::agent hc::manifest hc_warnings hc_sanitizers)
  add_test(NAME replay COMMAND test_replay)
  set_tests_properties(replay PROPERTIES TIMEOUT 30)
endif()
