# hc_tool_sdk — the third-party Tool SDK (Custom Tooling, Wave D). A C leaf an EXTERNAL author links to build
# a tool BINARY that connects to the bus, checks in, self-confines, and serves invokes. Minimal deps by
# charter: hc_transport + hc_json + hc_confine ONLY (no host/C++ code ever reaches a third-party binary).
# It is NOT linked into the host (the host ToolHost LAUNCHES tool binaries; it does not link the SDK).

add_library(hc_tool_sdk STATIC src/hc_tool.c)
add_library(hc::tool_sdk ALIAS hc_tool_sdk)

target_include_directories(hc_tool_sdk PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_tool_sdk PUBLIC hc::transport hc::json hc::confine
                      PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    # the read-only example tool — also Wave D's end-to-end integration fixture (the ToolHost launches it).
    add_executable(hc_tool_hello examples/hello_tool/main.c)
    target_link_libraries(hc_tool_hello PRIVATE hc::tool_sdk hc_warnings hc_sanitizers)

    # the DELIBERATELY-hung fixture — proves the ToolHost's timeout sweep reaps a non-replying tool (D6 security).
    add_executable(hc_tool_sleeper examples/sleeper_tool/main.c)
    target_link_libraries(hc_tool_sleeper PRIVATE hc::tool_sdk hc_warnings hc_sanitizers)
endif()
