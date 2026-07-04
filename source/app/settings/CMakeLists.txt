# settings — the operator's persisted application settings (WI-2 E1). A C++ host leaf: a Settings value
# struct + parse/serialize/validate/merge_env/load/save, depending on hc::json + hc::fs ONLY (never hc_ui,
# hc_policy, host_storage or the orchestrator — it is a leaf the host maps to/from those layers). The API
# key is deliberately NOT a field here; it lives in hc_secrets.

add_library(hc_settings STATIC src/hc_settings.cpp)
add_library(hc::settings ALIAS hc_settings)
target_include_directories(hc_settings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_settings PRIVATE hc::json hc::fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # the pure offline gate: round-trip (incl. the sparse panels map + egress list), malformed/missing ->
  # defaults, validate clamps + the task_deadline >= llm_call_total invariant, and env-precedence.
  add_executable(test_settings tests/test_settings.cpp)
  target_link_libraries(test_settings PRIVATE hc::settings hc_warnings hc_sanitizers)
  add_test(NAME settings COMMAND test_settings)
  set_tests_properties(settings PROPERTIES TIMEOUT 30)
endif()
