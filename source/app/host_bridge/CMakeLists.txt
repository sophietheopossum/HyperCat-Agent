# host_bridge — the host-side bus adapters: the "tokens" stream -> UiAdapter; the tool.authorize gate
# -> AuthGate; and the semantic-recall service -> MemoryBroker (P01). Backend-side (never hc_ui): it
# exposes its own view structs; main.cpp maps them into the UiSnapshot. The MemoryBroker uses hc_memory
# (the store) + hc_llm/hc_http (its embedding client), but these are FORWARD-DECLARED in the public
# header (held only by pointer), so they are PRIVATE deps confined to the .cpp — no ABI leak.

find_package(Threads REQUIRED)

add_library(hc_host_bridge STATIC src/host_bridge.cpp src/cap_authority.cpp)
add_library(hc::host_bridge ALIAS hc_host_bridge)
target_include_directories(hc_host_bridge PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# PRIVATE: the W4.3 ExecGate uses ws_util.hpp (the single ws-subdir source, in app/) — confined to the .cpp.
target_include_directories(hc_host_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(hc_host_bridge
  # PUBLIC hc::caps — the public header exposes hc_cap_claims (CapRecord/grant) by value, so consumers need its
  # include path; the CapabilityAuthority calls hc_caps_mint/verify/authorizes in the .cpp.
  PUBLIC hc::caps
  PRIVATE hc::memory hc::llm hc::http hc::bus hc::json hc::exec hc::prompt_defang hc::secrets Threads::Threads
          hc_warnings hc_sanitizers)
# The HC_AUTO_APPROVE headless auto-approver compiles in ONLY under this dev/CI opt-in (default OFF) — a
# release build has no operator-gate bypass at all (the field + the env read + the branch are #ifdef'd out).
# PUBLIC (not PRIVATE): the def gates a MEMBER of AuthGate in the public header (host_bridge.hpp), so every TU
# that includes it — notably app/project_session.cpp, which new/deletes an AuthGate — MUST agree on the layout.
# PRIVATE skewed the class size between TUs (712 vs 704 bytes -> a new/delete type mismatch). Release is
# unaffected: the option is OFF by default, so the def is added to no target and the member is absent everywhere.
if(HC_ENABLE_TEST_GATES)
  target_compile_definitions(hc_host_bridge PUBLIC HC_ENABLE_TEST_GATES)
endif()

if(HC_BUILD_TESTS)
  # integration test: a real broker + a fake worker client + the AuthGate (filter, surface, resolve).
  add_executable(test_host_bridge tests/test_host_bridge.cpp)
  target_include_directories(test_host_bridge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..) # ws_util.hpp (W4.3 exec test)
  target_link_libraries(test_host_bridge
    PRIVATE hc::host_bridge hc::bus hc::json Threads::Threads hc_warnings hc_sanitizers)
  add_test(NAME host_bridge COMMAND test_host_bridge)
  set_tests_properties(host_bridge PROPERTIES TIMEOUT 30)
endif()
