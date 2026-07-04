# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Savannah Goring. Part of the HyperCat Tool SDK (see LICENSE, Apache-2.0).
# (The HyperCat application itself is licensed separately; this helper stands alone.)
"""hypercat_tool - the Python helper for a HyperCat MANAGED-runtime tool.

A managed tool is a plain Python script: you write one function per tool and call
``hypercat_tool.serve({...})``. There is NO confinement code here - the host launcher
(hc_tool_launch) has already jailed this interpreter (Landlock fs + seccomp) BEFORE it
exec'd Python, and it opened the bus socket for you and handed it in as ``--bus-fd N``
(the no-egress jail KILLs socket(), so the tool could not open the bus itself - this
mirrors how the native C SDK connects before self-confining). This module just speaks
the bus protocol over that inherited fd: hello -> welcome -> token check-in -> serve
``tool.invoke`` requests, dispatching each to your function and returning its result.

What you can rely on (the jail the launcher applied): your code runs in its own process
confined to the declared workspace subtree (read-only system dirs otherwise), with no
network unless the manifest granted egress, and a scrubbed environment (the host's API
key never reaches you). Threads and subprocesses ARE permitted within that jail (the
managed floor is looser than the native C floor by design), but you cannot create new
sockets or escape the filesystem jail, and every sensitive call is still operator-gated
at the host. Do blocking, single-purpose work over the call's arguments and return.

Wire contract (see docs/PROTOCOL.md): a frame is a 4-byte big-endian length prefix
followed by a JSON envelope ``{"t","from","to","corr","body"}`` whose ``body`` is itself
a JSON-encoded string. This is a PUBLIC, versioned ABI - keep it in step with the C SDK.

Usage:
    import hypercat_tool

    def word_count(args):            # args is the parsed arguments dict
        text = args.get("text", "")
        return str(len(text.split()))  # return a string result

    hypercat_tool.serve({"word_count": word_count})
"""

import json
import os
import socket
import struct
import sys

ABI_VERSION = 1
_MAX_FRAME = 16 * 1024 * 1024  # mirrors hc_transport's default frame cap (bounds a hostile/buggy peer)
_HANDSHAKE_TIMEOUT_S = 5.0     # bounded wait for welcome + check-in reply (mirrors the C SDK)


def _argval(name, default=None):
    """Return the value following ``name`` in argv, or default."""
    a = sys.argv
    for i in range(1, len(a) - 1):
        if a[i] == name:
            return a[i + 1]
    return default


def _read_token(fd):
    """Read the one-time check-in token from the inherited --token-fd (capped, until EOF)."""
    chunks = []
    total = 0
    while total < 4096:
        b = os.read(fd, 4096 - total)
        if not b:
            break
        chunks.append(b)
        total += len(b)
    os.close(fd)
    return b"".join(chunks).decode("utf-8", "replace")


def _send_frame(sock, t, frm=None, to=None, corr=0, body=None):
    """Send one framed envelope. Omitted fields match the C SDK (no key when empty/zero)."""
    env = {"t": t}
    if frm:
        env["from"] = frm
    if to:
        env["to"] = to
    if corr:
        env["corr"] = corr
    if body:
        env["body"] = body  # body is a STRING field holding JSON text (matches the C envelope)
    payload = json.dumps(env).encode("utf-8")
    sock.sendall(struct.pack(">I", len(payload)) + payload)


def _recv_exact(sock, n):
    """Read exactly n bytes, or raise ConnectionError on EOF/short close."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("bus closed")
        buf.extend(chunk)
    return bytes(buf)


def _recv_frame(sock):
    """Receive one framed envelope -> (type, from, corr, body_dict). Raises on close/oversize/parse error."""
    (length,) = struct.unpack(">I", _recv_exact(sock, 4))
    if length > _MAX_FRAME:
        raise ValueError("frame exceeds the maximum")
    env = json.loads(_recv_exact(sock, length).decode("utf-8"))
    body_txt = env.get("body", "")
    body = json.loads(body_txt) if body_txt else {}
    return env.get("t", ""), env.get("from", ""), int(env.get("corr", 0) or 0), body


def serve(tools):
    """Run the whole managed-tool lifecycle. ``tools`` maps a function name to a callable.

    Each callable takes the parsed arguments dict and returns a result; a non-string return is
    JSON-encoded. A raised exception becomes an error reply (the host relays it to the model).
    Returns a process exit code (0 = clean shutdown).
    """
    tool_id = _argval("--id")
    checkin_to = _argval("--checkin-to", "toolhost")
    token_fd_s = _argval("--token-fd")
    bus_fd_s = _argval("--bus-fd")
    if not tool_id or not token_fd_s or not bus_fd_s:
        sys.stderr.write("hypercat_tool: missing --id / --token-fd / --bus-fd\n")
        return 2

    token = _read_token(int(token_fd_s))
    if not token:
        sys.stderr.write("hypercat_tool: empty token\n")
        return 2

    # The launcher pre-opened + connected this fd; wrap it WITHOUT calling socket() (which the jail KILLs):
    # passing fileno + explicit family/type reuses the existing fd in place.
    sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM, 0, fileno=int(bus_fd_s))

    try:
        sock.settimeout(_HANDSHAKE_TIMEOUT_S)
        _send_frame(sock, "hello", frm=tool_id)
        t, _, _, _ = _recv_frame(sock)
        if t != "welcome":
            sys.stderr.write("hypercat_tool: no welcome from broker\n")
            return 1

        _send_frame(sock, "req", frm=tool_id, to=checkin_to, corr=1,
                    body=json.dumps({"cmd": "checkin", "v": ABI_VERSION, "token": token}))
        ok = False
        while True:  # await the corr=1 check-in reply
            t, _, corr, body = _recv_frame(sock)
            if corr == 1:
                ok = (t == "reply") and bool(body.get("ok", False))
                break
        if not ok:
            sys.stderr.write("hypercat_tool: check-in rejected (bad token / unknown id)\n")
            return 1

        sys.stderr.write("hypercat_tool %s: serving %d function(s)\n" % (tool_id, len(tools)))
        sock.settimeout(None)  # block indefinitely waiting for the next invoke
        while True:
            try:
                t, frm, corr, body = _recv_frame(sock)
            except (ConnectionError, ValueError, OSError):
                break  # host gone / bad frame -> exit the serve loop
            if t != "req":
                continue
            cmd = body.get("cmd", "")
            if cmd == "tool.shutdown":
                _send_frame(sock, "reply", frm=tool_id, to=frm, corr=corr, body=json.dumps({"ok": True}))
                break
            if cmd != "tool.invoke":
                _send_frame(sock, "reply", frm=tool_id, to=frm, corr=corr,
                            body=json.dumps({"ok": False, "error": "unknown command"}))
                continue
            fn = body.get("tool", "")
            args_txt = body.get("args", "") or "{}"
            reply = _invoke(tools, fn, args_txt)
            _send_frame(sock, "reply", frm=tool_id, to=frm, corr=corr, body=json.dumps(reply))
    finally:
        try:
            sock.close()
        except OSError:
            pass
    return 0


def _invoke(tools, fn, args_txt):
    """Dispatch one tool.invoke to the author's callback; never raises (errors become an error reply)."""
    func = tools.get(fn)
    if func is None:
        return {"v": ABI_VERSION, "ok": False, "error": "unknown tool function"}
    try:
        args = json.loads(args_txt) if args_txt else {}
        if not isinstance(args, dict):
            args = {}
        result = func(args)
        if not isinstance(result, str):
            result = json.dumps(result)  # a dict/number/list result is JSON-encoded to the string the host expects
        return {"v": ABI_VERSION, "ok": True, "result": result}
    except Exception as e:  # a tool bug must not crash the process - report it as a failed call
        return {"v": ABI_VERSION, "ok": False, "error": "tool raised: %s" % e}
