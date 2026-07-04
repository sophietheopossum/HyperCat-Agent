# fleet — the host's worker-fleet manager: the roster + spawn-arg building, lifted out of main()'s
# provision_fleet so worker lifecycle is one module. Borrows the supervisor (process lifecycle); the
# role table + settings types appear in the public header, so those are PUBLIC link deps.

add_library(hc_fleet STATIC src/hc_fleet.cpp)
add_library(hc::fleet ALIAS hc_fleet)

target_include_directories(hc_fleet PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_include_directories(hc_fleet PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..) # app/ for ws_util.hpp
target_link_libraries(hc_fleet
  PUBLIC hc::roles hc::settings
  PRIVATE hc::supervisor hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
  add_executable(test_fleet tests/test_fleet.cpp)
  target_link_libraries(test_fleet PRIVATE hc::fleet hc_warnings hc_sanitizers)
  add_test(NAME fleet COMMAND test_fleet)
endif()
