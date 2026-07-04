# hc_audio_helper — the confined audio decode/probe helper (W-Audio). A short-lived child the HOST spawns per file
# so the vendored audio decoders parse UNTRUSTED bytes in a SEPARATE, hc_confine'd address space (no fs/net/exec),
# never in the host process. Links hc::audio (only decode/probe is referenced — runtime.o/OpenAL is not pulled into
# the binary) + hc::confine (the kernel floor). A sibling of agentd/: another helper executable the host launches.

add_executable(hc_audio_helper main.c)
target_include_directories(hc_audio_helper PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_audio_helper PRIVATE hc::audio hc::confine hc_warnings hc_sanitizers)
message(STATUS "HyperCat: + audio helper (hc_audio_helper)")

if(HC_BUILD_TESTS)
  # The helper contract test: spawn the real hc_audio_helper with a valid WAV on stdin (asserts correct metadata +
  # the confinement posture the helper self-applied) and with garbage (asserts a graceful ok=0 reply + clean exit —
  # a malformed file must not crash the helper / the caller). Mirrors the host's spawn path. Skips the confinement
  # asserts on a kernel without Landlock+seccomp (the posture honestly reports the floor's absence).
  add_executable(test_audio_helper tests/test_audio_helper.c)
  target_include_directories(test_audio_helper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)
  target_link_libraries(test_audio_helper PRIVATE hc::audio hc_warnings hc_sanitizers)
  target_compile_definitions(test_audio_helper PRIVATE AUDIO_HELPER_PATH="$<TARGET_FILE:hc_audio_helper>")
  add_dependencies(test_audio_helper hc_audio_helper)
  add_test(NAME audio_helper COMMAND test_audio_helper)
  set_tests_properties(audio_helper PROPERTIES TIMEOUT 30)
endif()
