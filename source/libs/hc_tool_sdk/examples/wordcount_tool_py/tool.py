#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Savannah Goring. A HyperCat Tool SDK example (Apache-2.0).
"""wordcount-py - a minimal MANAGED-runtime (Python) HyperCat tool.

The host launcher (hc_tool_launch) has already jailed this interpreter and opened the bus
before exec'ing Python; this script only declares its function and calls serve(). `hypercat_tool`
is vendored alongside this script in the installed package (the SDK ships python/hypercat_tool.py;
copy it next to tool.py). The manifest declares runtime: managed with interpreter python3 + entry
tool.py, so the ToolHost launches `python3 tool.py ...` under the managed jail.
"""
import hypercat_tool


def word_count(args):
    """Count whitespace-separated words in args['text']; return the count as a string."""
    text = args.get("text", "")
    return str(len(text.split()))


if __name__ == "__main__":
    raise SystemExit(hypercat_tool.serve({"word_count": word_count}))
