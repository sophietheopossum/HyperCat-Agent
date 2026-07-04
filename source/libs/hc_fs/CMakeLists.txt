# hc_fs — shared POSIX filesystem primitives (the consolidation of the crash-safe helpers that hc_store,
# hc_artifacts, and hc_memory had each mirrored). No dependencies — pure POSIX.

set(HC_FS_SRC src/hc_fs_posix.c)

add_library(hc_fs STATIC ${HC_FS_SRC})
add_library(hc::fs ALIAS hc_fs)

target_include_directories(hc_fs PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_fs PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_fs tests/test_hc_fs.c)
    target_link_libraries(test_hc_fs PRIVATE hc_fs hc_warnings hc_sanitizers)
    add_test(NAME hc_fs COMMAND test_hc_fs)
endif()
