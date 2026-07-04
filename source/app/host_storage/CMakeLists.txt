# host_storage — resolve the host's persistent data dir (HC_DATA_DIR > XDG_DATA_HOME > ~/.local/share)
# + the single-host advisory lock, with an ephemeral fallback. A leaf host module: depends only on
# hc::fs (the 0700 mkdir) + libc. Keeps main.cpp thin and makes the resolution/lock unit-testable.

add_library(hc_host_storage STATIC src/host_storage.cpp)
add_library(hc::host_storage ALIAS hc_host_storage)
target_include_directories(hc_host_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_host_storage PRIVATE hc::fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # gate: data-dir resolution precedence + 0700 create + the flock acquire/contend/release + the
  # open_host_storage ephemeral/persistent/fallback decision. Offline, deterministic.
  add_executable(test_host_storage tests/test_host_storage.cpp)
  target_link_libraries(test_host_storage PRIVATE hc::host_storage hc_warnings hc_sanitizers)
  add_test(NAME host_storage COMMAND test_host_storage)
  set_tests_properties(host_storage PROPERTIES TIMEOUT 30)
endif()
