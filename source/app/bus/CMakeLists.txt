# bus — message broker + client (req/reply, pub/sub, backpressure) over hc_transport. C++ host
# module. The broker is thread-per-connection, so it links Threads. hc::transport / hc::json are
# PRIVATE (pimpl hides them from the public header).

find_package(Threads REQUIRED)

add_library(hc_bus STATIC src/hc_bus.cpp)
add_library(hc::bus ALIAS hc_bus)

target_include_directories(hc_bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_bus PRIVATE hc::transport hc::json Threads::Threads hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_bus tests/test_hc_bus.cpp)
    target_link_libraries(test_hc_bus PRIVATE hc_bus Threads::Threads hc_warnings hc_sanitizers)
    add_test(NAME hc_bus COMMAND test_hc_bus)
endif()
