#!/usr/bin/env python3
"""Assemble a Prefab-enabled Android Archive (.aar) for an engine backend.

Layout produced (engine = v8 | hermes):
  napi-<engine>.aar
  ├── AndroidManifest.xml
  ├── classes.jar
  ├── jni/{abi}/libnapi_<engine>.so
  ├── prefab/prefab.json
  └── prefab/modules/napi_<engine>/{module.json,include/,libs/}

Consumers of the AAR use Gradle's externalNativeBuild + Prefab v2.

NOTE: the Hermes android .so requires a cross-compiled Hermes engine (host
hermesc import) — see PLAN.md M7.3. This script only assembles whatever .so
build.py produced.
"""
from __future__ import annotations

import argparse
import json
import shutil
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DIST = ROOT / "out" / "dist"

# Map our platform/arch to Android ABI naming.
ABI_MAP = {("android", "arm64"): "arm64-v8a",
           ("android", "x86_64"): "x86_64",
           ("android", "armeabi-v7a"): "armeabi-v7a"}

MANIFEST = """\
<manifest xmlns:android="http://schemas.android.com/apk/res/android"
          package="com.napi.{engine}" />
"""


def lib_name(engine: str) -> str:
    return f"libnapi_{engine}.so"


def build_dir(engine: str, platform: str, arch: str, config: str = "release") -> Path:
    base = ROOT / "out" / "build"
    if engine == "hermes":
        return base / f"{engine}-{platform}-{arch}-{config}" / "src" / "hermes"
    return base / f"v8-{platform}-{arch}-{config}"


def copy_headers(engine: str, dest: Path):
    shutil.copytree(ROOT / "include" / "napi", dest / "napi", dirs_exist_ok=True)
    (dest / "napi_v8").mkdir(exist_ok=True)
    if engine == "v8":
        shutil.copytree(ROOT / "include" / "napi_v8", dest / "napi_v8",
                        dirs_exist_ok=True)
    else:
        shutil.copy2(ROOT / "include" / "napi_v8" / "embedding.h",
                     dest / "napi_v8" / "embedding.h")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="v8", choices=["v8", "hermes"])
    ap.add_argument("--config", default="release")
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    engine = args.engine
    base = f"napi_{engine}"
    so_name = lib_name(engine)
    DIST.mkdir(parents=True, exist_ok=True)

    sos = {}
    for (plat, arch), abi in ABI_MAP.items():
        so = build_dir(engine, plat, arch, args.config) / so_name
        if so.exists():
            sos[abi] = so
    if not sos:
        sys.exit(f"[error] no Android {so_name} found; build android first")

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        (work / "AndroidManifest.xml").write_text(MANIFEST.format(engine=engine))
        with zipfile.ZipFile(work / "classes.jar", "w"):
            pass
        for abi, so in sos.items():
            jni_dir = work / "jni" / abi
            jni_dir.mkdir(parents=True)
            shutil.copy2(so, jni_dir / so_name)

        prefab = work / "prefab"
        prefab.mkdir()
        (prefab / "prefab.json").write_text(json.dumps({
            "name": base,
            "schema_version": 2,
            "dependencies": [],
            "version": args.version,
        }, indent=2))
        mod = prefab / "modules" / base
        mod.mkdir(parents=True)
        (mod / "module.json").write_text(json.dumps({
            "export_libraries": [],
            "android": {},
        }, indent=2))
        inc = mod / "include"
        inc.mkdir()
        copy_headers(engine, inc)
        libs = mod / "libs"
        for abi, so in sos.items():
            d = libs / f"android.{abi}"
            d.mkdir(parents=True)
            shutil.copy2(so, d / so_name)
            (d / "abi.json").write_text(json.dumps({
                "abi": abi,
                "api": 21,
                "ndk": 26,
                "stl": "c++_shared",
            }, indent=2))

        (work / "R.txt").write_text("")

        platforms = [f"android/{arch}" for (_, arch) in ABI_MAP
                     if build_dir(engine, "android", arch, args.config).is_dir()]
        info = ROOT / "scripts" / "gen_build_info.py"
        if info.exists():
            subprocess.check_call([
                sys.executable, str(info),
                "--platforms", *platforms,
                "--version", args.version,
                "--out", str(work / "BUILD_INFO.md")])

        out_aar = DIST / f"napi-{engine}.aar"
        if out_aar.exists():
            out_aar.unlink()
        with zipfile.ZipFile(out_aar, "w", zipfile.ZIP_DEFLATED) as zf:
            for p in work.rglob("*"):
                if p.is_file():
                    zf.write(p, p.relative_to(work))

    print(f"[done] {out_aar}")
    print(f"  ABIs: {sorted(sos)}")
    print(f"  size: {out_aar.stat().st_size / 1024 / 1024:.1f} MB")


if __name__ == "__main__":
    main()
