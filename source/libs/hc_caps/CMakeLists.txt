# hc_caps — unforgeable, scoped, expiring object-capability tokens (P09). A pure leaf: depends only on
# hc_hash (HMAC-SHA-256) + libc. The host's CapabilityAuthority (app/host_bridge) owns the stateful half.

add_library(hc_caps STATIC src/hc_caps.c)
add_library(hc::caps ALIAS hc_caps)

target_include_directories(hc_caps PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_caps PRIVATE hc::hash hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_caps tests/test_hc_caps.c)
    target_link_libraries(test_hc_caps PRIVATE hc::caps hc_warnings hc_sanitizers)
    add_test(NAME hc_caps COMMAND test_hc_caps)
endif()
