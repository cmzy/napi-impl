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

# A handful of upstream tests hard-code V8's *exact* engine error STRING for an
# assignment that fails at the JS-engine level (assign to a read-only / getter-only
# property, add to a non-extensible object). The Node-API *behavior* is identical
# on every engine — a TypeError is thrown and the property is unchanged — but the
# message text is the engine's, not part of the Node-API contract (JSC says
# "Attempted to assign to readonly property." / "...not extensible"; V8 says
# "Cannot assign to read only property 'X' of object '#<Y>'", etc.). Running this
# V8-authored suite on JSC, those 4 cases fail purely on message wording. We relax
# each such regex to `/^TypeError:/` — still asserting the observable behavior (a
# TypeError is thrown) but engine-agnostic. The relaxed regex also matches V8, so
# the suite stays correct on every backend. If an upstream bump renames these
# literals, the patch warns (so it doesn't silently rot).
ENGINE_AGNOSTIC = "/^TypeError:/"
ENGINE_AGNOSTIC_ERROR_PATCHES = {
    "test_constructor/test.js": [
        "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
        "/^TypeError: Cannot assign to read only property 'readonlyValue' of object '#<MyObject>'$/",
    ],
    "test_properties/test.js": [
        "/^TypeError: Cannot assign to read only property '.*' of object '#<Object>'$/",
        "/^TypeError: Cannot set property .* of #<Object> which has only a getter$/",
    ],
    "6_object_wrap/test.js": [
        "/^TypeError: Cannot set property .* of #<.*> which has only a getter$/",
    ],
    "test_object/test.js": [
        "/Cannot add property w, object is not extensible/",
        "/Cannot assign to read only property 'x' of object '#<Object>/",
        "/Cannot delete property 'x' of #<Object>/",  # JSC: "Unable to delete property."
    ],
}


def relax_engine_specific_errors(target: Path):
    """Replace V8-exact error-string regexes with engine-agnostic /^TypeError:/."""
    for rel, literals in ENGINE_AGNOSTIC_ERROR_PATCHES.items():
        path = target / rel
        if not path.exists():
            print(f"[warn] engine-agnostic patch: {rel} not found", file=sys.stderr)
            continue
        text = path.read_text()
        for lit in literals:
            if lit not in text:
                print(f"[warn] engine-agnostic patch: literal not found in {rel}:\n"
                      f"        {lit}", file=sys.stderr)
                continue
            text = text.replace(lit, ENGINE_AGNOSTIC)
        path.write_text(text)
    print("[ok] relaxed V8-specific engine error-string assertions to /^TypeError:/")


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

    relax_engine_specific_errors(TARGET)
    stamp.write_text(args.tag + "\n")
    n = sum(1 for _ in TARGET.rglob("*.c")) + sum(1 for _ in TARGET.rglob("*.cc"))
    js = sum(1 for _ in TARGET.rglob("*.js"))
    print(f"[ok] test/js-native-api/ -> {args.tag} ({n} C/C++ + {js} JS test files)")


if __name__ == "__main__":
    main()
