#!/usr/bin/env python3
"""Assemble a Prefab-enabled Android Archive (.aar) for napi_v8.

Layout produced:
  napi-v8.aar
  ├── AndroidManifest.xml
  ├── classes.jar
  ├── jni/{abi}/libnapi_v8.so
  ├── prefab/prefab.json
  └── prefab/modules/napi_v8/{module.json,include/,libs/}

Consumers of the AAR use Gradle's externalNativeBuild + Prefab v2.
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
          package="com.napi.v8" />
"""

PREFAB_PREFAB_JSON = {
    "name": "napi_v8",
    "schema_version": 2,
    "dependencies": [],
    "version": "1.0.0",
}


def build_dir(platform: str, arch: str) -> Path:
    return ROOT / "out" / "build" / f"v8-{platform}-{arch}-release"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--version", default="1.0.0")
    args = ap.parse_args()

    DIST.mkdir(parents=True, exist_ok=True)

    sos = {}
    for (plat, arch), abi in ABI_MAP.items():
        so = build_dir(plat, arch) / "libnapi_v8.so"
        if so.exists():
            sos[abi] = so
    if not sos:
        sys.exit("[error] no Android .so found; build android first")

    with tempfile.TemporaryDirectory() as tmpdir:
        work = Path(tmpdir)
        # AndroidManifest.xml
        (work / "AndroidManifest.xml").write_text(MANIFEST)
        # classes.jar (empty)
        with zipfile.ZipFile(work / "classes.jar", "w") as jf:
            pass
        # jni/
        for abi, so in sos.items():
            jni_dir = work / "jni" / abi
            jni_dir.mkdir(parents=True)
            shutil.copy2(so, jni_dir / "libnapi_v8.so")

        # prefab/
        prefab = work / "prefab"
        prefab.mkdir()
        (prefab / "prefab.json").write_text(
            json.dumps({**PREFAB_PREFAB_JSON, "version": args.version},
                       indent=2))
        mod = prefab / "modules" / "napi_v8"
        mod.mkdir(parents=True)
        (mod / "module.json").write_text(json.dumps({
            "export_libraries": [],
            "android": {},
        }, indent=2))
        # Headers
        inc = mod / "include"
        inc.mkdir()
        for src in (ROOT / "include" / "napi", ROOT / "include" / "napi_v8"):
            shutil.copytree(src, inc / src.name, dirs_exist_ok=True)
        # Per-ABI libs
        libs = mod / "libs"
        for abi, so in sos.items():
            d = libs / f"android.{abi}"
            d.mkdir(parents=True)
            shutil.copy2(so, d / "libnapi_v8.so")
            (d / "abi.json").write_text(json.dumps({
                "abi": abi,
                "api": 21,
                "ndk": 26,
                "stl": "c++_shared",
            }, indent=2))

        # R.txt (empty)
        (work / "R.txt").write_text("")

        out_aar = DIST / "napi-v8.aar"
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
