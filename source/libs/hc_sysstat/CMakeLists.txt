# hc_sysstat — per-process CPU% / RSS / uptime sampler behind an opaque, platform-neutral handle. The
# OS-free CPU%-delta math lives in src/hc_sysstat.c (unit-tested with no /proc); the per-OS raw reader is
# added below. Linux now (src/hc_sysstat_linux.c); macOS is a deferred one-line drop-in (src/hc_sysstat_mac.c,
# not yet written) — only the matching OS source compiles. No third-party dependency. See hc_sysstat.h.
add_library(hc_sysstat STATIC src/hc_sysstat.c)
if(APPLE)
  target_sources(hc_sysstat PRIVATE src/hc_sysstat_mac.c)
else()
  target_sources(hc_sysstat PRIVATE src/hc_sysstat_linux.c)
endif()
add_library(hc::sysstat ALIAS hc_sysstat)
target_include_directories(hc_sysstat PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_sysstat PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  add_executable(test_hc_sysstat tests/test_hc_sysstat.c)
  target_link_libraries(test_hc_sysstat PRIVATE hc_sysstat hc_warnings hc_sanitizers)
  add_test(NAME hc_sysstat COMMAND test_hc_sysstat)
  set_tests_properties(hc_sysstat PROPERTIES TIMEOUT 30)
endif()
