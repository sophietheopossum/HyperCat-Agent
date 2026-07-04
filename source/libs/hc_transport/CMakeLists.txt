# hc_transport — framed Unix-domain-socket byte transport (Linux + macOS). libc only; the UDS
# echo test uses a listener thread, so it links Threads.

add_library(hc_transport STATIC src/hc_transport.c)
add_library(hc::transport ALIAS hc_transport)

target_include_directories(hc_transport PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_transport PRIVATE hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    find_package(Threads REQUIRED)
    add_executable(test_hc_transport tests/test_hc_transport.c)
    target_link_libraries(test_hc_transport PRIVATE hc_transport hc_warnings hc_sanitizers Threads::Threads)
    add_test(NAME hc_transport COMMAND test_hc_transport)
endif()
