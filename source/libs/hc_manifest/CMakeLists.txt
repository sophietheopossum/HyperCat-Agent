# hc_manifest — per-turn append-only audit + replay record (the replay enabler for P03). Depends on the
# shared hc_fs (filesystem) + hc_hash (SHA-256) + hc_json (record build/parse). Never hc_llm.

set(HC_MANIFEST_SRC src/hc_manifest.c)

add_library(hc_manifest STATIC ${HC_MANIFEST_SRC})
add_library(hc::manifest ALIAS hc_manifest)

target_include_directories(hc_manifest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_manifest PUBLIC hc_hash) # in the public header (HC_SHA256_HEX_LEN)
target_link_libraries(hc_manifest PRIVATE hc_fs hc_json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_manifest tests/test_hc_manifest.c)
    target_link_libraries(test_hc_manifest PRIVATE hc_manifest hc_warnings hc_sanitizers)
    add_test(NAME hc_manifest COMMAND test_hc_manifest)
endif()
