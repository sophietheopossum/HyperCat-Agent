# hc_projects — the on-disk project index (Wave 3: per-project isolation foundation). An append-only
# index.jsonl (last-wins replay + orphan-adoption) + traversal-safe ids + an atomic active pointer. Depends on
# hc_fs (crash-safe append/atomic write + capped read + dir listing) + hc_json (the index record schema).

add_library(hc_projects STATIC src/hc_projects.c)
add_library(hc::projects ALIAS hc_projects)
target_include_directories(hc_projects PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_projects PRIVATE hc_json hc_fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_projects tests/test_hc_projects.c)
    target_link_libraries(test_hc_projects PRIVATE hc_projects hc_warnings hc_sanitizers)
    add_test(NAME hc_projects COMMAND test_hc_projects)
    set_tests_properties(hc_projects PROPERTIES TIMEOUT 30)
endif()
