#!/usr/bin/env python3
"""Run nodejs/node test/js-native-api/ tests using our runner.

For each test/js-native-api/<feature>/build/Release/<feature>.so binding,
iterate every test*.js (and test.js) in that directory and invoke
runner with (binding.so, module_name, test.js).

Reports pass/fail counts.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "test" / "js-native-api"

def runner_bin(platform: str, arch: str, config: str) -> Path:
    return (ROOT / "third_party" / "v8" / "out"
            / f"napi-{platform}-{arch}-{config}" / "runner")


def list_tests(d: Path):
    """All .js files starting with 'test' in the directory."""
    return sorted(p for p in d.iterdir()
                  if p.is_file() and p.suffix == ".js"
                  and p.stem.startswith("test"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default="mac")
    ap.add_argument("--arch", default="x86_64")
    ap.add_argument("--config", default="release")
    ap.add_argument("--filter", default="", help="substring on feature dir name")
    ap.add_argument("--timeout", type=int, default=30)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--keep-going", action="store_true",
                    help="report all failures instead of stopping at first")
    args = ap.parse_args()

    runner = runner_bin(args.platform, args.arch, args.config)
    if not runner.exists():
        sys.exit(f"runner not built: {runner}")

    # libNapiV8.dylib has install_name="./libNapiV8.dylib"; help dyld find it.
    env = os.environ.copy()
    env["DYLD_LIBRARY_PATH"] = (
        str(runner.parent) + os.pathsep + env.get("DYLD_LIBRARY_PATH", ""))

    passed, failed, skipped = [], [], []

    for d in sorted(TESTS.iterdir()):
        if not d.is_dir():
            continue
        if args.filter and args.filter not in d.name:
            continue
        so = d / "build" / "Release" / f"{d.name}.so"
        if not so.exists():
            skipped.append((d.name, "binding not built"))
            continue
        for tjs in list_tests(d):
            tag = f"{d.name}/{tjs.name}"
            cmd = [str(runner), str(so), d.name, str(tjs)]
            try:
                r = subprocess.run(
                    cmd, capture_output=True, text=True,
                    timeout=args.timeout, env=env)
            except subprocess.TimeoutExpired:
                failed.append((tag, "timeout"))
                if args.verbose: print(f"[TIMEOUT] {tag}")
                continue
            if r.returncode == 0:
                passed.append(tag)
                if args.verbose: print(f"[PASS] {tag}")
            else:
                msg = (r.stderr.strip().split("\n")[-1]
                       if r.stderr.strip()
                       else f"exit {r.returncode}")
                failed.append((tag, msg))
                if args.verbose or not args.keep_going:
                    print(f"[FAIL] {tag}\n{r.stderr}")
                if not args.keep_going:
                    break
        else:
            continue
        if not args.keep_going and failed:
            break

    total = len(passed) + len(failed)
    print(f"\n=== summary ===")
    print(f"passed:  {len(passed)} / {total}")
    print(f"failed:  {len(failed)}")
    print(f"skipped: {len(skipped)} feature dirs")
    if failed and not args.verbose:
        print("\nfailures:")
        for t, m in failed[:20]:
            print(f"  {t}  --  {m}")
    sys.exit(0 if not failed else 1)


if __name__ == "__main__":
    main()
