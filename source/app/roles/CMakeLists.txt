# roles — the declarative worker role table (Worker Revamp, Wave 2): capability -> {prompt overlay, toolset,
# model}. A C++ host leaf depending on hc::json + hc::fs ONLY (never hc_ui / the orchestrator / agentd /
# settings) — the host resolves it + threads each worker its own slice at spawn.

add_library(hc_roles STATIC src/hc_roles.cpp)
add_library(hc::roles ALIAS hc_roles)
target_include_directories(hc_roles PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_roles PRIVATE hc::json hc::fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # the pure offline gate: built-in defaults, serialize<->parse round-trip, validate (drop empty role + dedup
  # tools), the tool name <-> enum mapping, and malformed/missing -> graceful.
  add_executable(test_roles tests/test_roles.cpp)
  target_link_libraries(test_roles PRIVATE hc::roles hc_warnings hc_sanitizers)
  add_test(NAME roles COMMAND test_roles)
  set_tests_properties(roles PROPERTIES TIMEOUT 30)
endif()
