// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Savannah Goring. Part of the HyperCat Tool SDK (see LICENSE, Apache-2.0).
// (The HyperCat application itself is licensed separately; this crate stands alone.)

//! `hypercat-tool` — write a **native**-runtime HyperCat tool in Rust.
//!
//! A native tool is a compiled binary (installed at `bin/<id>`) that SELF-confines: it connects to the host
//! message bus, completes the hello + token check-in handshake, then drops itself to the strict kernel floor
//! (Landlock fs + seccomp) by a single FFI call into the SDK's static lib ([`hc_tool_confine`]), and serves
//! `tool.invoke` calls. This is the *strict* floor — like the C SDK, after confine the tool may NOT spawn
//! threads/processes or open new sockets (the bus connection is acquired BEFORE confine and reused). For tools
//! that need a real threaded runtime, use the *managed* path instead (an interpreter jailed by the host launcher;
//! see `docs/PROTOCOL.md`) — Rust's strict native floor is the maximally-jailed option.
//!
//! What you can rely on (the jail this applies): your code runs in its own process confined to the declared
//! workspace subtree (system dirs read-only otherwise), no network unless the manifest granted egress, scrubbed
//! environment, and every sensitive call still operator-gated at the host.
//!
//! Wire contract (see `docs/PROTOCOL.md`): a frame is a 4-byte big-endian length prefix followed by a JSON
//! envelope `{"t","from","to","corr","body"}` whose `body` is itself a JSON-encoded string. A PUBLIC, versioned
//! ABI — keep it in step with the C SDK.
//!
//! ```no_run
//! use hypercat_tool::Value;
//! fn reverse(args: &Value) -> Result<String, String> {
//!     Ok(args.get("text").and_then(|v| v.as_str()).unwrap_or("").chars().rev().collect())
//! }
//! fn main() { std::process::exit(hypercat_tool::serve(&[("reverse", reverse)])); }
//! ```

use std::ffi::CString;
use std::io::{Read, Write};
use std::os::raw::{c_char, c_int};
use std::os::unix::net::UnixStream;

pub use serde_json::Value; // re-exported so a tool needs no direct serde_json dependency
use serde_json::json;

/// The wire/ABI version (matches `HC_TOOL_ABI_VERSION` in the C SDK).
pub const ABI_VERSION: i64 = 1;
const MAX_FRAME: u32 = 16 * 1024 * 1024; // mirrors hc_transport's default frame cap

/// One tool function: receives the parsed arguments and returns `Ok(result)` or `Err(message)`.
pub type ToolFn = fn(&Value) -> Result<String, String>;

extern "C" {
    /// SDK FFI: apply the strict Landlock+seccomp floor to THIS process, FAIL-CLOSED. Returns 0 only if the full
    /// jail is live; non-zero if confinement is unavailable/partial (the caller must refuse to run). `workspace`
    /// may be null; `workspace_rw`/`allow_net` are 0/1.
    fn hc_tool_confine(workspace: *const c_char, workspace_rw: c_int, allow_net: c_int) -> c_int;
}

fn argval(name: &str) -> Option<String> {
    let args: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i + 1 < args.len() {
        if args[i] == name {
            return Some(args[i + 1].clone());
        }
        i += 1;
    }
    None
}

fn has_flag(name: &str) -> bool {
    std::env::args().any(|a| a == name)
}

fn io_err<E: std::fmt::Display>(e: E) -> std::io::Error {
    std::io::Error::new(std::io::ErrorKind::Other, e.to_string())
}

/// Send one framed envelope. Omitted fields match the C SDK (no key when empty/zero). `body` is a JSON string.
fn send_frame(s: &mut UnixStream, t: &str, from: &str, to: Option<&str>, corr: i64, body: Option<&str>)
    -> std::io::Result<()>
{
    let mut env = json!({ "t": t });
    if !from.is_empty() {
        env["from"] = json!(from);
    }
    if let Some(to) = to {
        env["to"] = json!(to);
    }
    if corr != 0 {
        env["corr"] = json!(corr);
    }
    if let Some(b) = body {
        env["body"] = json!(b); // body is a STRING field holding JSON text (matches the C envelope)
    }
    let payload = serde_json::to_vec(&env).map_err(io_err)?;
    s.write_all(&(payload.len() as u32).to_be_bytes())?;
    s.write_all(&payload)?;
    Ok(())
}

/// Receive one framed envelope -> (type, from, corr, body). Errors on close/oversize/parse failure.
fn recv_frame(s: &mut UnixStream) -> std::io::Result<(String, String, i64, Value)> {
    let mut lenb = [0u8; 4];
    s.read_exact(&mut lenb)?;
    let len = u32::from_be_bytes(lenb);
    if len > MAX_FRAME {
        return Err(io_err("frame exceeds the maximum"));
    }
    let mut buf = vec![0u8; len as usize];
    s.read_exact(&mut buf)?;
    let env: Value = serde_json::from_slice(&buf).map_err(io_err)?;
    let t = env.get("t").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let from = env.get("from").and_then(|v| v.as_str()).unwrap_or("").to_string();
    let corr = env.get("corr").and_then(|v| v.as_i64()).unwrap_or(0);
    let body_txt = env.get("body").and_then(|v| v.as_str()).unwrap_or("");
    let body: Value = if body_txt.is_empty() {
        json!({})
    } else {
        serde_json::from_str(body_txt).unwrap_or_else(|_| json!({}))
    };
    Ok((t, from, corr, body))
}

/// Read the one-time check-in token from the inherited `--token-fd` (capped, until EOF). Takes ownership of `fd`.
fn read_token(fd: i32) -> Option<String> {
    use std::os::unix::io::FromRawFd;
    let mut f = unsafe { std::fs::File::from_raw_fd(fd) }; // closed on drop
    let mut buf = Vec::new();
    let mut tmp = [0u8; 512];
    loop {
        match f.read(&mut tmp) {
            Ok(0) => break,
            Ok(n) => {
                buf.extend_from_slice(&tmp[..n]);
                if buf.len() >= 4096 {
                    break;
                }
            }
            Err(ref e) if e.kind() == std::io::ErrorKind::Interrupted => continue,
            Err(_) => return None,
        }
    }
    Some(String::from_utf8_lossy(&buf).into_owned())
}

fn invoke(tools: &[(&str, ToolFn)], name: &str, args_txt: &str) -> String {
    let func = tools.iter().find(|(n, _)| *n == name).map(|(_, f)| *f);
    let func = match func {
        Some(f) => f,
        None => return json!({"v": ABI_VERSION, "ok": false, "error": "unknown tool function"}).to_string(),
    };
    let args: Value = serde_json::from_str(args_txt).unwrap_or_else(|_| json!({}));
    match func(&args) {
        Ok(result) => json!({"v": ABI_VERSION, "ok": true, "result": result}).to_string(),
        Err(e) => json!({"v": ABI_VERSION, "ok": false, "error": e}).to_string(),
    }
}

/// Run the whole native-tool lifecycle: connect, hello, check in, SELF-CONFINE (strict floor via FFI), then serve
/// `tool.invoke` calls until the host shuts the tool down. Returns a process exit code (0 = clean shutdown).
pub fn serve(tools: &[(&str, ToolFn)]) -> i32 {
    let sock = match argval("--sock") {
        Some(s) => s,
        None => {
            eprintln!("hypercat-tool: missing --sock");
            return 2;
        }
    };
    let id = match argval("--id") {
        Some(s) => s,
        None => {
            eprintln!("hypercat-tool: missing --id");
            return 2;
        }
    };
    let token_fd = match argval("--token-fd").and_then(|s| s.parse::<i32>().ok()) {
        Some(n) if n >= 3 => n,
        _ => {
            eprintln!("hypercat-tool: bad --token-fd");
            return 2;
        }
    };
    let checkin_to = argval("--checkin-to").unwrap_or_else(|| "toolhost".to_string());
    let workspace = argval("--workspace");
    let ws_rw = argval("--workspace-mode").as_deref() == Some("rw");
    let allow_net = has_flag("--allow-net");

    let token = match read_token(token_fd) {
        Some(t) if !t.is_empty() => t,
        _ => {
            eprintln!("hypercat-tool: empty token");
            return 2;
        }
    };

    // connect + hello + welcome + check-in — all BEFORE confine (socket/connect are still permitted here)
    let mut s = match UnixStream::connect(&sock) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("hypercat-tool: connect failed: {e}");
            return 1;
        }
    };
    let _ = s.set_read_timeout(Some(std::time::Duration::from_secs(5)));
    if send_frame(&mut s, "hello", &id, None, 0, None).is_err() {
        eprintln!("hypercat-tool: hello failed");
        return 1;
    }
    match recv_frame(&mut s) {
        Ok((t, ..)) if t == "welcome" => {}
        _ => {
            eprintln!("hypercat-tool: no welcome from broker");
            return 1;
        }
    }
    let checkin = json!({ "cmd": "checkin", "v": ABI_VERSION, "token": token }).to_string();
    if send_frame(&mut s, "req", &id, Some(&checkin_to), 1, Some(&checkin)).is_err() {
        eprintln!("hypercat-tool: checkin send failed");
        return 1;
    }
    let mut ok = false;
    loop {
        match recv_frame(&mut s) {
            Ok((t, _, 1, body)) => {
                ok = t == "reply" && body.get("ok").and_then(|v| v.as_bool()).unwrap_or(false);
                break;
            }
            Ok(_) => continue,
            Err(_) => break,
        }
    }
    if !ok {
        eprintln!("hypercat-tool: check-in rejected (bad token / unknown id)");
        return 1;
    }

    // self-confine the strict way (the bus fd is already open; no new socket/thread after this). FAIL-CLOSED.
    let ws_c = workspace.as_deref().map(|w| CString::new(w).unwrap_or_default());
    let ws_ptr = ws_c.as_ref().map(|c| c.as_ptr()).unwrap_or(std::ptr::null());
    let rc = unsafe { hc_tool_confine(ws_ptr, ws_rw as c_int, allow_net as c_int) };
    if rc != 0 {
        eprintln!("hypercat-tool {id}: full confinement unavailable — refusing to run (fail-closed)");
        return 8;
    }

    // serve invokes until shutdown (or the connection drops)
    let _ = s.set_read_timeout(None);
    eprintln!("hypercat-tool {id}: serving {} function(s)", tools.len());
    loop {
        let (t, from, corr, body) = match recv_frame(&mut s) {
            Ok(f) => f,
            Err(_) => break, // host gone / bad frame
        };
        if t != "req" {
            continue;
        }
        let cmd = body.get("cmd").and_then(|v| v.as_str()).unwrap_or("");
        if cmd == "tool.shutdown" {
            let _ = send_frame(&mut s, "reply", &id, Some(&from), corr, Some("{\"ok\":true}"));
            break;
        }
        if cmd != "tool.invoke" {
            let _ = send_frame(&mut s, "reply", &id, Some(&from), corr,
                               Some("{\"ok\":false,\"error\":\"unknown command\"}"));
            continue;
        }
        let fn_name = body.get("tool").and_then(|v| v.as_str()).unwrap_or("");
        let args_txt = body.get("args").and_then(|v| v.as_str()).unwrap_or("{}");
        let reply = invoke(tools, fn_name, args_txt);
        let _ = send_frame(&mut s, "reply", &id, Some(&from), corr, Some(&reply));
    }
    0
}
