# policy — egress allowlist (anti-SSRF) + exec allowlist (C++ host module). Provides a C-callable
# guard that plugs into hc_http_set_guard. Pure logic, fully offline-testable. Links hc::http
# PUBLIC because the public header references hc_http_peer.

add_library(hc_policy STATIC src/hc_policy.cpp)
add_library(hc::policy ALIAS hc_policy)

target_include_directories(hc_policy PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_policy PUBLIC hc::http PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_policy tests/test_hc_policy.cpp)
    target_link_libraries(test_hc_policy PRIVATE hc_policy hc_warnings hc_sanitizers)
    add_test(NAME hc_policy COMMAND test_hc_policy)
endif()
