#!/usr/bin/env python3
"""Build NapiV8.xcframework from per-arch dylibs.

Inputs (built by scripts/build.py beforehand):
  out/build/v8-mac-arm64-release/libNapiV8.dylib
  out/build/v8-mac-x86_64-release/libNapiV8.dylib
  out/build/v8-ios-arm64-release/libNapiV8.dylib       (device)
  out/build/v8-ios_sim-arm64-release/libNapiV8.dylib   (apple silicon sim)
  out/build/v8-ios_sim-x86_64-release/libNapiV8.dylib  (intel sim)

Output: out/dist/NapiV8.xcframework
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


def make_framework(name: str, binary: Path, target_dir: Path,
                   minos: str, version: str) -> Path:
    fw = target_dir / f"{name}.framework"
    if fw.exists():
        shutil.rmtree(fw)
    fw.mkdir(parents=True)
    # Binary
    shutil.copy2(binary, fw / name)
    # install_name fix so it resolves at @rpath
    subprocess.run(["install_name_tool", "-id",
                    f"@rpath/{name}.framework/{name}", str(fw / name)],
                   check=False)
    # Headers
    headers = fw / "Headers"
    headers.mkdir()
    for src in PUBLIC_HEADERS:
        dst = headers / src.name
        shutil.copytree(src, dst, dirs_exist_ok=True)
    (headers / "umbrella.h").write_text(UMBRELLA)
    # Module map
    modules = fw / "Modules"
    modules.mkdir()
    (modules / "module.modulemap").write_text(MODULE_MAP)
    # Info.plist
    (fw / "Info.plist").write_text(
        INFO_PLIST_TMPL.format(minos=minos, version=version))
    return fw


def build_dir(platform: str, arch: str) -> Path:
    return ROOT / "out" / "build" / f"v8-{platform}-{arch}-release"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    DIST.mkdir(parents=True, exist_ok=True)
    work = DIST / ".apple_work"
    if work.exists(): shutil.rmtree(work)
    work.mkdir()

    slices = []

    # macOS slice (lipo arm64 + x86_64)
    mac_bins = [build_dir("mac", a) / "libNapiV8.dylib"
                for a in ("arm64", "x86_64") if (build_dir("mac", a) / "libNapiV8.dylib").exists()]
    if mac_bins:
        mac_bin = lipo(work / "mac.NapiV8", mac_bins)
        mac_fw = make_framework("NapiV8", mac_bin, work / "macos",
                                "11.0", args.version)
        slices += ["-framework", str(mac_fw)]

    # iOS device slice (arm64 only)
    ios_dev = build_dir("ios", "arm64") / "libNapiV8.dylib"
    if ios_dev.exists():
        bin = lipo(work / "ios_dev.NapiV8", [ios_dev])
        fw = make_framework("NapiV8", bin, work / "ios_dev",
                            "13.0", args.version)
        slices += ["-framework", str(fw)]

    # iOS simulator slice (lipo arm64 + x86_64)
    sim_bins = [build_dir("ios_sim", a) / "libNapiV8.dylib"
                for a in ("arm64", "x86_64")
                if (build_dir("ios_sim", a) / "libNapiV8.dylib").exists()]
    if sim_bins:
        sim_bin = lipo(work / "ios_sim.NapiV8", sim_bins)
        fw = make_framework("NapiV8", sim_bin, work / "ios_sim",
                            "13.0", args.version)
        slices += ["-framework", str(fw)]

    if not slices:
        sys.exit("[error] no per-platform dylibs found; run build.py first")

    out_xcf = DIST / "NapiV8.xcframework"
    if out_xcf.exists():
        shutil.rmtree(out_xcf)

    run(["xcodebuild", "-create-xcframework",
         *slices, "-output", str(out_xcf)])
    shutil.rmtree(work, ignore_errors=True)

    print(f"\n[done] {out_xcf}")
    print(f"  size: {sum(p.stat().st_size for p in out_xcf.rglob('*') if p.is_file()) / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
