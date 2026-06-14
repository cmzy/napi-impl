#!/usr/bin/env python3
"""Compile each test/js-native-api/<feature>/*.c into a loadable .so.

Output: test/js-native-api/<feature>/build/Release/<feature>.so

Discovers tests by scanning subdirectories that contain at least one .c or
.cc file. The module name is the directory basename (matches NAPI_MODULE's
NODE_GYP_MODULE_NAME convention used in entry_point.h).
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "test" / "js-native-api"
INCLUDE_NAPI = ROOT / "include"

# Path to libNapiV8.dylib (mac arm64/x86_64 build dir, matching build.py).
def lib_dir(platform: str, arch: str, config: str) -> Path:
    return ROOT / "third_party" / "v8" / "out" / f"napi-{platform}-{arch}-{config}"


def build_one(feature_dir: Path, libdir: Path, dry_run: bool,
              platform: str) -> bool:
    name = feature_dir.name
    sources = sorted(p for p in feature_dir.iterdir()
                     if p.suffix in (".c", ".cc"))
    if not sources:
        return False
    # mac uses ./build/Release/ (Node convention so test.js can find it via
    # common.buildType); other platforms use a platform-suffixed dir.
    rel = "Release" if platform == "mac" else f"Release_{platform}"
    out_dir = feature_dir / "build" / rel
    out_dir.mkdir(parents=True, exist_ok=True)
    out_so = out_dir / f"{name}.so"

    cxx = "clang"
    is_cpp = any(s.suffix == ".cc" for s in sources)
    if is_cpp:
        cxx = "clang++"

    extra_flags = []
    if "ios_sim" in str(libdir):
        sdk = subprocess.run(
            ["xcrun", "--sdk", "iphonesimulator", "--show-sdk-path"],
            capture_output=True, text=True).stdout.strip()
        extra_flags = [
            "-isysroot", sdk,
            "-target", "x86_64-apple-ios13.0-simulator",
            "-mios-simulator-version-min=13.0",
        ]
    elif "ios" in str(libdir):
        sdk = subprocess.run(
            ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"],
            capture_output=True, text=True).stdout.strip()
        extra_flags = [
            "-isysroot", sdk,
            "-target", "arm64-apple-ios13.0",
            "-miphoneos-version-min=13.0",
        ]

    lib_link_name = "napi_v8" if (libdir / "libnapi_v8.so").exists() else "NapiV8"
    cmd = [
        cxx,
        "-shared", "-fPIC", "-fvisibility=hidden",
        *extra_flags,
        f"-I{INCLUDE_NAPI / 'napi'}",   # tests do #include <js_native_api.h>
        f"-I{INCLUDE_NAPI}",             # for users including <napi/...>
        f"-I{TESTS}",
        "-DBUILDING_NODE_EXTENSION",
        "-DNAPI_VERSION=10",
        "-DNAPI_EXPERIMENTAL",
        f"-DNODE_GYP_MODULE_NAME={name}",
        "-O2",
        "-Wno-everything",
        *(str(s) for s in sources),
        f"-L{libdir}", f"-l{lib_link_name}",
        f"-Wl,-rpath,{libdir}",
        "-o", str(out_so),
    ]
    if is_cpp:
        cmd.insert(2, "-std=c++17")

    if dry_run:
        print(" ".join(cmd))
        return True
    print(f"[build] {name}")
    res = subprocess.run(cmd, capture_output=True, text=True)
    if res.returncode != 0:
        print(f"[fail-build] {name}\n{res.stderr}", file=sys.stderr)
        return False
    # libNapiV8.dylib was linked with install_name="./libNapiV8.dylib", which
    # dyld can't resolve via @rpath. Rewrite the dep to absolute path.
    if (libdir / "libNapiV8.dylib").exists():
        subprocess.run(
            ["install_name_tool", "-change",
             "./libNapiV8.dylib",
             str(libdir / "libNapiV8.dylib"),
             str(out_so)],
            check=False)
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default="mac")
    ap.add_argument("--arch", default="x86_64")
    ap.add_argument("--config", default="release")
    ap.add_argument("--filter", default="", help="substring to filter test dirs")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    libdir = lib_dir(args.platform, args.arch, args.config)
    lib_names = ("libNapiV8.dylib", "libnapi_v8.so", "napi_v8.dll")
    if not any((libdir / n).exists() for n in lib_names):
        sys.exit(f"napi library not found in {libdir} — run scripts/build.py first")

    if not TESTS.is_dir():
        sys.exit(f"{TESTS} missing — run scripts/sync_napi_tests.py first")

    ok = fail = skipped = 0
    for d in sorted(TESTS.iterdir()):
        if not d.is_dir():
            continue
        if args.filter and args.filter not in d.name:
            skipped += 1
            continue
        if not any(p.suffix in (".c", ".cc") for p in d.iterdir()):
            skipped += 1
            continue
        if build_one(d, libdir, args.dry_run, args.platform):
            ok += 1
        else:
            fail += 1
    print(f"\n[summary] built {ok} / {ok + fail}, skipped {skipped}")
    sys.exit(0 if fail == 0 else 1)


if __name__ == "__main__":
    main()
