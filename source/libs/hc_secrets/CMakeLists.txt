# hc_secrets — process-local secret store with non-elidable zeroization. No external deps in v1
# (the OS-keychain backend is a dependency-gated seam, deferred). libc only.

add_library(hc_secrets STATIC src/hc_secrets.c)
add_library(hc::secrets ALIAS hc_secrets)

target_include_directories(hc_secrets PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hc_secrets PRIVATE hc_warnings hc_sanitizers)

# OS keychain backend (Linux: libsecret / the freedesktop Secret Service). FIND-OR-SKIP — the graceful pattern
# (NOT REQUIRED, like hc_json's cJSON): build the real arms only when the option is ON, we are NOT on Apple (the
# macOS Security.framework arm is a documented seam with no code yet), AND libsecret-1 is actually present. Else
# the in-memory-only build, and the persistence verbs compile as UNSUPPORTED stubs. The libsecret/glib headers
# go in SYSTEM so they stay out of -Wall -Wextra -Wpedantic. PkgConfig is loaded at the top level.
if(HC_WITH_KEYCHAIN AND NOT APPLE)
    pkg_check_modules(LIBSECRET libsecret-1) # no REQUIRED -> a missing dep degrades the build, never breaks it
    if(LIBSECRET_FOUND)
        target_compile_definitions(hc_secrets PRIVATE HC_WITH_KEYCHAIN_BACKEND)
        target_include_directories(hc_secrets SYSTEM PRIVATE ${LIBSECRET_INCLUDE_DIRS})
        target_link_directories(hc_secrets PRIVATE ${LIBSECRET_LIBRARY_DIRS})
        target_link_libraries(hc_secrets PRIVATE ${LIBSECRET_LIBRARIES})
        message(STATUS "hc_secrets: keychain backend ON (libsecret ${LIBSECRET_VERSION})")
    else()
        message(STATUS "hc_secrets: libsecret-1 not found -- in-memory only (keychain verbs return UNSUPPORTED)")
    endif()
elseif(APPLE)
    # SEAM (no code): macOS Keychain Services. When a macOS build lands, add a src/hc_secrets_mac.mm arm here
    # behind HC_WITH_KEYCHAIN_BACKEND + `-framework Security`; until then macOS gets the in-memory build.
    message(STATUS "hc_secrets: macOS keychain seam not implemented yet -- in-memory only")
endif()

if(HC_BUILD_TESTS)
    add_executable(test_hc_secrets tests/test_hc_secrets.c)
    target_link_libraries(test_hc_secrets PRIVATE hc_secrets hc_warnings hc_sanitizers)
    add_test(NAME hc_secrets COMMAND test_hc_secrets)

    add_executable(test_hc_secrets_keychain tests/test_hc_secrets_keychain.c)
    target_link_libraries(test_hc_secrets_keychain PRIVATE hc_secrets hc_warnings hc_sanitizers)
    add_test(NAME hc_secrets_keychain COMMAND test_hc_secrets_keychain)
    set_tests_properties(hc_secrets_keychain PROPERTIES TIMEOUT 30)
endif()
