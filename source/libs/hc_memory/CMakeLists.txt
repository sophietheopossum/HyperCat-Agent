# hc_memory — scoped semantic-memory store (text + embedding vectors + flat exact cosine query).
# Depends on hc_json + the shared hc_fs. The embedding vectors are computed by the host (via hc_llm) and
# handed IN — this library NEVER calls hc_llm, preserving the acyclic downward DAG. libm for sqrt.

set(HC_MEMORY_SRC src/hc_memory.c)

add_library(hc_memory STATIC ${HC_MEMORY_SRC})
add_library(hc::memory ALIAS hc_memory)

target_include_directories(hc_memory PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_memory PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_memory PRIVATE hc_json hc_fs hc_warnings hc_sanitizers)
target_link_libraries(hc_memory PUBLIC m) # sqrt — transitive so consumers (tests, host) get it too

if(HC_BUILD_TESTS)
    add_executable(test_hc_memory tests/test_hc_memory.c)
    target_link_libraries(test_hc_memory PRIVATE hc_memory hc_warnings hc_sanitizers)
    add_test(NAME hc_memory COMMAND test_hc_memory)
endif()
