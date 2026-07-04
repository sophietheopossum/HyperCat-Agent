# hc_http — libcurl-backed HTTP/SSE client + connectivity probe. The ONLY module that links
# libcurl; curl headers stay PRIVATE so consumers never include curl.h (mirrors hc_json/cJSON).
# POSIX sockets back the net probe and the offline loopback test.

find_package(CURL QUIET)
if(CURL_FOUND)
    set(HC_HTTP_CURL CURL::libcurl)
else()
    pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
    set(HC_HTTP_CURL PkgConfig::CURL)
endif()

add_library(hc_http STATIC
    src/hc_http.c
    src/hc_http_sse.c
    src/hc_http_probe.c)
add_library(hc::http ALIAS hc_http)

target_include_directories(hc_http PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_http PRIVATE ${HC_HTTP_CURL} hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(test_hc_http tests/test_hc_http.c)
    # the test reaches the internal SSE parser header via the PRIVATE src dir
    target_include_directories(test_hc_http PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_hc_http PRIVATE hc_http hc_warnings hc_sanitizers Threads::Threads)
    add_test(NAME hc_http COMMAND test_hc_http)
endif()
