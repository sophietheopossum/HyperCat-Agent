# hc_audio — HyperCat's audio primitive (Music Player + future notifications). Mirrors hc_image: the vendored
# single-header decoders (dr_wav/dr_mp3/dr_flac + stb_vorbis) compile here, ISOLATED behind a SYSTEM include so
# their warning-noise is silenced under -Wall/-Wextra/-Wpedantic while OUR wrapper code stays fully checked.
# Sanitizers stay ON — we WANT ASan/UBSan over the untrusted-byte parsers. decode/probe need no device; the
# OpenAL streaming player (runtime.c) links OpenAL (output only) + pthreads.

find_package(OpenAL)   # system OpenAL (openal-soft); provides OpenAL::OpenAL or OPENAL_LIBRARY/INCLUDE_DIR
if(NOT OpenAL_FOUND AND NOT OPENAL_FOUND)
    # portability fallback (not exercised where system OpenAL exists): exact-pinned openal-soft
    include(FetchContent)
    FetchContent_Declare(openal_soft
        GIT_REPOSITORY https://github.com/kcat/openal-soft.git
        GIT_TAG        d3875f333fb6abe2f39d82caca329414871ae53b) # exact-pinned == tag 1.23.1
    set(ALSOFT_UTILS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(ALSOFT_TESTS OFF CACHE BOOL "" FORCE)
    set(ALSOFT_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(openal_soft)
endif()
find_package(Threads REQUIRED)

add_library(hc_audio STATIC
  src/decode.c
  src/stb_vorbis_impl.c
  src/spectrum.c
  src/runtime.c)
add_library(hc::audio ALIAS hc_audio)

target_include_directories(hc_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# the vendored decoders live in third_party/; SYSTEM silences their (third-party) warnings, our code stays checked
target_include_directories(hc_audio SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
# stb_vorbis: memory (pulldata) API only, no stdio — set for BOTH the impl TU and decode.c's HEADER_ONLY include
target_compile_definitions(hc_audio PRIVATE STB_VORBIS_NO_STDIO STB_VORBIS_NO_PUSHDATA_API)
# the decoders use libm (pow/exp/etc.)
target_link_libraries(hc_audio PRIVATE m hc_warnings hc_sanitizers Threads::Threads)
if(TARGET OpenAL::OpenAL)
    target_link_libraries(hc_audio PRIVATE OpenAL::OpenAL)
else()
    target_include_directories(hc_audio SYSTEM PRIVATE ${OPENAL_INCLUDE_DIR})
    target_link_libraries(hc_audio PRIVATE ${OPENAL_LIBRARY})
endif()

if(HC_BUILD_TESTS)
    add_executable(test_hc_audio tests/test_hc_audio.c)
    target_link_libraries(test_hc_audio PRIVATE hc_audio hc_warnings hc_sanitizers)
    add_test(NAME hc_audio COMMAND test_hc_audio)
    set_tests_properties(hc_audio PROPERTIES TIMEOUT 30)

    # the engine smoke test forces the OpenAL "null" backend (ALSOFT_DRIVERS=null) — exercises the streaming
    # pipeline + thread under ASan/UBSan with NO audible output and tolerant of headless CI.
    add_executable(test_hc_audio_player tests/test_hc_audio_player.c)
    target_link_libraries(test_hc_audio_player PRIVATE hc_audio hc_warnings hc_sanitizers)
    add_test(NAME hc_audio_player COMMAND test_hc_audio_player)
    set_tests_properties(hc_audio_player PROPERTIES TIMEOUT 30 ENVIRONMENT "ALSOFT_DRIVERS=null")
endif()
