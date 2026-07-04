# orchestrator — host-side domain layer that drives agendas/tasks over the bus + supervisor. C++
# host module, built up in sub-modules behind one static lib:
#   model (pure domain + state machine) <- engine (scheduler, emits intents, no I/O)
#     <- codec (internal JSON) <- the driver facade (the only bus/supervisor toucher).
# hc::bus / hc::supervisor / hc::json are PRIVATE (pimpl hides them). Link deps grow as the
# sub-modules that need them land. The end-to-end gate lives in tests/ (it spawns agentd).

find_package(Threads REQUIRED)

add_library(hc_orchestrator STATIC
  src/hc_orch_model.cpp
  src/hc_orch_engine.cpp
  src/hc_verify.cpp
  src/hc_orch_codec.cpp
  src/hc_orch_wal.cpp
  src/hc_orchestrator.cpp
)
add_library(hc::orchestrator ALIAS hc_orchestrator)

target_include_directories(hc_orchestrator PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_orchestrator PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hc_orchestrator
  PRIVATE hc::bus hc::supervisor hc::json hc::wal hc::util Threads::Threads hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  add_executable(test_orch_model tests/test_orch_model.cpp)
  target_link_libraries(test_orch_model PRIVATE hc_orchestrator hc_warnings hc_sanitizers)
  add_test(NAME orch_model COMMAND test_orch_model)

  add_executable(test_orch_engine tests/test_orch_engine.cpp)
  target_link_libraries(test_orch_engine PRIVATE hc_orchestrator hc_warnings hc_sanitizers)
  add_test(NAME orch_engine COMMAND test_orch_engine)

  # the codec is internal (src/); the test reaches it via a PRIVATE src include, as hc_http does
  add_executable(test_orch_codec tests/test_orch_codec.cpp)
  target_include_directories(test_orch_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_orch_codec PRIVATE hc_orchestrator hc::json hc_warnings hc_sanitizers)
  add_test(NAME orch_codec COMMAND test_orch_codec)

  # P04 verify: the pure tally + the engine wiring (hc_verify.hpp is internal -> PRIVATE src include)
  add_executable(test_verify tests/test_verify.cpp)
  target_include_directories(test_verify PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_verify PRIVATE hc_orchestrator hc_warnings hc_sanitizers)
  add_test(NAME verify COMMAND test_verify)

  # P14 WAL: the record schema + the pure recovery fold (hc_orch_wal.hpp is internal -> PRIVATE src include)
  add_executable(test_orch_wal tests/test_orch_wal.cpp)
  target_include_directories(test_orch_wal PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
  target_link_libraries(test_orch_wal PRIVATE hc_orchestrator hc::json hc_warnings hc_sanitizers)
  add_test(NAME orch_wal COMMAND test_orch_wal)
endif()
