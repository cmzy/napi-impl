#!/usr/bin/env python3
"""Compile each test/js-native-api/<feature>/*.c into a loadable .so.

Output: test/js-native-api/<feature>/build/Release/<feature>.so

Discovers tests by scanning subdirectories that contain at least one .c or
.cc file. The module name is the directory basename (matches NAPI_MODULE's
NODE_GYP_MODULE_NAME convention used in entry_point.h).
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TESTS = ROOT / "test" / "js-native-api"
INCLUDE_NAPI = ROOT / "include"

# Directory holding the engine's napi library (matching scripts/build.py output).
def lib_dir(platform: str, arch: str, config: str, engine: str = "v8") -> Path:
    if engine == "hermes":
        return (ROOT / "out" / "build"
                / f"hermes-{platform}-{arch}-{config}" / "src" / "hermes")
    if engine == "jsc":
        return (ROOT / "out" / "build"
                / f"jsc-{platform}-{arch}-{config}" / "src" / "jsc")
    return ROOT / "third_party" / "v8" / "out" / f"napi-{platform}-{arch}-{config}"


def gyp_targets(feature_dir: Path):
    """Parse binding.gyp -> [(target_name, [source Paths])].

    Node test dirs can define several addons in one directory (e.g. test_object
    builds both `test_object` and `test_exceptions`); each is its own .so. gyp
    files are Python-dict-ish (trailing commas, optional # comments), so strip
    comments and literal-eval. Returns None to fall back to globbing.
    """
    gyp = feature_dir / "binding.gyp"
    if not gyp.exists():
        return None
    text = "\n".join(
        line for line in gyp.read_text(encoding="utf-8").splitlines()
        if not line.lstrip().startswith("#"))
    try:
        import ast
        data = ast.literal_eval(text)
        out = []
        for t in data.get("targets", []):
            name = t.get("target_name")
            srcs = [feature_dir / s for s in t.get("sources", [])
                    if isinstance(s, str) and s.endswith((".c", ".cc"))]
            srcs = [s for s in srcs if s.exists()]
            if name and srcs:
                out.append((name, sorted(srcs)))
        return out or None
    except (ValueError, SyntaxError, TypeError):
        return None


def build_one(name: str, sources, feature_dir: Path, libdir: Path,
              dry_run: bool, platform: str, arch: str = "x86_64") -> bool:
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
            "-target", f"{arch}-apple-ios13.0-simulator",
            "-mios-simulator-version-min=13.0",
        ]
    elif "ios" in str(libdir):
        sdk = subprocess.run(
            ["xcrun", "--sdk", "iphoneos", "--show-sdk-path"],
            capture_output=True, text=True).stdout.strip()
        extra_flags = [
            "-isysroot", sdk,
            "-target", f"{arch}-apple-ios13.0",
            "-miphoneos-version-min=13.0",
        ]
    elif "android" in str(libdir):
        ndk = os.environ.get("ANDROID_NDK_ROOT") or os.environ.get(
            "ANDROID_NDK_HOME")
        if not ndk:
            for candidate in (
                Path.home() / "Android/Sdk/ndk",
                Path("/opt/android-ndk"),
            ):
                if candidate.is_dir():
                    versions = sorted(p for p in candidate.iterdir()
                                       if p.is_dir())
                    if versions:
                        ndk = str(versions[-1])
                        break
        if not ndk:
            print("[fail-build] ANDROID_NDK_ROOT not set", file=sys.stderr)
            return False
        host = "linux-x86_64" if sys.platform.startswith("linux") else "darwin-x86_64"
        tc = Path(ndk) / "toolchains/llvm/prebuilt" / host / "bin"
        api = "21"
        if "x86_64" in str(libdir):
            cxx = str(tc / f"x86_64-linux-android{api}-clang")
            if is_cpp:
                cxx = str(tc / f"x86_64-linux-android{api}-clang++")
        else:
            cxx = str(tc / f"aarch64-linux-android{api}-clang")
            if is_cpp:
                cxx = str(tc / f"aarch64-linux-android{api}-clang++")
        extra_flags = ["-fPIC"]

    if (libdir / "libnapi_hermes.so").exists() or (libdir / "libnapi_hermes.dylib").exists():
        lib_link_name = "napi_hermes"
    elif (libdir / "libnapi_jsc.dylib").exists():
        lib_link_name = "napi_jsc"
    elif (libdir / "libnapi_v8.so").exists():
        lib_link_name = "napi_v8"
    else:
        lib_link_name = "NapiV8"
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
    ap.add_argument("--engine", default="v8", choices=["v8", "hermes", "jsc"])
    ap.add_argument("--platform", default="mac")
    ap.add_argument("--arch", default="x86_64")
    ap.add_argument("--config", default="release")
    ap.add_argument("--filter", default="", help="substring to filter test dirs")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    libdir = lib_dir(args.platform, args.arch, args.config, args.engine)
    lib_names = ("libNapiV8.dylib", "libnapi_v8.so", "napi_v8.dll",
                 "libnapi_hermes.so", "libnapi_hermes.dylib", "libnapi_jsc.dylib")
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
        # One .so per binding.gyp target (a dir may build several addons); fall
        # back to "all .c -> <dirname>.so" when there is no parseable gyp.
        targets = gyp_targets(d)
        if targets is None:
            targets = [(d.name,
                        sorted(p for p in d.iterdir()
                               if p.suffix in (".c", ".cc")))]
        for name, sources in targets:
            if build_one(name, sources, d, libdir, args.dry_run,
                         args.platform, args.arch):
                ok += 1
            else:
                fail += 1
    print(f"\n[summary] built {ok} / {ok + fail}, skipped {skipped}")
    sys.exit(0 if fail == 0 else 1)


if __name__ == "__main__":
    main()
