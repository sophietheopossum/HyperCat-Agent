# Building and installing a HyperCat tool

This is the mechanical companion to AUTHORING.md (which covers *what* to write). Here we cover
*building* your tool against the SDK and *installing* it so HyperCat will run it.

## Requirements

- **Linux only.** The SDK links HyperCat's sandbox (`hc_confine` = Landlock + seccomp); there is no
  macOS/Windows build in v1. A tool that cannot confine itself refuses to run (fail-closed).
- **glibc 2.35 or newer.** The prebuilt `lib/libhc_tool_sdk.a` is built at that floor, so it links on any
  newer distro.
- **C / C++:** a C compiler (`cc`/`gcc`/`clang`) and `pthread`. **Rust (native):** a `cargo` toolchain (the crate
  links the same static lib). **Python (managed):** just a system `python3` on the host — nothing to build.

## Build

The library is self-contained — cJSON is baked in, and there is no HyperCat runtime dependency. You link
one file plus `pthread`:

```sh
cc my_tool.c -I include -L lib -lhc_tool_sdk -lpthread -o my_tool
```

Or with CMake:

```cmake
add_executable(my_tool my_tool.c)
target_include_directories(my_tool PRIVATE ${SDK}/include)
target_link_libraries(my_tool PRIVATE ${SDK}/lib/libhc_tool_sdk.a Threads::Threads)
```

`my_tool.c` is just the example shape: include `hc_tool.h`, declare your function(s), and `return
hc_tool_main(argc, argv, defs, n)`. That one call is the whole `main()`. (See `examples/hello_tool/`.)

## Build — Python (managed runtime)

No compile step. Your package is the entry script plus the helper module (there is no `bin/<id>` — the host runs
the *system* `python3` on your script under the managed jail):

```
<id>/
├── manifest.json      # "runtime": {"mode":"managed","interpreter":"python3","entry":"tool.py"}
├── tool.py            # your script: defines the function(s), calls hypercat_tool.serve({...})
└── hypercat_tool.py   # the helper, copied verbatim from the SDK's python/ (pure stdlib; just vendor it)
```

`tool.py` does `import hypercat_tool` — which resolves because the package dir is on Python's path. (See
`examples/wordcount_tool_py/`.) The interpreter name must be allowlisted (`python3`/`python`/`node`); the host
resolves it to a real system binary — your package can never supply its own interpreter.

## Build — Rust (native runtime)

A Rust tool is a compiled native binary that links the SDK's static lib for the confine FFI. Add the
`hypercat-tool` crate (from the SDK's `rust/`) as a dependency, point it at the static lib, and build:

```sh
# from your crate; HC_TOOL_SDK_LIB_DIR holds libhc_tool_sdk.a (the SDK bundle's lib/)
HC_TOOL_SDK_LIB_DIR=/path/to/sdk/lib cargo build --release
cp target/release/<your-bin> <id>/bin/<id>
```

The crate's `build.rs` emits the link directives; the fat static lib bundles `hc_confine`, so the only other
link need is libc. The example builds with
`HC_TOOL_SDK_LIB_DIR=... cargo build --release --example reverse` (see `rust/hypercat-tool/examples/reverse.rs`).
Requires a Rust toolchain (`cargo`).

## Install

HyperCat discovers tools under a per-user directory; each tool is its own package. A **native** (C/C++/Rust)
package ships a compiled binary; a **managed** (Python) package ships its script(s) instead:

```
~/.local/share/hypercat/tools/<id>/        ~/.local/share/hypercat/tools/<id>/
├── manifest.json   # "id" MUST equal <id>  ├── manifest.json   # runtime: managed
└── bin/<id>        # native: the binary    ├── tool.py         # managed: the entry script
                                            └── hypercat_tool.py # + any modules it imports
```

- The `tools/` directory **must be host-private** (`chmod 700`, owner-only). HyperCat refuses to load from a
  world-readable install root.
- Then open HyperCat → the **Tools** panel → **Third-party** → your tool. Review its declared permissions and
  the disclaimer, tick **enabled** (type-to-confirm the first time — this records a hash of your whole package,
  `manifest.lock`: the manifest plus every file in the package tree, so a managed tool's scripts are pinned just
  like a native binary), and it launches confined. If any pinned byte ever changes, the launch is refused until
  you re-approve.

By default workers (the fleet) get your tool; the front-door conductor gets it only if the operator turns on
"conductor may use third-party tools" in the Tools panel (off by default).

## Versioning / compatibility

The SDK version tracks the wire protocol, `HC_TOOL_ABI_VERSION` (currently `1`). A tool built against SDK 1.0
keeps working with any HyperCat host that speaks tool-protocol v1. If the protocol ever makes a breaking
change, the ABI version bumps and the host rejects a mismatched tool rather than mis-dispatching it. See
`docs/PROTOCOL.md` for the contract itself.
