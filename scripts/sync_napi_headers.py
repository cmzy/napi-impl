#!/usr/bin/env python3
"""Sync Node-API headers from nodejs/node-api-headers to include/napi/.

We pin to a specific tag for reproducibility; bump via NAPI_HEADERS_VERSION.
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
INCLUDE_DIR = ROOT / "include" / "napi"

NAPI_HEADERS_REPO = "https://github.com/nodejs/node-api-headers.git"
DEFAULT_TAG = "v1.6.0"


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    subprocess.check_call(cmd, **kw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", default=os.environ.get("NAPI_HEADERS_VERSION", DEFAULT_TAG))
    args = ap.parse_args()

    INCLUDE_DIR.mkdir(parents=True, exist_ok=True)
    stamp = INCLUDE_DIR / ".version_stamp"
    if stamp.exists() and stamp.read_text().strip() == args.tag:
        print(f"[skip] include/napi/ already at {args.tag}")
        return

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        run(["git", "clone", "--depth=1", "--branch", args.tag,
             NAPI_HEADERS_REPO, str(tmp / "headers")])
        src_inc = tmp / "headers" / "include"
        if not src_inc.is_dir():
            raise SystemExit(f"[error] expected include/ in {NAPI_HEADERS_REPO}")

        # Wipe and copy.
        for child in INCLUDE_DIR.iterdir():
            if child.name == ".version_stamp":
                continue
            if child.is_dir():
                shutil.rmtree(child)
            else:
                child.unlink()

        for entry in src_inc.iterdir():
            dst = INCLUDE_DIR / entry.name
            if entry.is_dir():
                shutil.copytree(entry, dst)
            else:
                shutil.copy2(entry, dst)

        # Also stash the upstream .def symbol lists for gen_export_list.py.
        def_dir = tmp / "headers" / "def"
        for name in ("js_native_api.def", "node_api.def"):
            src = def_dir / name
            if src.exists():
                shutil.copy2(src, INCLUDE_DIR / name)

    stamp.write_text(args.tag + "\n")
    print(f"[ok] include/napi/ -> {args.tag}")


if __name__ == "__main__":
    main()
