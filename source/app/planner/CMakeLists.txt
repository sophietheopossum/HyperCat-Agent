# planner (P05) — turn a goal into a VALIDATED task DAG, installed via Orchestrator::set_decomposer; the
# autonomy layer that decides WHAT the work is (the orchestrator decides HOW). C++ host above the
# orchestrator facade: PUBLIC hc::orchestrator (the orch::Task model), PRIVATE hc::llm (the decompose
# call) + hc::json (parse). The pure parser/validator is the offline gate. P05b adds hc_replan — the
# bounded verify->replan loop over the facade (no new dep; it borrows the same facade + a Decomposer).

add_library(hc_planner STATIC src/hc_plan_validate.cpp src/hc_planner.cpp src/hc_replan.cpp)
add_library(hc::planner ALIAS hc_planner)
target_include_directories(hc_planner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_planner
  PUBLIC hc::orchestrator
  PRIVATE hc::llm hc::json hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # gate: the hostile-plan battery on validate_plan + the strict plan-JSON parser (pure, offline).
  add_executable(test_plan_validate tests/test_plan_validate.cpp)
  target_link_libraries(test_plan_validate PRIVATE hc::planner hc_warnings hc_sanitizers)
  add_test(NAME plan_validate COMMAND test_plan_validate)
  set_tests_properties(plan_validate PROPERTIES TIMEOUT 30)
endif()
