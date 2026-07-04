# hc_image — the in-house QOI image decoder (WI-5). The SHIPPED, security-critical primitive: QOI bytes ->
# RGBA8. QOI was chosen so the app ships NO new image dependency — the PNG decode lives only in the dev-time
# tools/png2qoi converter, never here. Depends on nothing but libc (a self-contained, reusable primitive);
# the app/ui module embeds the .qoi bytes and uploads the decoded buffer as a GL texture.

add_library(hc_image STATIC src/hc_image.c src/hc_image_stb.c)
add_library(hc::image ALIAS hc_image)
target_include_directories(hc_image PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# W4 P4.0b: the PNG/JPEG path (hc_image_stb.c) vendors third_party/stb_image.h. SYSTEM include silences stb's
# warning-noise under -Wall/-Wextra/-Wpedantic while OUR wrapper code in hc_image_stb.c stays checked; stb's
# HDR->LDR helper references pow() even under STBI_ONLY_PNG/JPEG, so link libm. Sanitizers stay ON (we WANT
# ASan/UBSan over the untrusted-byte parser).
target_include_directories(hc_image SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(hc_image PRIVATE m hc_warnings hc_sanitizers)

if(HC_BUILD_TESTS)
    add_executable(test_hc_image tests/test_hc_image.c)
    target_link_libraries(test_hc_image PRIVATE hc_image hc_warnings hc_sanitizers)
    add_test(NAME hc_image COMMAND test_hc_image)
    set_tests_properties(hc_image PROPERTIES TIMEOUT 30)
endif()
