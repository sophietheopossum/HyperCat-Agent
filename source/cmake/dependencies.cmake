# Dependency acquisition policy (Docs/Plan_HyperCat/12-build-and-deps.md).
#
# HyperCat acquires each dependency *where it is used*, keeping modules self-contained and
# foundation builds offline-capable against system packages:
#
#   hc_json  -> system libcjson  (pkg-config)   — the only module that touches cJSON
#   hc_http  -> system libcurl   (pkg-config / find_package CURL)
#   app/ui   -> GLFW + Dear ImGui (pinned FetchContent, only when the UI target is built)
#
# Pins for the fetched UI deps live next to the ui target. No network fetch happens at the
# top level, so building just the core libraries never reaches out.

find_package(PkgConfig REQUIRED)
