#!/usr/bin/env python3
"""Renders the build manifest: the template with the path of the source tree to build (which may
be another checkout than the packaging) and, for test builds, the test suite."""
import argparse
import json
import os
import sys


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--template", required=True)
    p.add_argument("--source", required=True)
    p.add_argument("--run-tests", choices=["0", "1"], default="0")
    p.add_argument("--manifest-dir", required=True, help="sources are referenced relative to it (sandboxed builds need them inside)")
    a = p.parse_args()
    with open(a.template, encoding="utf-8") as f:
        m = json.load(f)
    base = os.path.realpath(a.manifest_dir)
    source = os.path.realpath(a.source)
    if os.path.commonpath([base, source]) != base:
        sys.exit(f"render-manifest: {source} is not inside {base}")

    found = [x for x in m["modules"] if x.get("name") == "klinker"]
    if len(found) != 1:
        sys.exit("render-manifest: module klinker not found once")
    klinker = found[0]
    srcs = [s for s in klinker["sources"] if s["type"] == "dir"]
    if len(srcs) != 1 or len(klinker["sources"]) != 1:
        sys.exit("render-manifest: module klinker must have exactly one dir source")
    srcs[0]["path"] = os.path.relpath(source, base)
    # Local build trees and IDE state of a developer checkout are never sources.
    srcs[0]["skip"] = sorted(set(srcs[0].get("skip", [])) | {
        e for e in os.listdir(source) if e in (".git", ".idea", ".cache", ".vscode") or e.startswith(("build", "cmake-build"))
    })
    if a.run_tests == "1":
        klinker["config-opts"] = [o for o in klinker["config-opts"] if not o.startswith("-DBUILD_TESTING=")] + ["-DBUILD_TESTING=ON"]
        klinker["run-tests"] = True
        # The whole suite, offscreen; tests isolate HOME, XDG and the host themselves.
        klinker["test-rule"] = ""
        klinker["test-commands"] = ["ctest --output-on-failure"]
    json.dump(m, sys.stdout, indent=4)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
