# hc_tool_launch — the managed-runtime tool launcher (the non-C SDK path). A tiny host binary the ToolHost spawns
# to jail then exec a non-C tool's interpreter (see main.c). Links ONLY hc::confine (the kernel jail) — no bus, no
# app code; like agentd/audio_helper it is a runtime sibling resolved next to the host binary (exe_path).
add_executable(hc_tool_launch main.c)
target_link_libraries(hc_tool_launch PRIVATE hc::confine hc_warnings hc_sanitizers)
message(STATUS "HyperCat: + tool launcher (hc_tool_launch)")
