#!/usr/bin/env python3
"""Build a NapiV8.framework for a single Apple platform (macOS or iOS).

Output:
  out/dist/NapiV8-<platform>.framework

Inputs (per arch, built by scripts/build.py beforehand):
  out/build/v8-mac-arm64-release/libNapiV8.dylib
  out/build/v8-mac-x86_64-release/libNapiV8.dylib
  out/build/v8-ios-arm64-release/libNapiV8.dylib       (device)
  out/build/v8-ios_sim-arm64-release/libNapiV8.dylib   (apple-silicon sim)
  out/build/v8-ios_sim-x86_64-release/libNapiV8.dylib  (intel sim)

Layout (macOS framework, bundle):
  NapiV8-macos.framework/
  ├── NapiV8                          (Mach-O, lipo: mac arm64+x86_64)
  ├── Headers/{napi,napi_v8}/
  ├── Modules/module.modulemap
  ├── Info.plist
  └── BUILD_INFO.md

Layout (iOS framework, flat bundle, device-only):
  NapiV8-ios.framework/
  ├── NapiV8                          (Mach-O, ios arm64 device)
  ├── Headers/{napi,napi_v8}/
  ├── Modules/module.modulemap
  ├── Info.plist
  └── BUILD_INFO.md

For iOS Simulator slices the cmake packages cover the use case.
"""
from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "out" / "dist"
PUBLIC_HEADERS = [
    ROOT / "include" / "napi",
    ROOT / "include" / "napi_v8",
]
INFO_PLIST_TMPL = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleExecutable</key><string>NapiV8</string>
<key>CFBundleIdentifier</key><string>com.napi.v8</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>NapiV8</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>{version}</string>
<key>CFBundleVersion</key><string>{version}</string>
<key>MinimumOSVersion</key><string>{minos}</string>
</dict></plist>
"""
MODULE_MAP = """\
framework module NapiV8 {
  umbrella header "umbrella.h"
  export *
  module * { export * }
}
"""
UMBRELLA = """\
#ifndef NAPIV8_UMBRELLA_H_
#define NAPIV8_UMBRELLA_H_
#include "napi/js_native_api.h"
#include "napi/node_api.h"
#include "napi_v8/embedding.h"
#include "napi_v8/inspector.h"
#include "napi_v8/sab.h"
#endif
"""


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    subprocess.check_call(cmd, **kw)


def lipo(out: Path, inputs: list[Path]) -> Path:
    if len(inputs) == 1:
        shutil.copy2(inputs[0], out)
    else:
        run(["lipo", "-create", *[str(p) for p in inputs], "-output", str(out)])
    return out


def build_dir(platform: str, arch: str) -> Path:
    return ROOT / "out" / "build" / f"v8-{platform}-{arch}-release"


def collect(slices: list[tuple[str, str]]) -> list[Path]:
    found = []
    for plat, arch in slices:
        p = build_dir(plat, arch) / "libNapiV8.dylib"
        if p.exists():
            found.append(p)
    return found


def make_framework(name: str, binary: Path, target_dir: Path,
                   minos: str, version: str) -> Path:
    fw = target_dir / f"{name}.framework"
    if fw.exists():
        shutil.rmtree(fw)
    fw.mkdir(parents=True)
    shutil.copy2(binary, fw / name)
    subprocess.run(["install_name_tool", "-id",
                    f"@rpath/{name}.framework/{name}", str(fw / name)],
                   check=False)
    headers = fw / "Headers"
    headers.mkdir()
    for src in PUBLIC_HEADERS:
        shutil.copytree(src, headers / src.name, dirs_exist_ok=True)
    (headers / "umbrella.h").write_text(UMBRELLA)
    modules = fw / "Modules"
    modules.mkdir()
    (modules / "module.modulemap").write_text(MODULE_MAP)
    (fw / "Info.plist").write_text(
        INFO_PLIST_TMPL.format(minos=minos, version=version))
    return fw


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--target", required=True, choices=["macos", "ios"])
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    DIST.mkdir(parents=True, exist_ok=True)
    work = DIST / f".{args.target}_work"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    if args.target == "macos":
        bins = collect([("mac", "arm64"), ("mac", "x86_64")])
        if not bins:
            sys.exit("[error] no mac dylibs found; build mac first")
        binary = lipo(work / "macos.NapiV8", bins)
        minos = "11.0"
        platforms_for_info = [f"mac/{p.parent.name.split('-')[2]}" for p in bins]
    else:  # ios
        bins = collect([("ios", "arm64")])
        if not bins:
            sys.exit("[error] no ios device dylib found; build ios arm64 first")
        binary = lipo(work / "ios.NapiV8", bins)
        minos = "13.0"
        platforms_for_info = ["ios/arm64"]

    fw_dir_name = f"NapiV8-{args.target}"
    fw = make_framework("NapiV8", binary, work,
                        minos=minos, version=args.version)
    # Move/rename to dist with target suffix.
    dest = DIST / f"{fw_dir_name}.framework"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.move(str(fw), str(dest))

    run([sys.executable, str(ROOT / "scripts" / "gen_build_info.py"),
         "--platforms", *platforms_for_info,
         "--version", args.version,
         "--out", str(dest / "BUILD_INFO.md")])

    shutil.rmtree(work, ignore_errors=True)

    size = sum(p.stat().st_size for p in dest.rglob("*") if p.is_file())
    print(f"\n[done] {dest}")
    print(f"  size: {size / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
