# hc_sandbox — per-context POSIX filesystem jail (Linux + macOS). No external deps: realpath
# and openat are libc, and one implementation serves both POSIX targets (no platform split).

add_library(hc_sandbox STATIC src/hc_sandbox.c)
add_library(hc::sandbox ALIAS hc_sandbox)

target_include_directories(hc_sandbox PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_sandbox PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    find_package(Threads REQUIRED)                 # the escape battery includes a live TOCTOU race
    add_executable(test_hc_sandbox tests/test_hc_sandbox.c)
    target_link_libraries(test_hc_sandbox PRIVATE hc_sandbox hc_warnings hc_sanitizers Threads::Threads)
    add_test(NAME hc_sandbox COMMAND test_hc_sandbox)
endif()
