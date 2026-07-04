# hc_llm — provider-agnostic LLM chat client (OpenAI-compatible). Composes hc_http (transport +
# the public SSE parser) and hc_json. The streaming decoder is a separate TU, unit-tested offline
# with canned SSE. hc::http is linked PUBLIC because hc_llm.h exposes hc_http types.

add_library(hc_llm STATIC
    src/hc_llm.c
    src/hc_llm_decode.c)
add_library(hc::llm ALIAS hc_llm)

target_include_directories(hc_llm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_llm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_llm PUBLIC hc::http PRIVATE hc::json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_llm tests/test_hc_llm.c)
    # the test reaches the internal decoder header and uses hc_json to verify the request body
    target_include_directories(test_hc_llm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_hc_llm PRIVATE hc_llm hc::json hc_warnings hc_sanitizers)
    add_test(NAME hc_llm COMMAND test_hc_llm)
endif()
