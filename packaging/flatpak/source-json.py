#!/usr/bin/env python3
"""Describes a build for the central publisher: what was built from which source with which
packaging revision, packaging files, rendered manifest and runtime/SDK commits, and the digest of
the repository archive. The publisher checks it against the source run, tag and repository."""
import argparse
import hashlib
import json
import os
import subprocess
import sys


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def ostree(repo, *args):
    return subprocess.run(["ostree", f"--repo={repo}", *args], check=True, capture_output=True, text=True).stdout


def main():
    p = argparse.ArgumentParser()
    for name in ("repo", "tar", "app-id", "arch", "branch", "tag", "commit", "version",
                 "packaging-root", "packaging-commit", "rendered-manifest"):
        p.add_argument("--" + name, required=True)
    p.add_argument("--packaging-file", action="append", default=[], help="path relative to --packaging-root")
    a = p.parse_args()
    refs = {ref: ostree(a.repo, "rev-parse", ref).strip() for ref in sorted(ostree(a.repo, "refs").split())}
    app_ref = f"app/{a.app_id}/{a.arch}/{a.branch}"
    # flatpak-builder's own record of the runtime and SDK it built against.
    built = json.loads(ostree(a.repo, "cat", app_ref, "/files/manifest.json"))
    meta = dict(l.split("=", 1) for l in ostree(a.repo, "cat", app_ref, "/metadata").splitlines() if "=" in l)
    json.dump({
        "format": 2,
        "app_id": a.app_id,
        "arch": a.arch,
        "branch": a.branch,
        "source_tag": a.tag,
        "source_commit": a.commit,
        "version": a.version,
        "refs": refs,
        "repo_tar": "repo.tar",
        "repo_tar_sha256": sha256(a.tar),
        "packaging_commit": a.packaging_commit,
        "packaging_files": {f: sha256(os.path.join(a.packaging_root, f)) for f in sorted(a.packaging_file)},
        "rendered_manifest_sha256": sha256(a.rendered_manifest),
        "runtime": {"ref": meta["runtime"], "commit": built["runtime-commit"]},
        "sdk": {"ref": meta["sdk"], "commit": built["sdk-commit"]},
    }, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")


if __name__ == "__main__":
    main()
