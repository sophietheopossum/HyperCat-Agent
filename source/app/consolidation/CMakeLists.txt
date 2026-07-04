# hc_consolidation — the host-side memory consolidation passes (P6/P03): COMPACTION (summarize older
# turns into an episode summary appended BESIDE the raw transcript — never destroying it) + DISTILLATION
# (reflect over a session and emit provenance-tagged durable facts through a caller-supplied sink). Both
# are one-shot hc_agent turns via the P5 backend seam, so they run live OR offline. A leaf host module:
# depends on hc::agent + hc::store + hc::fs + hc::json; never hc_memory / the bus / UI (the write SINK,
# supplied by the host, owns the memory-write + human-trust policy).

add_library(hc_consolidation STATIC src/consolidation.cpp)
add_library(hc::consolidation ALIAS hc_consolidation)
target_include_directories(hc_consolidation PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_consolidation
  PUBLIC hc::agent
  PRIVATE hc::store hc::fs hc::json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # gate: a scripted backend drives compaction (episode beside raw) + distillation (facts to a capture
  # sink, budget cap holds, NONE skipped) + a below-threshold no-op. Offline, deterministic.
  add_executable(test_consolidation tests/test_consolidation.cpp)
  target_link_libraries(test_consolidation
    PRIVATE hc::consolidation hc::store hc::fs hc_warnings hc_sanitizers)
  add_test(NAME consolidation COMMAND test_consolidation)
  set_tests_properties(consolidation PROPERTIES TIMEOUT 30)
endif()
