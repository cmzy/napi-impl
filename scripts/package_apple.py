#!/usr/bin/env python3
"""Build an Apple .framework for a single platform (macOS or iOS).

Engine-aware (v8 | hermes); the two backends differ only in artifact names and
the public-header surface:

  v8     -> NapiV8.framework      (libNapiV8.dylib);  ships napi/ + the full
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
  src/hermes/) and <dylib> is libNapiV8.dylib / libnapi_hermes.dylib.

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
        "dylib": "libNapiV8.dylib",
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="v8", choices=["v8", "hermes"])
    ap.add_argument("--target", required=True, choices=["macos", "ios"])
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    engine = args.engine
    cfg = ENGINES[engine]

    DIST.mkdir(parents=True, exist_ok=True)
    work = DIST / f".{engine}_{args.target}_work"
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)

    if args.target == "macos":
        bins = collect(cfg, engine, [("mac", "arm64"), ("mac", "x86_64")])
        if not bins:
            sys.exit(f"[error] no mac {cfg['dylib']} found; build mac first")
        binary = lipo(work / "macos.bin", bins)
        minos = "11.0"
        platforms_for_info = [f"mac/{p.parents[len(cfg['subdir'])].name.split('-')[2]}"
                              for p in bins]
    else:  # ios
        bins = collect(cfg, engine, [("ios", "arm64")])
        if not bins:
            sys.exit(f"[error] no ios device {cfg['dylib']} found; build ios arm64 first")
        binary = lipo(work / "ios.bin", bins)
        minos = "13.0"
        platforms_for_info = ["ios/arm64"]

    fw = make_framework(cfg, binary, work, minos=minos, version=args.version)
    # Move/rename to dist with target suffix.
    dest = DIST / f"{cfg['fw']}-{args.target}.framework"
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
