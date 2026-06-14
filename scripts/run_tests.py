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


def binding_release_dir(platform: str) -> str:
    return "Release" if platform == "mac" else f"Release_{platform}"


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
    env["LD_LIBRARY_PATH"] = (
        str(runner.parent) + os.pathsep + env.get("LD_LIBRARY_PATH", ""))

    # iOS sim: spawn through simctl, pass env via SIMCTL_CHILD_*.
    sim_prefix: list[str] = []
    if args.platform == "ios_sim":
        env["SIMCTL_CHILD_DYLD_LIBRARY_PATH"] = str(runner.parent)
        sim_prefix = ["xcrun", "simctl", "spawn", "booted"]

    # Android: push everything once to /data/local/tmp/napi and shell via adb.
    android_dev = None
    if args.platform == "android":
        adb = os.environ.get("ANDROID_ADB") or str(
            Path.home() / "Android/Sdk/platform-tools/adb")
        android_dev = "/data/local/tmp/napi"
        subprocess.run([adb, "shell", "rm", "-rf", android_dev], check=False)
        subprocess.run([adb, "shell", "mkdir", "-p", android_dev], check=True)
        subprocess.run([adb, "push",
                        str(runner.parent / "libnapi_v8.so"), android_dev],
                       check=True, capture_output=True)
        subprocess.run([adb, "push", str(runner), android_dev],
                       check=True, capture_output=True)
        subprocess.run([adb, "shell", "chmod", "755",
                        f"{android_dev}/runner"], check=True)
        # Push libc++_shared.so so C++ bindings can dlopen.
        ndk = os.environ.get("ANDROID_NDK_ROOT") or os.environ.get(
            "ANDROID_NDK_HOME")
        if not ndk:
            ndk_dir = Path.home() / "Android/Sdk/ndk"
            if ndk_dir.is_dir():
                versions = sorted(p for p in ndk_dir.iterdir() if p.is_dir())
                if versions: ndk = str(versions[-1])
        if ndk:
            arch_dir = "x86_64-linux-android" if args.arch == "x86_64" else (
                "aarch64-linux-android")
            cxxshared = (Path(ndk) / "toolchains/llvm/prebuilt/linux-x86_64"
                         / "sysroot/usr/lib" / arch_dir / "libc++_shared.so")
            if cxxshared.exists():
                subprocess.run([adb, "push", str(cxxshared), android_dev],
                               check=True, capture_output=True)

    passed, failed, skipped = [], [], []

    for d in sorted(TESTS.iterdir()):
        if not d.is_dir():
            continue
        if args.filter and args.filter not in d.name:
            continue
        so = d / "build" / binding_release_dir(args.platform) / f"{d.name}.so"
        if not so.exists():
            skipped.append((d.name, "binding not built"))
            continue
        for tjs in list_tests(d):
            tag = f"{d.name}/{tjs.name}"
            if android_dev is not None:
                # Push binding + test, then adb shell.
                subprocess.run([adb, "push", str(so), android_dev],
                               check=True, capture_output=True)
                subprocess.run([adb, "push", str(tjs), android_dev],
                               check=True, capture_output=True)
                remote = (
                    f"cd {android_dev} && LD_LIBRARY_PATH={android_dev} "
                    f"./runner {android_dev}/{so.name} {d.name} "
                    f"{android_dev}/{tjs.name}"
                )
                cmd = [adb, "shell", remote]
            else:
                cmd = [*sim_prefix, str(runner), str(so), d.name, str(tjs)]
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
