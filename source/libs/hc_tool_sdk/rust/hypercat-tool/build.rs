// SPDX-License-Identifier: Apache-2.0
// Link the HyperCat Tool SDK's fat static lib (libhc_tool_sdk.a) so the crate can FFI to hc_tool_confine().
// The lib bundles hc_confine/hc_transport/hc_json + static cJSON, so the only extra link need is libc (default).
// Point HC_TOOL_SDK_LIB_DIR at the directory holding libhc_tool_sdk.a. Defaults to ../../lib — the SDK repo /
// bundle layout (this crate sits at rust/hypercat-tool/, the fat lib at lib/), so `cargo build` works out of the
// box from a checkout of the published SDK. Override the env var when linking against a different build (e.g.
// HyperCat's CI gate points it at the freshly-built fat lib).
use std::env;

fn main() {
    let dir = env::var("HC_TOOL_SDK_LIB_DIR").unwrap_or_else(|_| "../../lib".to_string());
    println!("cargo:rustc-link-search=native={dir}");
    println!("cargo:rustc-link-lib=static=hc_tool_sdk");
    println!("cargo:rerun-if-env-changed=HC_TOOL_SDK_LIB_DIR");
}
