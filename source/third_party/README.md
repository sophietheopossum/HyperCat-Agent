# third_party — vendored fallbacks

Policy: dependencies are normally **fetched at configure time, pinned to exact tags** in
`cmake/dependencies.cmake`. This directory holds **vendored fallbacks** so that offline or
audited builds do not depend on a network fetch, plus anything that has no upstream package.

## Expected contents

- **glad/** — the OpenGL loader. glad has no fetchable package we rely on; generate it once
  (GL 3.3 core, or the project's chosen profile) and commit the generated `src/gl.c` +
  `include/glad/gl.h` + `include/KHR/khrplatform.h` here. The `ui` module links it.
- **cjson/**, **glfw/**, **imgui/** *(optional)* — if a build must be fully offline, drop the
  pinned source trees here; `cmake/dependencies.cmake` can be pointed at them instead of
  fetching. Keep the version identical to the pin so reproducibility holds.

## Vendored single-header decoders (parsers compiled into the C core, like `stb_image.h`)

- **stb_image.h** — PNG/JPEG decode for `hc_image` (W4 P4.0b). Public domain / MIT (stb).
- **dr_wav.h** (v0.13.10), **dr_mp3.h** (v0.6.35), **dr_flac.h** (v0.12.40) — WAV/MP3/FLAC decode for
  `hc_audio` (Music Player). Public domain (Unlicense) OR MIT-0, dual-licensed (mackron/dr_libs).
  Pinned: `mackron/dr_libs@dbbd08d81fd2b084c5ae931531871d0c5fd83b87`.
- **stb_vorbis.c** — Ogg Vorbis decode for `hc_audio`. Public domain / MIT (stb).
  Pinned: `nothings/stb@31c1ad37456438565541f4919958214b6e762fb4`.
  Each is compiled in exactly ONE hardened wrapper TU under `libs/hc_audio/` (no stdio, bounded pre-decode
  caps, asserts disabled) — the untrusted-bytes posture of `hc_image_stb.c`. The full justification + the
  OpenAL backend row are in `Docs/Plan_HyperCat/12-build-and-deps.md`.

## Rules

- Vendored copies match the pin exactly — no local edits to third-party source (fork drift
  is forbidden by policy; extend additively in our own code instead).
- Record the version + license of anything vendored in the dependency manifest referenced by
  `Docs/Plan_HyperCat/12-build-and-deps.md`.
