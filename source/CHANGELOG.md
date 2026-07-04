# HyperCat changelog

All notable changes to HyperCat are recorded here. Versions follow a 0.x.y
scheme during the pre-1.0 phase.

## 2026-07-04: source publication

The HyperCat application source is now published in the HyperCat-Agent
repository, in the `source/` directory, under the Apache License 2.0. Earlier
test builds were distributed as closed-source binaries; the binary bundles on
the releases page remain the supported way to install.

## 0.1.2: tools in Python and Rust

What is new since 0.1.0:

- Write your own tools in Python and Rust, not only C and C++. A compiled tool
  (C, C++, Rust) self-confines to the strict kernel floor. A Python tool is
  jailed by a host launcher before the interpreter even starts, then speaks the
  same protocol. Either way the tool runs in its own confined process, with no
  network unless you grant it and no socket of its own, and every sensitive call
  still passes the operator gate. The Python floor is deliberately a little
  looser than the native one (it may use threads and subprocesses inside the same
  filesystem and network jail), which is the honest cost of running a real
  interpreter.
- The Tool SDK now ships a pure-standard-library Python helper and a Rust crate
  beside the C header and static library, with a worked example in each language
  and the docs to match.
- The SDK lives in its own repository now, kept separate from HyperCat, so the
  SDK and the application are versioned and released independently.
- The supply-chain pin covers a tool's whole package (a content hash over every
  file), so a Python tool's scripts are pinned exactly like a native binary and
  any change after you approve it is caught before launch.
- The conductor can be given access to your tools as well as the workers (off by
  default; you turn it on, and each sensitive call is still gated).
- Fixes and hardening across the tool subsystem, checked end to end with a real
  model driving a tool to a correct result.

Upgrading from 0.1.0: the supply-chain pin now hashes a tool's whole package
rather than only its manifest and binary, so any third-party tool you approved
under 0.1.0 is refused on first launch until you re-approve it in the Tools panel
(a one-time type-to-confirm). That is the pin doing its job, and nothing is lost.

## 0.1.0: first test release

The initial test build. Highlights:

- A conversational conductor that plans work and drives a fleet of worker
  agents, then reports back what they actually produced.
- Projects that seal each body of work, each with its own files, sessions, and
  memory.
- A jailed file workspace with a browser, a viewer, and an editor.
- A human approval gate on every agent action (write a file, run a command,
  save to memory), with an optional contained-write auto-approval for when you
  want less friction.
- Semantic memory carried across runs, an observability dashboard, and a
  per-worker activity timeline.
- A built-in music player.
- A customizable conductor personality (your voice on top of HyperCat's locked
  identity), the built-in tools as a toggleable System Tools catalog, and a
  framework for your own tools, written in C or C++ and run as confined,
  operator-approved sibling processes, with a Tools panel to manage them.
- A separately published Tool SDK (Apache-2.0) so you can build and ship those
  tools: one header plus one prebuilt static library, an example, a protocol
  spec, and an authoring guide. Built and verified to link on glibc 2.35+ Linux.
- Security throughout: a per-context filesystem sandbox, a default-deny network
  allowlist, a default-deny program allowlist, and API keys held in the OS
  keyring or the environment, never written to disk.
- Linux x86-64, installable on the major distribution families.
