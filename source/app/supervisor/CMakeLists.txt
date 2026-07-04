# supervisor — host-side worker lifecycle manager (spawn agentd, authenticate token check-ins,
# monitor + restart). C++ host module. hc::bus / hc::json are PRIVATE (pimpl hides them from the
# public header). Threads for the monitor + check-in threads. The internal hc_spawn TU stays under
# src/ (not a public surface). The step-4 crash-isolation GATE is an integration test in tests/
# (it needs the agentd executable), so there is no in-module test here.

find_package(Threads REQUIRED)

add_library(hc_supervisor STATIC src/hc_supervisor.cpp src/hc_spawn.cpp)
add_library(hc::supervisor ALIAS hc_supervisor)

target_include_directories(hc_supervisor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_supervisor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_supervisor PRIVATE hc::bus hc::json Threads::Threads hc_warnings hc_sanitizers)
