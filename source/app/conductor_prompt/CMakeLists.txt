# conductor_prompt — assemble + validate the conductor system prompt (immutable spine + mutable voice slot).
# A leaf host module: depends on hc::prompt_defang ONLY (it takes the persona string as a parameter — no
# settings, no I/O, no LLM), so it stays a pure, unit-testable seam the host maps the resolved persona through.

add_library(hc_conductor_prompt STATIC src/conductor_prompt.cpp)
add_library(hc::conductor_prompt ALIAS hc_conductor_prompt)

target_include_directories(hc_conductor_prompt PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_conductor_prompt PRIVATE hc::prompt_defang hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_conductor_prompt tests/test_conductor_prompt.cpp)
    target_link_libraries(test_conductor_prompt
                          PRIVATE hc::conductor_prompt hc::prompt_defang hc_warnings hc_sanitizers)
    add_test(NAME conductor_prompt COMMAND test_conductor_prompt)

    # live_persona_check — a MANUAL network harness (NOT a ctest): drive a real LLM turn under several personas
    # to confirm the voice changes while identity + the floor hold. Run with a key; needs network.
    add_executable(live_persona_check tools/live_persona_check.cpp)
    target_link_libraries(live_persona_check PRIVATE hc::conductor_prompt hc::llm hc::http hc_warnings hc_sanitizers)
endif()
