# hc_wal — per-stream append-only write-ahead log (P14 crash-recoverable agendas). A GENERIC line log
# (open/append/replay/list/remove); the orchestrator owns the record schema + the fold. Depends only on the
# shared hc_fs (crash-safe append + capped read). Never hc_json / hc_llm (domain-agnostic, reusable).

add_library(hc_wal STATIC src/hc_wal.c)
add_library(hc::wal ALIAS hc_wal)
target_include_directories(hc_wal PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_wal PRIVATE hc_fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_wal tests/test_hc_wal.c)
    target_link_libraries(test_hc_wal PRIVATE hc_wal hc_warnings hc_sanitizers)
    add_test(NAME hc_wal COMMAND test_hc_wal)
    set_tests_properties(hc_wal PROPERTIES TIMEOUT 30)
endif()
