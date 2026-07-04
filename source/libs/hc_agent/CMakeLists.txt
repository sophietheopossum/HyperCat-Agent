# hc_agent — single-agent turn loop. Drives hc_llm, dispatches tool calls, keeps history.
# Links hc::llm PUBLIC (hc_agent.h exposes hc_llm types) and hc::json PRIVATE (json confined to
# the .c). The offline test runs a loopback fake-OpenAI server, so it needs Threads.

add_library(hc_agent STATIC src/hc_agent.c)
add_library(hc::agent ALIAS hc_agent)

target_include_directories(hc_agent PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_agent PUBLIC hc::llm PRIVATE hc::json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(test_hc_agent tests/test_hc_agent.c)
    target_link_libraries(test_hc_agent PRIVATE hc_agent hc::http hc_warnings hc_sanitizers Threads::Threads)
    add_test(NAME hc_agent COMMAND test_hc_agent)

    # P2 compaction hook: offline (a stub backend — no hc_llm, no loopback server, no threads).
    # Links only hc_agent; hc::llm/hc::http arrive transitively (hc_agent's own link line).
    add_executable(test_hc_agent_compaction tests/test_hc_agent_compaction.c)
    target_link_libraries(test_hc_agent_compaction PRIVATE hc_agent hc_warnings hc_sanitizers)
    add_test(NAME hc_agent_compaction COMMAND test_hc_agent_compaction)
endif()
