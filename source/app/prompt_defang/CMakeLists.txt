# prompt_defang — pure prompt-injection defangs shared by the host memory fence (host_bridge) and the agentd
# worker's skills injection (W6 P6.2). Zero deps (std <string> only), so both the host and the separate
# agentd process link it — the plan's "factor the pure fn so agentd links it".

add_library(hc_prompt_defang STATIC src/prompt_defang.cpp)
add_library(hc::prompt_defang ALIAS hc_prompt_defang)

target_include_directories(hc_prompt_defang PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_prompt_defang PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_prompt_defang tests/test_prompt_defang.cpp)
    target_link_libraries(test_prompt_defang PRIVATE hc_prompt_defang hc_warnings hc_sanitizers)
    add_test(NAME prompt_defang COMMAND test_prompt_defang)
endif()
