# Writing a HyperCat tool

HyperCat lets you extend the agents with your own tools, written in C, C++, Python, or Rust. A tool is a small
program that HyperCat runs in its own confined process and talks to over a local socket. When an agent decides to
use your tool, HyperCat sends it the call, your code runs, and you return a result. Welcome aboard (=^.^=)

There are two runtime modes (the manifest picks one). **Native** (C / C++ / Rust): a compiled binary that
self-confines to the strict floor — maximally jailed. **Managed** (Python / other interpreters): the host jails
the interpreter for you and hands it the bus, so you write no sandbox code — a slightly looser floor that allows
threads/subprocesses within the same jail. Same protocol either way; pick by language.

This is the SDK reference — *what* to write. Two companions cover the rest: **BUILDING.md** (how to compile,
link, and install your tool) and **PROTOCOL.md** (the wire + manifest spec — the contract, if you want to write
a tool in another language). It is deliberately small: you declare your function(s), call one entry point, and
the SDK handles the connection, the security handshake, and the sandbox.

## The shape of a tool

```c
#include "hc_tool.h"
#include <stdlib.h>
#include <string.h>

static char *my_invoke(const char *args_json, void *user) {
    /* args_json is a JSON object string (the call arguments). Do your work and return a malloc'd
     * result STRING. HyperCat frees it. Return NULL to signal failure. */
    const char *out = "{\"ok\":true}";
    char *r = malloc(strlen(out) + 1);
    if (r) strcpy(r, out);
    return r;
}

int main(int argc, char **argv) {
    hc_tool_def defs[] = {
        { "my_tool", "{\"type\":\"function\",\"function\":{\"name\":\"my_tool\"}}", my_invoke, NULL },
    };
    return hc_tool_main(argc, argv, defs, sizeof defs / sizeof defs[0]);
}
```

`hc_tool_main` is the whole program. It connects to the bus, checks in with the one-time token HyperCat
hands it, drops into a kernel sandbox, then serves calls until HyperCat shuts it down.

## The same tool in Python (managed runtime)

Prefer Python? Ship a script plus the helper module — no compilation, no sandbox code (the host launcher jails
the interpreter for you and hands it the bus). Vendor `hypercat_tool.py` (from the SDK's `python/`) next to your
script:

```python
import hypercat_tool

def my_tool(args):              # args: the parsed arguments dict
    name = args.get("name", "world")
    return f"hello, {name}"     # return a string (a dict/number/list is JSON-encoded for you)

hypercat_tool.serve({"my_tool": my_tool})
```

Set `"runtime": {"mode": "managed", "interpreter": "python3", "entry": "tool.py"}` in the manifest. The managed
floor is slightly looser than native — you MAY use threads and subprocesses, within the same filesystem + network
jail — the deliberate cost of running a real interpreter. See `examples/wordcount_tool_py/`.

## The same tool in Rust (native runtime)

A Rust tool is a compiled, maximally-jailed native binary — it self-confines via one FFI call into the SDK. Add
the `hypercat-tool` crate (from the SDK's `rust/`):

```rust
use hypercat_tool::Value;

fn my_tool(args: &Value) -> Result<String, String> {
    let name = args.get("name").and_then(|v| v.as_str()).unwrap_or("world");
    Ok(format!("hello, {name}"))    // Ok(result) or Err(message)
}

fn main() { std::process::exit(hypercat_tool::serve(&[("my_tool", my_tool)])); }
```

Its manifest is `runtime: native` (the default — omit the block). See `rust/hypercat-tool/examples/reverse.rs`;
BUILDING.md covers the link setup (the crate links the SDK's static lib for the confine FFI).

## The manifest

Beside your binary you ship a `manifest.json`. It declares your tool's identity, the function(s) it exposes,
and the permissions it needs. HyperCat reads it at install, shows it to the operator for approval, and
enforces it. See `examples/hello_tool/manifest.json` for the minimal read-only form. Key rules:

- `id` MUST equal the install directory name.
- `runtime`: omit it (native) for a compiled `bin/<id>`; for Python use
  `{"mode":"managed","interpreter":"python3","entry":"tool.py"}` — `interpreter` is an allowlisted name the host
  resolves to a system binary (never one in your package), `entry` a single in-package file (no `/` or `..`).
- A function `name` may not shadow a built-in tool (`fs_write`, `run`, etc.) or use the `hc_`/`system_`
  prefixes. Pick a distinct name.
- `permissions` default to deny. Request only what you need:
  - `fs.mode` (`none`/`read`/`readwrite`) — kernel-enforced: you get a Landlock grant to one workspace
    subtree at exactly that mode (a `read` tool cannot write; `none` gets no workspace at all).
  - `egress_hosts` — declared **intent**, not a per-host firewall. In v1 a non-empty list grants
    **unrestricted** network egress at the kernel floor; the listed hosts drive the operator's review and
    make **every network call human-gated at run time** (per-host kernel enforcement is a future hardening).
    Request egress only if you truly need it, and expect each call to prompt the operator.
  - `exec_allow` — **not supported in v1**: a tool cannot spawn processes (the sandbox blocks `exec`
    outright), so a non-empty `exec_allow` is **rejected at install**. Leave it out.
- `limits` are clamped to host maxima. v1 enforces `timeout_ms` (a tool that does not reply in time is
  killed and the call denied); `cpu`/`memory`/`files` are reserved (declared + clamped, not yet applied).

## The sandbox, and what it means for your code

Your `invoke` runs AFTER HyperCat has confined the process (for a **native** tool — for a **managed** tool the
host launcher confined the interpreter before it started). The floor is strict on purpose, so a bug or a hostile
input in your tool cannot reach beyond it:

- **(native only)** No new threads or processes (`clone`/`fork`/`exec` are blocked). Do single-threaded, blocking
  work and return. A **managed** (Python) tool MAY use threads/subprocesses, but still only within the same
  filesystem + network jail (no new sockets to the network, no escape from the workspace).
- No network unless your manifest requests egress. A granted tool gets **unrestricted** network at the kernel
  floor in v1, but every network call is gated by the operator at run time — so request it only if you need it.
- The filesystem is limited to the workspace subtree HyperCat grants you (per your `fs.mode`), read-only for
  `read`. Nothing else on disk is reachable.
- Your process cannot read another agent's memory or the host's secrets — and it inherits NO environment, so
  there is no host API key or config in your `getenv`.

If the sandbox cannot be installed (for example on a platform without it), the tool refuses to run rather
than running unprotected. On v1, third-party tools are Linux-only for this reason.

## Building

Link the SDK static library and its dependencies:

```cmake
add_executable(my_tool main.c)
target_link_libraries(my_tool PRIVATE hc::tool_sdk)
```

## Installing

Place your built binary and `manifest.json` under `~/.local/share/hypercat/tools/<id>/` (binary at
`bin/<id>`); the `tools/` directory must be host-private (`chmod 700`, owner-only — HyperCat refuses to load
from a world-readable install root). Then open the Tools panel: your package appears with its declared
permissions and a disclaimer. Enabling it the first time is type-to-confirm (you type its id) — on confirm
HyperCat records a hash of the manifest+binary (`manifest.lock`) and launches your tool confined. From then on
it launches when an agent calls it; if the bytes ever change, the launch is refused until you re-approve.

## A note on trust

Third-party tools are code you chose to install, running on your machine. HyperCat confines them and keeps
the operator in the loop for sensitive actions, but it does not vet what a tool does within its grants.
Install tools you trust.
