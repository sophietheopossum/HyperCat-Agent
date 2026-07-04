# agentd — agent worker process (one agent per OS process; doc 02). Links the C++ bus client + the
# C JSON codec for messaging, the agent stack (hc_agent -> hc_llm -> hc_http) so a task.assign runs a
# REAL hc_agent turn when a model + key are configured (else a deterministic offline echo), hc_sandbox
# for the human-gated fs_write tool (jailed to the worker's --workspace, when set), hc_reasoning for the
# deep_reason tool (the staged 5-stage chain over the worker's hc_llm), and hc_store to persist each
# task's transcript as a session (--sessions) for the host's session browser. NB hc_reasoning is an
# app/ module linked into this WORKER executable: deep_reason runs inside the worker (on its own hc_llm),
# not on the host bus — both hypercat + agentd are consumers, so this is not a libs/ layering cycle.
# hc::bus carries the thread-using broker in the same archive as BusClient, so it links Threads.
# agentd uses only the BusClient half; the Broker is dead code in a worker's address space — the
# accepted v1 trade-off (both share the Message wire type). A hc::bus_client split target is a
# future option if a worker's image size ever matters.

find_package(Threads REQUIRED)

# hc::policy (an app/ leaf, like hc::reasoning above) supplies the anti-SSRF egress guard the worker
# installs on its HTTP client (WI-2 E0). Pure logic over hc_http types — agentd -> hc::policy -> libs is
# acyclic (hc::policy never depends on agentd): the same established host-module-into-worker pattern.
add_executable(agentd main.cpp worker.cpp worker_protocol.cpp worker_tools.cpp worker_fs.cpp worker_role.cpp
                      worker_runtime_tools.cpp)
target_link_libraries(agentd
  # hc::caps — the worker HOLDS + PRESENTS capability tokens (P09.3). It only uses the verb/scope enum VALUES
  # and treats the token as opaque (it has no signing key; only the host verifies) — no mint/verify here.
  PRIVATE hc::bus hc::agent hc::llm hc::http hc::json hc::sandbox hc::reasoning hc::store hc::manifest
          hc::policy hc::util hc::prompt_defang hc::caps hc::confine Threads::Threads hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  # worker_fs (the sandbox-backed fs_read/fs_list/fs_update core) is testable in isolation: it links only
  # hc::sandbox, so the file-op + confinement behaviour is verified without the bus/llm/agent stack.
  add_executable(test_worker_fs tests/test_worker_fs.cpp worker_fs.cpp)
  target_link_libraries(test_worker_fs PRIVATE hc::sandbox hc_warnings hc_sanitizers)
  target_include_directories(test_worker_fs PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME worker_fs COMMAND test_worker_fs)

  # the W2.6 role TOOLSET subset policy (parse_role_tools: csv -> RoleToolset). It is a PURE leaf (worker_role.cpp
  # depends only on <string>), so the test links that TU alone — no bus/llm/agent stack — pinning the subset
  # decision register_agent_tools consumes.
  add_executable(test_role_tools tests/test_role_tools.cpp worker_role.cpp)
  target_link_libraries(test_role_tools PRIVATE hc_warnings hc_sanitizers)
  target_include_directories(test_role_tools PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME role_tools COMMAND test_role_tools)
endif()
