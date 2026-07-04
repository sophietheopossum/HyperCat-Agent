# hc_confine — drop the calling process to a kernel-enforced least-privilege floor (P07): Linux seccomp-bpf +
# Landlock, applied in-place by an agent worker after it acquires its resources, before its turn loop. A DAG
# leaf: depends on no hc_* lib, only the kernel (raw syscalls, no libseccomp — the no-new-dep posture).

add_library(hc_confine STATIC src/hc_confine.c)
add_library(hc::confine ALIAS hc_confine)

target_include_directories(hc_confine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_confine PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    # the under-confinement smoke test (the gate): apply the floor in a child, then prove the worker's ops still
    # work AND the forbidden is denied/killed. Skips cleanly on a kernel without Landlock+seccomp.
    add_executable(test_hc_confine tests/test_hc_confine.c)
    target_link_libraries(test_hc_confine PRIVATE hc::confine hc_warnings hc_sanitizers)
    add_test(NAME hc_confine COMMAND test_hc_confine)
    set_tests_properties(hc_confine PROPERTIES TIMEOUT 30)
endif()
