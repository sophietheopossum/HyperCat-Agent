# tests — cross-module integration + security tests (per-module unit tests live beside each
# module). Added by the top-level CMakeLists when this dir carries a CMakeLists.txt.

if(NOT HC_BUILD_TESTS)
  return()
endif()

find_package(Threads REQUIRED)

# Step-4 IPC gate: the supervisor spawns REAL agentd processes; verify two workers exchange
# messages, a killed worker does not take down the broker or its peer (crash-isolation), and the
# supervisor reaps + respawns. AGENTD_PATH is the just-built worker binary it launches.
add_executable(test_supervisor_gate test_supervisor_gate.cpp)
target_link_libraries(test_supervisor_gate
  PRIVATE hc::supervisor hc::bus hc::json Threads::Threads hc_warnings hc_sanitizers)
add_dependencies(test_supervisor_gate agentd)
target_compile_definitions(test_supervisor_gate PRIVATE AGENTD_PATH="$<TARGET_FILE:agentd>")
add_test(NAME supervisor_gate COMMAND test_supervisor_gate)
set_tests_properties(supervisor_gate PROPERTIES TIMEOUT 60)

# fleet gate (W2 P2.3) — add-by-role + slot assignment + remove (reap) over real worker processes.
add_executable(test_fleet_gate test_fleet_gate.cpp)
target_link_libraries(test_fleet_gate
  PRIVATE hc::fleet hc::supervisor hc::bus hc::roles hc::settings Threads::Threads hc_warnings hc_sanitizers)
add_dependencies(test_fleet_gate agentd)
target_compile_definitions(test_fleet_gate PRIVATE AGENTD_PATH="$<TARGET_FILE:agentd>")
add_test(NAME fleet_gate COMMAND test_fleet_gate)
set_tests_properties(fleet_gate PROPERTIES TIMEOUT 60)

# Step-5 orchestrator gate: an overseer decomposes an agenda and drives real agentd workers to
# completion over the bus, recovering a mid-task worker crash by reassignment.
add_executable(test_orchestrator_gate test_orchestrator_gate.cpp)
target_link_libraries(test_orchestrator_gate
  PRIVATE hc::orchestrator hc::planner hc::supervisor hc::bus hc::json Threads::Threads hc_warnings hc_sanitizers)
add_dependencies(test_orchestrator_gate agentd)
target_compile_definitions(test_orchestrator_gate PRIVATE AGENTD_PATH="$<TARGET_FILE:agentd>")
add_test(NAME orchestrator_gate COMMAND test_orchestrator_gate)
set_tests_properties(orchestrator_gate PROPERTIES TIMEOUT 60)

# P14 crash-recovery gate: a real agenda journals + its WAL is cleaned up on settle; then a crash mid-
# agenda (the exact fsync'd WAL records) is recovered + resumed over real workers — Done results survive
# (no re-run), in-flight tasks re-dispatch. Needs a PRIVATE src include for the internal record schema.
add_executable(test_wal_recovery test_wal_recovery.cpp)
target_include_directories(test_wal_recovery PRIVATE ${CMAKE_SOURCE_DIR}/app/orchestrator/src)
target_link_libraries(test_wal_recovery
  PRIVATE hc::orchestrator hc::wal hc::supervisor hc::bus hc::json Threads::Threads hc_warnings hc_sanitizers)
add_dependencies(test_wal_recovery agentd)
target_compile_definitions(test_wal_recovery PRIVATE AGENTD_PATH="$<TARGET_FILE:agentd>")
add_test(NAME wal_recovery COMMAND test_wal_recovery)
set_tests_properties(wal_recovery PROPERTIES TIMEOUT 60)

# live_validate: a real streamed turn against a hosted provider (OpenRouter). Built but NOT a CTest
# (it needs network + an API key). Run manually: OPENROUTER_API_KEY=... ./tests/live_validate [model]
add_executable(live_validate live_validate.c)
target_link_libraries(live_validate PRIVATE hc::agent hc::llm hc::http hc::json hc_warnings hc_sanitizers)

# live_validate_conductor: the Conductor (P5) against a live model — scripted say()/snapshot() over the real
# LLM-backed agent with instrumented ControlPlane stubs. Also network + key, NOT a CTest. Run manually:
# OPENROUTER_API_KEY=... ./tests/live_validate_conductor [model]
add_executable(live_validate_conductor live_validate_conductor.cpp)
target_link_libraries(live_validate_conductor
  PRIVATE hc::conductor hc::agent hc::llm hc::http hc::json Threads::Threads hc_warnings hc_sanitizers)
