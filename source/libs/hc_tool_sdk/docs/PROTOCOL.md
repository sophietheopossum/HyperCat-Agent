# The HyperCat tool protocol (v1)

This is the contract a tool speaks. The C SDK (`hc_tool_main`) implements all of it for you, so you normally
never touch this — but it is published so the contract is auditable and so a tool can be written in any language
that can do Unix-domain sockets + JSON. The wire version is `HC_TOOL_ABI_VERSION = 1`; every request/reply body
carries `"v":1`. A host that speaks a different major version rejects the tool rather than mis-dispatching it.

## Runtime modes: native vs managed

A tool runs in one of two modes, chosen by the manifest's `runtime` block (default `native`). The *protocol* is
identical in both; only **who applies the jail** and **how you get the bus connection** differ:

- **`native`** — a COMPILED binary (the C SDK, or the Rust crate) installed at `bin/<id>`. The host spawns it
  directly; the tool **connects to `--sock` itself** and **self-confines** to the strict floor (no exec, no
  threads, no new sockets after confine). Maximally jailed. This is the path the C SDK and the Rust crate take.
- **`managed`** — an INTERPRETER (e.g. Python) named in the manifest. An interpreter can't self-confine (the
  strict floor kills `execve`/`clone`/`socket`, so it would die), so the host runs a small launcher
  (`hc_tool_launch`) that **jails itself first, then `execve`s the interpreter** on your entry script — the jail
  is inherited, transparently. Because the no-egress jail kills `socket()`, the launcher also **opens the bus for
  you before confining** and passes the connected fd as **`--bus-fd <fd>`**; your script uses that fd instead of
  connecting. The managed floor is slightly looser than native (threads/subprocesses are permitted *within* the
  fs+network jail) — the deliberate cost of running a real runtime. The Python helper takes this path.

So: a native tool does steps 1+4 below (connect, self-confine); a managed tool skips both (the launcher did the
confine; use `--bus-fd` instead of connecting) and otherwise speaks the exact same handshake + serve loop.

## How the host launches you

The host (its "ToolHost") starts your tool with a **scrubbed, empty environment** (no host secrets reach you —
there is nothing useful in `getenv`). It passes everything on argv / inherited fds:

```
# native: the host spawns your binary directly
your_tool  --sock <unix-socket-path> --id tool:<id> --token-fd <fd> --checkin-to toolhost
           [--workspace <dir>] [--workspace-mode rw|ro] [--allow-net]

# managed: the launcher jails itself, then execs:  <interpreter> <entry> <the same bus args> --bus-fd <fd>
python3 tool.py --sock <path> --id tool:<id> --token-fd <fd> --checkin-to toolhost --bus-fd <fd>
           [--workspace ...] [--workspace-mode ...] [--allow-net]   # (the launcher consumes the workspace/net
                                                                    #  flags itself; they may also appear here)
```

- `--token-fd <fd>`: a one-time secret token has been written to this inherited file descriptor. Read it (it is
  small; bounded) and close the fd. You present it at check-in to prove you are the process the host spawned.
- `--bus-fd <fd>` (**managed only**): an already-connected bus socket the launcher opened for you before jailing.
  Use this fd directly (do NOT connect to `--sock` — the jail forbids new sockets). Absent for native tools.
- `--workspace <dir>` + `--workspace-mode`: the single directory subtree you may touch, read-only (`ro`) or
  read-write (`rw`); present only if your manifest requested filesystem access. (For managed tools the launcher
  has already applied this to the jail; the flags are informational to your script.)
- `--allow-net`: your manifest requested egress (for managed, again already applied by the launcher).

## The bus envelope

Every message is one **length-prefixed JSON object** on the socket: a 4-byte unsigned length (big-endian) then
that many bytes of UTF-8 JSON. The envelope:

```json
{ "t": "<type>", "from": "<sender id>", "to": "<recipient id>", "corr": <int64>, "body": "<json string>" }
```

`t` is one of `hello` / `welcome` / `req` / `reply` / `err`. `corr` correlates a `reply`/`err` to its `req`.
`body` is itself a JSON object encoded as a string (the per-message payload below). The broker stamps `from`
with your authenticated id — you cannot forge it.

## The lifecycle

1. **Connect** to `--sock` (**native only**). A **managed** tool skips this — it uses the already-connected
   `--bus-fd` the launcher handed it.
2. **Hello**: send `{"t":"hello","from":"tool:<id>"}`. Await a `{"t":"welcome",...}` (the broker confirms your id
   is authorized). Anything else → give up.
3. **Check in**: send a `req` to `--checkin-to` (default `toolhost`), `corr` = 1, with
   `body = {"cmd":"checkin","v":1,"token":"<the token you read>"}`. Await the `corr`-1 `reply`; proceed only on
   `{"ok":true}`. (A bad/missing token → `{"ok":false}` → exit.)
4. **Confine yourself** (**native only**): drop to the kernel least-privilege floor BEFORE serving any call —
   Landlock for the filesystem (only your `--workspace` at the declared mode + system dirs read-only) and seccomp
   for syscalls (no `exec`, no `clone`/threads, no new sockets; network denied unless `--allow-net`). FAIL-CLOSED:
   if the full jail cannot be applied (a non-Linux host, or a kernel missing Landlock/seccomp), refuse to run. The
   C SDK does this via `hc_confine`; the Rust crate calls `hc_tool_confine()` by FFI. A **managed** tool does NOT
   do this step — the host launcher already jailed the interpreter before it started, so just go straight to
   serving.
5. **Serve** `req`s until shutdown:
   - `body = {"cmd":"tool.invoke","v":1,"tool":"<fn>","args":"<args-json-string>"}` → run your function `<fn>`
     with the arguments, then `reply` (same `corr`) with
     `{"v":1,"ok":true,"result":"<string>"}` or `{"v":1,"ok":false,"error":"<string>"}`.
   - `body = {"cmd":"tool.shutdown",...}` → `reply` `{"ok":true}` and exit cleanly.
   The host bounds each call by your manifest `timeout_ms`; if you do not reply in time it kills you and tells the
   caller the call timed out. Do blocking work and return. (A **native** tool is also single-threaded after
   confine — `clone`/`exec`/new sockets are denied; a **managed** tool MAY use threads/subprocesses within its
   jail, but cannot create new sockets or leave the workspace either way.)

## The manifest (`manifest.json`)

Shipped beside your binary; the host validates it at install and enforces it. `id` MUST equal the install
directory name. Default-deny: anything you don't request, you don't get.

```json
{
  "manifest_version": 1,
  "id": "my_tool",
  "name": "My Tool",
  "description": "One line; shown to the operator.",
  "author": "you",
  "version": "0.1.0",
  "runtime": { "mode": "native" },     // "native" (default — omit the block) | "managed" (adds interpreter+entry)
  "tools": [
    { "name": "my_fn", "sensitive": false,
      "spec_json": { "type": "function", "function": { "name": "my_fn", "description": "...",
                     "parameters": { "type": "object", "properties": { } } } } }
  ],
  "permissions": {
    "fs": { "mode": "none" },          // "none" | "read" | "readwrite" — a workspace subtree
    "egress_hosts": []                 // declared intent; see the note below
  },
  "limits": { "timeout_ms": 5000, "max_output_bytes": 4096, "cpu_seconds": 5,
              "mem_bytes": 134217728, "max_files": 64 }
}
```

Rules and v1 notes:

- `runtime`: omit it (or `{"mode":"native"}`) for a compiled `bin/<id>` that self-confines. For an interpreter
  use `{"mode":"managed","interpreter":"python3","entry":"tool.py"}` — `interpreter` must be an **allowlisted**
  name (`python3` / `python` / `node`; the host resolves it to a SYSTEM binary, never one shipped in your package)
  and `entry` must be a **single in-package file** (no `/`, not `.`/`..`). The whole package tree (every file
  except the runtime `work/`) is hash-pinned at install, so your scripts/modules are pinned, not just a binary.
- A `tools[].name` may not collide with a built-in tool name or use the `hc_`/`system_` prefixes.
- `sensitive: true` (or any manifest granting fs-write / egress / exec) makes **every call to that tool
  human-gated** by the operator at run time.
- `permissions.fs.mode`: kernel-enforced via Landlock. `read` is a read-only jail; `none` gets no workspace.
- `permissions.egress_hosts`: **declared intent, not a per-host firewall in v1.** A non-empty list grants
  *unrestricted* network egress at the kernel floor; the listed hosts drive the operator's review and make every
  network call human-gated. Request egress only if you truly need it.
- `permissions.exec_allow`: **not supported in v1** — a tool cannot spawn processes; a non-empty `exec_allow` is
  rejected at install. Leave it out.
- `limits`: clamped to host maxima. v1 enforces `timeout_ms`; `cpu`/`memory`/`files` are reserved (declared and
  clamped, not yet applied).

## Trust model (why this shape)

You run in your own process and address space, confined to your declared permissions, with no host secret in
your environment, and every sensitive action is operator-gated at the host. A crash or compromise in your tool
cannot reach the host or another agent. That is the whole point of the out-of-process design — and it is why the
contract above (the protocol + the manifest) is the entire surface, with no access to HyperCat's internals.
