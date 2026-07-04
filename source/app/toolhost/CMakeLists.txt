# toolhost — supervises confined third-party tool subprocesses + routes their bus invokes (Custom Tooling,
# Wave D). tool_manifest is the PURE install-time validator (hc::json + hc::tooling); hc_toolhost is the
# launcher + async router (it owns a BusClient, borrows the Broker, spawns tool binaries via posix_spawn +
# token-fd, and relays tool.invoke <-> tool.result). hypercat links hc::toolhost once it is wired into the host.

add_library(hc_toolhost STATIC src/tool_manifest.cpp src/hc_toolhost.cpp)
add_library(hc::toolhost ALIAS hc_toolhost)

target_include_directories(hc_toolhost PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# hc::tooling is in the public header (the parsed ToolManifest) -> PUBLIC; bus/json/fs/hash are impl-only -> PRIVATE
# (hc::hash backs the manifest.lock supply-chain pin in tool_lock_hex).
target_link_libraries(hc_toolhost PUBLIC hc::tooling
                      PRIVATE hc::bus hc::json hc::fs hc::hash hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_tool_manifest tests/test_tool_manifest.cpp)
    target_link_libraries(test_tool_manifest PRIVATE hc::toolhost hc::json hc::tooling hc_warnings hc_sanitizers)
    add_test(NAME tool_manifest COMMAND test_tool_manifest)

    # the end-to-end integration gate: launches the SDK's hello_tool binary through the ToolHost + invokes it.
    add_executable(test_toolhost tests/test_toolhost.cpp)
    target_link_libraries(test_toolhost PRIVATE hc::toolhost hc::bus hc::tooling hc::fs hc::json hc_warnings
                          hc_sanitizers)
    add_dependencies(test_toolhost hc_tool_hello hc_tool_sleeper hc_tool_launch)
    target_compile_definitions(test_toolhost PRIVATE HELLO_TOOL_PATH="$<TARGET_FILE:hc_tool_hello>"
                               SLEEPER_TOOL_PATH="$<TARGET_FILE:hc_tool_sleeper>"
                               # the non-C path: the managed-runtime launcher + the Python helper source, so the
                               # gate can stand up a real managed (Python) tool end-to-end (launch -> jail -> bus).
                               HC_TOOL_LAUNCH_PATH="$<TARGET_FILE:hc_tool_launch>"
                               HYPERCAT_TOOL_PY="${CMAKE_SOURCE_DIR}/libs/hc_tool_sdk/python/hypercat_tool.py")
    add_test(NAME toolhost COMMAND test_toolhost)
endif()
