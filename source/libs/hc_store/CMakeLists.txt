# hc_store — append-only JSONL session store. Depends on hc_json + the shared hc_fs (POSIX filesystem
# primitives, Linux + macOS — Docs/Plan_HyperCat/13-platform-portability.md).

set(HC_STORE_SRC src/hc_store.c)

add_library(hc_store STATIC ${HC_STORE_SRC})
add_library(hc::store ALIAS hc_store)

target_include_directories(hc_store PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_store PRIVATE hc_json hc_fs hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_store tests/test_hc_store.c)
    target_link_libraries(test_hc_store PRIVATE hc_store hc_warnings hc_sanitizers)
    add_test(NAME hc_store COMMAND test_hc_store)
endif()
