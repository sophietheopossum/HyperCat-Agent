# hc_pty — POSIX pseudo-terminal child spawn (the operator terminal). Pure POSIX (posix_openpt + fork +
# execve), NO third-party dependency. See hc_pty.h for the security posture (the scrubbed child env that
# keeps the API key out of the spawned shell).
add_library(hc_pty STATIC src/hc_pty.c)
add_library(hc::pty ALIAS hc_pty)
target_include_directories(hc_pty PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_pty PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  add_executable(test_hc_pty tests/test_hc_pty.c)
  target_link_libraries(test_hc_pty PRIVATE hc_pty hc_warnings hc_sanitizers)
  add_test(NAME hc_pty COMMAND test_hc_pty)
  set_tests_properties(hc_pty PROPERTIES TIMEOUT 30)
endif()
