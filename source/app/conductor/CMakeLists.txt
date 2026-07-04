# conductor — the host-side Conductor component: the conversational front-door agent that also drives the
# fleet (Docs/Plan_Conductor). C++ host module, built behind one static lib (hc::conductor). P3a landed the
# GoalStore (durable goals on hc_wal); P3b adds the threaded agent component + conversation session + resume.
# hc::agent is PUBLIC (hc_conductor.hpp exposes hc_agent_backend); hc::wal/json/store + Threads are PRIVATE
# (the public headers expose none of them). Offline-testable: GoalStore + the conductor loop run on a stub
# backend with no LLM / network.

find_package(Threads REQUIRED)

add_library(hc_conductor STATIC
  src/goal_store.cpp
  src/conductor.cpp
  src/conductor_tools.cpp
)
add_library(hc::conductor ALIAS hc_conductor)

target_include_directories(hc_conductor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_conductor
  PUBLIC  hc::agent
  PRIVATE hc::wal hc::json hc::store Threads::Threads hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # P3a — GoalStore: durable goals on hc_wal (round-trip, multi-transition fold, resume, traversal, corruption).
  add_executable(test_goal_store tests/test_goal_store.cpp)
  target_link_libraries(test_goal_store PRIVATE hc_conductor hc_warnings hc_sanitizers)
  add_test(NAME goal_store COMMAND test_goal_store)

  # P3b — the Conductor component: say/snapshot/stop, streamed reply, resume, budget (offline stub backend).
  # TSan-built too (the conductor thread vs the snapshot reader is the concurrency under test).
  add_executable(test_conductor tests/test_conductor.cpp)
  target_link_libraries(test_conductor
    PRIVATE hc_conductor hc::agent hc::store Threads::Threads hc_warnings hc_sanitizers)
  add_test(NAME conductor COMMAND test_conductor)

  # P4 — the control-plane tools: dispatch routing + ask_user blocking (a tool-emitting stub + a fake ControlPlane).
  add_executable(test_conductor_tools tests/test_conductor_tools.cpp)
  target_link_libraries(test_conductor_tools
    PRIVATE hc_conductor hc::agent Threads::Threads hc_warnings hc_sanitizers)
  add_test(NAME conductor_tools COMMAND test_conductor_tools)
endif()
