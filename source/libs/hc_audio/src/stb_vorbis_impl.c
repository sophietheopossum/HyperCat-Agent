/* stb_vorbis_impl — the ONE translation unit that compiles the vendored stb_vorbis.c implementation (Ogg Vorbis
 * decode for hc_audio). Isolated here, like hc_image_stb.c, so its ~5k lines compile in exactly one place;
 * decode.c includes the same header with STB_VORBIS_HEADER_ONLY for the declarations. The hardening defines
 * (STB_VORBIS_NO_STDIO, STB_VORBIS_NO_PUSHDATA_API, the channel cap) are set as target compile definitions in
 * CMakeLists so both TUs agree — see libs/hc_audio/CMakeLists.txt. */
#include "stb_vorbis.c"
