# hc_hash — vendored SHA-256 (content-identity: hc_artifacts ids, hc_manifest turn hashes) + HMAC-SHA-256
# (the keyed MAC behind the P09 capability tokens). Self-contained, no dependency.

set(HC_HASH_SRC src/sha256.c src/hmac.c)

add_library(hc_hash STATIC ${HC_HASH_SRC})
add_library(hc::hash ALIAS hc_hash)

target_include_directories(hc_hash PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_hash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src) # the library-internal sha256_core.h
target_link_libraries(hc_hash PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_hash tests/test_hc_hash.c)
    target_link_libraries(test_hc_hash PRIVATE hc_hash hc_warnings hc_sanitizers)
    add_test(NAME hc_hash COMMAND test_hc_hash)
endif()
