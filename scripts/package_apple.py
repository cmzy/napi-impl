#!/usr/bin/env python3
"""Build an Apple .framework for a single platform (macOS or iOS).

Engine-aware (v8 | hermes); the two backends differ only in artifact names and
the public-header surface:

  v8     -> NapiV8.framework      (libnapi_v8.dylib);  ships napi/ + the full
            napi_v8/ embedding+inspector+sab headers.
  hermes -> NapiHermes.framework  (libnapi_hermes.dylib);  ships napi/ + only
            napi_v8/embedding.h (Hermes has no inspector / SharedArrayBuffer ext).

Output:
  out/dist/<Fw>-<platform>.framework        (<Fw> = NapiV8 | NapiHermes)

Inputs (per arch, built by scripts/build.py beforehand):
  out/build/<engine>-mac-arm64-release/.../<dylib>
  out/build/<engine>-mac-x86_64-release/.../<dylib>
  out/build/<engine>-ios-arm64-release/.../<dylib>       (device)

  where <...> is the engine's build subdir (v8: the build root; hermes:
  src/hermes/) and <dylib> is libnapi_v8.dylib / libnapi_hermes.dylib.

Layout (macOS framework, bundle):
  <Fw>-macos.framework/
  ├── <Fw>                            (Mach-O, lipo: mac arm64+x86_64 if both built)
  ├── Headers/{napi,napi_v8}/
  ├── Modules/module.modulemap
  ├── Info.plist
  └── BUILD_INFO.md

Layout (iOS framework, flat bundle, device-only):
  <Fw>-ios.framework/
  ├── <Fw>                            (Mach-O, ios arm64 device)
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

# Per-engine naming + header surface. `subdir` is appended to the per-arch build
# dir to reach the dylib (Hermes' CMake drops it under src/hermes/).
ENGINES = {
    "v8": {
        "dylib": "libnapi_v8.dylib",
        "fw": "NapiV8",
        "bundle_id": "com.napi.v8",
        "subdir": (),
        "v8_headers": True,   # ship full napi_v8/ + inspector+sab in umbrella
    },
    "hermes": {
        "dylib": "libnapi_hermes.dylib",
        "fw": "NapiHermes",
        "bundle_id": "com.napi.hermes",
        "subdir": ("src", "hermes"),
        "v8_headers": False,  # only napi_v8/embedding.h
    },
    # JSC is Apple-only and shipped as a STATIC framework (binary = libnapi_jsc.a):
    # iOS free-team apps have no dynamic-code entitlement and static linking is
    # preferred. Ships the full napi_v8/ surface (JSC supports inspector + SAB).
    # Distributed as an .xcframework (mac + ios device + ios simulator slices) —
    # device and simulator arm64 cannot share one flat framework, which is exactly
    # what xcframework solves.
    "jsc": {
        "dylib": "libnapi_jsc.a",
        "fw": "NapiJSC",
        "bundle_id": "com.napi.jsc",
        "subdir": ("src", "jsc"),
        "v8_headers": True,
        "static": True,
    },
}

INFO_PLIST_TMPL = """\
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleDevelopmentRegion</key><string>en</string>
<key>CFBundleExecutable</key><string>{fw}</string>
<key>CFBundleIdentifier</key><string>{bundle_id}</string>
<key>CFBundleInfoDictionaryVersion</key><string>6.0</string>
<key>CFBundleName</key><string>{fw}</string>
<key>CFBundlePackageType</key><string>FMWK</string>
<key>CFBundleShortVersionString</key><string>{version}</string>
<key>CFBundleVersion</key><string>{version}</string>
<key>MinimumOSVersion</key><string>{minos}</string>
</dict></plist>
"""
MODULE_MAP_TMPL = """\
framework module {fw} {{
  umbrella header "umbrella.h"
  export *
  module * {{ export * }}
}}
"""


def umbrella(fw: str, v8_headers: bool) -> str:
    guard = f"{fw.upper()}_UMBRELLA_H_"
    lines = [
        f"#ifndef {guard}",
        f"#define {guard}",
        '#include "napi/js_native_api.h"',
        '#include "napi/node_api.h"',
        '#include "napi/fast_call.h"',
        '#include "napi_v8/embedding.h"',
    ]
    if v8_headers:
        lines += ['#include "napi_v8/inspector.h"', '#include "napi_v8/sab.h"']
    lines.append("#endif")
    return "\n".join(lines) + "\n"


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    subprocess.check_call(cmd, **kw)


def lipo(out: Path, inputs: list[Path]) -> Path:
    if len(inputs) == 1:
        shutil.copy2(inputs[0], out)
    else:
        run(["lipo", "-create", *[str(p) for p in inputs], "-output", str(out)])
    return out


def build_dir(cfg: dict, engine: str, platform: str, arch: str) -> Path:
    return ROOT / "out" / "build" / f"{engine}-{platform}-{arch}-release" / Path(*cfg["subdir"])


def collect(cfg: dict, engine: str, slices: list[tuple[str, str]]) -> list[Path]:
    found = []
    for plat, arch in slices:
        p = build_dir(cfg, engine, plat, arch) / cfg["dylib"]
        if p.exists():
            found.append(p)
    return found


def copy_headers(cfg: dict, headers: Path):
    shutil.copytree(ROOT / "include" / "napi", headers / "napi", dirs_exist_ok=True)
    (headers / "napi_v8").mkdir(exist_ok=True)
    if cfg["v8_headers"]:
        shutil.copytree(ROOT / "include" / "napi_v8", headers / "napi_v8",
                        dirs_exist_ok=True)
    else:
        shutil.copy2(ROOT / "include" / "napi_v8" / "embedding.h",
                     headers / "napi_v8" / "embedding.h")


def make_framework(cfg: dict, binary: Path, target_dir: Path,
                   minos: str, version: str) -> Path:
    fw_name = cfg["fw"]
    fw = target_dir / f"{fw_name}.framework"
    if fw.exists():
        shutil.rmtree(fw)
    fw.mkdir(parents=True)
    shutil.copy2(binary, fw / fw_name)
    # A static framework's binary is an ar archive, not a Mach-O dylib, so it has
    # no install name to rewrite (skip install_name_tool — it would error).
    if not cfg.get("static"):
        subprocess.run(["install_name_tool", "-id",
                        f"@rpath/{fw_name}.framework/{fw_name}", str(fw / fw_name)],
                       check=False)
    headers = fw / "Headers"
    headers.mkdir()
    copy_headers(cfg, headers)
    (headers / "umbrella.h").write_text(umbrella(fw_name, cfg["v8_headers"]))
    modules = fw / "Modules"
    modules.mkdir()
    (modules / "module.modulemap").write_text(MODULE_MAP_TMPL.format(fw=fw_name))
    (fw / "Info.plist").write_text(INFO_PLIST_TMPL.format(
        fw=fw_name, bundle_id=cfg["bundle_id"], minos=minos, version=version))
    return fw


# Per-target slices (build.py platform/arch) + minimum OS. Device and simulator
# arm64 are distinct platforms (same arch) → must be separate xcframework slices.
TARGET_SLICES = {
    "macos":   ([("mac", "arm64"), ("mac", "x86_64")], "11.0"),
    "ios":     ([("ios", "arm64")], "13.0"),                      # device
    "ios_sim": ([("ios_sim", "arm64"), ("ios_sim", "x86_64")], "13.0"),
}


def build_one(cfg: dict, engine: str, target: str, version: str, work: Path) -> Path:
    """Build a single flat .framework for one target (macOS/iOS/iOS-sim)."""
    slices, minos = TARGET_SLICES[target]
    bins = collect(cfg, engine, slices)
    if not bins:
        sys.exit(f"[error] no {target} {cfg['dylib']} found; build {target} first")
    binary = lipo(work / f"{target}.bin", bins)
    sub = work / target
    sub.mkdir(parents=True, exist_ok=True)
    fw = make_framework(cfg, binary, sub, minos=minos, version=version)
    plats = [f"{plat}/{arch}" for plat, arch in slices
             if (build_dir(cfg, engine, plat, arch) / cfg["dylib"]).exists()]
    run([sys.executable, str(ROOT / "scripts" / "gen_build_info.py"),
         "--platforms", *plats, "--version", version,
         "--out", str(fw / "BUILD_INFO.md")])
    return fw


def report_size(dest: Path):
    size = sum(p.stat().st_size for p in dest.rglob("*") if p.is_file())
    print(f"\n[done] {dest}")
    print(f"  size: {size / 1024 / 1024:.1f} MB")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="v8", choices=["v8", "hermes", "jsc"])
    ap.add_argument("--target", required=True,
                    choices=["macos", "ios", "ios_sim", "xcframework"])
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    engine = args.engine
    cfg = ENGINES[engine]
    DIST.mkdir(parents=True, exist_ok=True)
    work = DIST / f".{engine}_{args.target}_work"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    if args.target == "xcframework":
        # Combine macOS + iOS device + iOS simulator static frameworks into one
        # .xcframework (the only correct way to ship device+sim arm64 together).
        if not cfg.get("static"):
            sys.exit(f"[error] xcframework target is for static engines (jsc), not {engine}")
        sub_fws = []
        for tgt in ("macos", "ios", "ios_sim"):
            slices, _ = TARGET_SLICES[tgt]
            if collect(cfg, engine, slices):  # only slices that were built
                sub_fws.append(build_one(cfg, engine, tgt, args.version, work))
        if not sub_fws:
            sys.exit("[error] no slices built; build mac/ios/ios_sim first")
        dest = DIST / f"{cfg['fw']}.xcframework"
        if dest.exists():
            shutil.rmtree(dest)
        xc = ["xcodebuild", "-create-xcframework"]
        for fw in sub_fws:
            xc += ["-framework", str(fw)]
        xc += ["-output", str(dest)]
        run(xc)
        shutil.rmtree(work, ignore_errors=True)
        report_size(dest)
        return

    fw = build_one(cfg, engine, args.target, args.version, work)
    dest = DIST / f"{cfg['fw']}-{args.target}.framework"
    if dest.exists():
        shutil.rmtree(dest)
    shutil.move(str(fw), str(dest))
    shutil.rmtree(work, ignore_errors=True)
    report_size(dest)


if __name__ == "__main__":
    main()
