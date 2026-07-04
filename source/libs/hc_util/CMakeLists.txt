# hc_util — header-only leaf utilities (hc_getenv_int, ...). An INTERFACE target: no compiled TU, no ABI;
# consumers add hc::util for the include dir. Zero dependencies beyond libc.

add_library(hc_util INTERFACE)
add_library(hc::util ALIAS hc_util)
target_include_directories(hc_util INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(HC_BUILD_TESTS)
    add_executable(test_hc_util tests/test_hc_util.c)
    target_link_libraries(test_hc_util PRIVATE hc_util hc_warnings hc_sanitizers)
    add_test(NAME hc_util COMMAND test_hc_util)
    set_tests_properties(hc_util PROPERTIES TIMEOUT 15)
endif()
