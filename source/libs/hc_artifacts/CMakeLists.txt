# hc_artifacts — content-addressed object store + provenance log. Depends on hc_json + the shared hc_fs
# (POSIX filesystem) + hc_hash (the shared vendored SHA-256). No new third-party dependency.

set(HC_ARTIFACTS_SRC src/hc_artifacts.c)

add_library(hc_artifacts STATIC ${HC_ARTIFACTS_SRC})
add_library(hc::artifacts ALIAS hc_artifacts)

target_include_directories(hc_artifacts PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_artifacts PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_artifacts PRIVATE hc_json hc_fs hc_hash hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_artifacts tests/test_hc_artifacts.c)
    target_link_libraries(test_hc_artifacts PRIVATE hc_artifacts hc_warnings hc_sanitizers)
    add_test(NAME hc_artifacts COMMAND test_hc_artifacts)
endif()
