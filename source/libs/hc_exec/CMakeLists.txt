# hc_exec — the bounded, KERNEL-CONFINED (Landlock + seccomp) child-process spawner behind the host's brokered
# `run` tool (Worker Revamp W4). One responsibility: run one allowlisted binary under a fail-closed jail and
# capture its bounded output. Linux-only confinement; a non-Linux build refuses exec (HC_EXEC_ERR_UNSUPPORTED).
# No dependencies beyond libc + the Linux uapi headers (linux/landlock.h, linux/seccomp.h, linux/filter.h).

add_library(hc_exec STATIC src/hc_exec.c)
add_library(hc::exec ALIAS hc_exec)
target_include_directories(hc_exec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_exec PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # the confinement gate: echo runs + captures; a non-zero exit is reported; a sleeper is killed at the
  # timeout; output is truncated at the cap; the child env carries NO OPENROUTER_API_KEY; a write OUTSIDE the
  # workspace is refused by Landlock while a write INSIDE succeeds; bad args are rejected.
  add_executable(test_hc_exec tests/test_hc_exec.c)
  target_link_libraries(test_hc_exec PRIVATE hc::exec hc_warnings hc_sanitizers)
  add_test(NAME hc_exec COMMAND test_hc_exec)
  set_tests_properties(hc_exec PROPERTIES TIMEOUT 60)
endif()
