HyperCat Tool SDK 1.0
=====================

Build your own tools for HyperCat. A tool is a small program you write in C (or any
language that can speak the protocol) that HyperCat runs in its own confined process
and calls over a local socket. When an agent decides to use your tool, HyperCat sends
it the call, your code runs, and you return a result.

This SDK is licensed under the Apache License 2.0 (see LICENSE + NOTICE). You own the
tools you build with it. HyperCat itself is licensed separately, under its own
Apache License 2.0.

WHAT'S IN HERE
  include/hc_tool.h          The public API — one header, one entry point.
  lib/libhc_tool_sdk.a       Prebuilt static library to link against (self-contained;
                             cJSON is baked in — no extra runtime dependency).
  examples/hello_tool/       A complete, copy-me starting point (read-only, no perms).
  docs/AUTHORING.md          How to write a tool: the shape, the manifest, the sandbox.
  docs/BUILDING.md           How to build, link, and install your tool.
  docs/PROTOCOL.md           The wire + manifest spec (the contract, for any language).
  LICENSE / NOTICE / THIRD_PARTY.txt   Apache-2.0 + the bundled cJSON (MIT) attribution.
  VERSION                    The SDK / ABI version.

QUICK START
  1. Copy examples/hello_tool/ as your starting point.
  2. Edit main.c (your function bodies) and manifest.json (your tool's identity +
     the permissions it needs).
  3. Build:  cc my_tool.c -Iinclude -Llib -lhc_tool_sdk -lpthread -o my_tool
  4. Install: put it at  ~/.local/share/hypercat/tools/<id>/bin/<id>  with your
     manifest.json beside it (the tools/ dir must be chmod 700). Then enable it in
     HyperCat's Tools panel. See docs/BUILDING.md for the details.

REQUIREMENTS
  Linux only (the sandbox uses Landlock + seccomp). glibc 2.35 or newer. A C compiler.
  Tools are confined: no network or filesystem beyond what your manifest declares, and
  no spawning processes — see docs/AUTHORING.md.
