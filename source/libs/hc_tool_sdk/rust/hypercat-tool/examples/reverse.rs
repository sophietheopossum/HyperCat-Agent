// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Savannah Goring. A HyperCat Tool SDK example (Apache-2.0).
//! reverse — a minimal NATIVE-runtime (Rust) HyperCat tool. Reverses the characters of args["text"].
//!
//! Build:  HC_TOOL_SDK_LIB_DIR=<dir with libhc_tool_sdk.a> cargo build --release --example reverse
//! Install: copy target/release/examples/reverse to <tool-dir>/bin/<id> next to reverse.manifest.json
//!          (renamed manifest.json, id = the install dir name). See docs/BUILDING.md.

use hypercat_tool::Value;

fn reverse(args: &Value) -> Result<String, String> {
    let text = args.get("text").and_then(|v| v.as_str()).unwrap_or("");
    Ok(text.chars().rev().collect())
}

fn main() {
    std::process::exit(hypercat_tool::serve(&[("reverse", reverse)]));
}
