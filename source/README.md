# HyperCat, from source

This directory is my complete application source: the C core libraries, the C++ host, the worker
and helper processes, the tests, and the packaging that produces the release bundles. If you are
here to read me, build me, or take me apart to see how I tick, welcome in (=・ω・=)

If you just want to use HyperCat, the packaged bundle on the
[releases page](https://github.com/savannah-i-g/HyperCat-Agent/releases) is the supported way to
install, and the [front-page README](../README.md) covers it.

## Layout

| Path | What it is |
|---|---|
| `libs/` | The C core: one static library per concern (JSON, HTTP, LLM client, session store, sandbox, secrets, transport, agent runtime, audio, image, and friends), each behind a narrow `extern "C"` header with documented ownership. |
| `app/` | The C++ host: the Dear ImGui shell, the message bus, the orchestrator, the conductor, policy, settings, and the worker supervisor. |
| `agentd/` | The worker-agent process. The host spawns one per agent, each in its own address space. |
| `audio_helper/` | The sandboxed audio decoder process. |
| `tool_launch/` | The launcher that jails managed-runtime (Python) tools before the interpreter starts. |
| `tests/` | Top-level integration gates. Each module also carries its own unit tests next to its code. |
| `third_party/` | Vendored single-header decoders (stb, dr_libs), compiled directly from this tree. Everything else is a pinned fetch or a system package. |
| `docker/` | The container build matrix used to verify releases across distribution families. |
| `packaging/` | The installer and the notices that ship in the release bundle. |
| `tools/` | Development-time utilities: asset conversion, packaging, and build scripts. |

## Building

Linux on x86-64 is the supported platform for this release. On the Debian/Ubuntu family:

```bash
sudo apt install build-essential cmake ninja-build pkg-config git ca-certificates \
     libcurl4-openssl-dev libcjson-dev libglfw3-dev libopenal-dev libsecret-1-dev \
     libgl1-mesa-dev libx11-dev
```

The `docker/` directory holds the equivalent environments for Fedora, Arch, and openSUSE.
Then, from this `source/` directory:

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure   # the unit and integration gates
```

The first configure fetches an exact-pinned Dear ImGui (and OpenAL Soft only if no system copy
exists), so it needs the network once. GLFW, libcurl, and libcjson come from system packages,
and building just the core libraries stays offline.

## Running your build

```bash
export OPENROUTER_API_KEY="your-key-here"
export HC_MODEL="anthropic/claude-opus-4.1"   # any model id your provider offers
./build/app/hypercat
```

The host resolves its sibling processes (`agentd`, the audio helper, the tool launcher) next to
its own binary, falling back to the build-tree paths baked in at compile time, so a build-tree
run works as-is. `tools/package.sh` produces the flat, relocatable release bundle.

## Contributing and style

Formatting is enforced by `.clang-format`, static analysis by `.clang-tidy`. The tree is strict
about modularity: one module, one responsibility, C at the seams with a stable ABI, C++ in the
host, and dependencies forming a DAG. If you extend me, please keep to that shape.

Tools of your own do not need this tree at all: the
[HyperCat Tool SDK](https://github.com/savannah-i-g/HyperCat-Tool-SDK) (Apache-2.0) builds
against a single header and runs your tool in its own confined process.

## License

Copyright (c) 2026 Savannah Goring. Licensed under the Apache License, Version 2.0; see the
repository [LICENSE](../LICENSE) and [`packaging/THIRD_PARTY.txt`](packaging/THIRD_PARTY.txt)
for the third-party components. The HyperCat name and mascot artwork are brand assets, and the
license does not grant trademark rights.
