# tooling — the reserved built-in tool-name registry (anti-spoof for third-party tool install). A pure leaf
# (std <string> only); the host ToolHost (Wave D) + the install path (Wave E) check names against it.

add_library(hc_tooling STATIC src/system_tool_ids.cpp)
add_library(hc::tooling ALIAS hc_tooling)

target_include_directories(hc_tooling PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_tooling PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_system_tool_ids tests/test_system_tool_ids.cpp)
    target_link_libraries(test_system_tool_ids PRIVATE hc::tooling hc_warnings hc_sanitizers)
    add_test(NAME system_tool_ids COMMAND test_system_tool_ids)
endif()
