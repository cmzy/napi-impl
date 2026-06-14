#!/usr/bin/env python3
"""Sync Node.js NAPI test suite (test/js-native-api) into our test/ tree.

We use sparse + shallow clone to avoid pulling the full ~1GB nodejs/node repo.
The synced tree lands at test/js-native-api/ (gitignored).

Pin via NODE_TESTS_VERSION env var or --tag flag. Default tracks Node LTS.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TARGET = ROOT / "test" / "js-native-api"

NODE_REPO = "https://github.com/nodejs/node.git"
DEFAULT_TAG = "v22.11.0"   # Node 22 LTS; bump as needed

# Subpath inside nodejs/node to copy.
SUBPATH = "test/js-native-api"


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag",
                    default=os.environ.get("NODE_TESTS_VERSION", DEFAULT_TAG),
                    help="nodejs/node git tag (default: v22.11.0)")
    ap.add_argument("--force", action="store_true",
                    help="Re-sync even if already at target tag")
    args = ap.parse_args()

    stamp = TARGET / ".version_stamp"
    if stamp.exists() and stamp.read_text().strip() == args.tag and not args.force:
        print(f"[skip] test/js-native-api/ already at {args.tag}")
        return

    TARGET.parent.mkdir(parents=True, exist_ok=True)

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        clone = tmp / "node"
        # Sparse + shallow + blobless => only fetch the SUBPATH we need.
        run(["git", "clone", "--depth=1", "--filter=blob:none",
             "--sparse", "--branch", args.tag,
             NODE_REPO, str(clone)])
        run(["git", "sparse-checkout", "set", SUBPATH], cwd=str(clone))

        src = clone / SUBPATH
        if not src.is_dir():
            raise SystemExit(f"[error] {SUBPATH} not present in {args.tag}")

        # Replace target dir.
        if TARGET.exists():
            shutil.rmtree(TARGET)
        shutil.copytree(src, TARGET)

    stamp.write_text(args.tag + "\n")
    n = sum(1 for _ in TARGET.rglob("*.c")) + sum(1 for _ in TARGET.rglob("*.cc"))
    js = sum(1 for _ in TARGET.rglob("*.js"))
    print(f"[ok] test/js-native-api/ -> {args.tag} ({n} C/C++ + {js} JS test files)")


if __name__ == "__main__":
    main()
