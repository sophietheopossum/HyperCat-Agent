# reasoning — the optional staged deep_reason tool (5-stage prompt chain over hc::llm). C++ host
# module. hc::llm is PRIVATE (the Reasoner BORROWS an hc_llm*; the public header forward-declares
# the opaque handle, leaking nothing). The offline test stands up a loopback fake-OpenAI socket
# server, so test_reasoning links hc::http (hc_http_global_init/new) and Threads directly — the
# library under test needs neither (its network goes through the borrowed hc_llm -> hc_http).

add_library(hc_reasoning STATIC src/hc_reasoning.cpp)
add_library(hc::reasoning ALIAS hc_reasoning)

target_include_directories(hc_reasoning PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_reasoning PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_reasoning PRIVATE hc::llm hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  find_package(Threads REQUIRED)
  add_executable(test_reasoning tests/test_reasoning.cpp)
  target_link_libraries(test_reasoning
    PRIVATE hc::reasoning hc::llm hc::http Threads::Threads hc_warnings hc_sanitizers)
  add_test(NAME reasoning COMMAND test_reasoning)
  set_tests_properties(reasoning PROPERTIES TIMEOUT 30)
endif()
