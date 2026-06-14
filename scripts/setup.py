#!/usr/bin/env python3
"""One-shot environment setup for napi-impl.

Performs:
  1. Install depot_tools to third_party/depot_tools (or reuse).
  2. Write .gclient and sync V8 to third_party/v8 at the version in V8_VERSION.
  3. Apply patches in patches/v8/series (idempotent via .applied stamp).
  4. Symlink napi-impl into the V8 source tree as v8/napi/ so GN can build us.
  5. Sync node-api-headers into include/napi/.

Re-run safely: each step is idempotent.
"""
from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"
DEPOT_TOOLS = THIRD_PARTY / "depot_tools"
V8_DIR = THIRD_PARTY / "v8"
PATCHES_V8 = ROOT / "patches" / "v8"

DEPOT_TOOLS_URL = os.environ.get(
    "DEPOT_TOOLS_URL",
    "https://chromium.googlesource.com/chromium/tools/depot_tools.git",
)

V8_URL = "https://chromium.googlesource.com/v8/v8.git"


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    # On Windows, depot_tools' shims (gclient, gn) are .bat files; Python's
    # subprocess won't find them without shell or .bat extension. Convert to a
    # string command and use shell=True so cmd.exe handles PATHEXT resolution.
    if sys.platform == "win32" and isinstance(cmd, list):
        kw.setdefault("shell", True)
        subprocess.check_call(
            subprocess.list2cmdline([str(c) for c in cmd]), **kw)
        return
    subprocess.check_call(cmd, **kw)


def read_v8_version() -> str:
    txt = (ROOT / "V8_VERSION").read_text().strip().splitlines()
    for line in txt:
        line = line.strip()
        if line and not line.startswith("#"):
            return line
    raise RuntimeError("V8_VERSION file is empty")


def step_depot_tools():
    THIRD_PARTY.mkdir(exist_ok=True)
    if not (DEPOT_TOOLS / ".git").is_dir():
        run(["git", "clone", "--depth=1", DEPOT_TOOLS_URL, str(DEPOT_TOOLS)])
    # Bootstrap python3 inside depot_tools so the gn/ninja wrappers can find
    # their pinned interpreter (creates python3_bin_reldir.txt).
    if (not (DEPOT_TOOLS / "python3_bin_reldir.txt").exists()
            and sys.platform != "win32"):
        # Windows depot_tools self-bootstraps when gclient runs first; only
        # POSIX hosts need the manual bash bootstrap.
        run(["/bin/bash", "-c",
             "source bootstrap_python3 && bootstrap_python3"],
            cwd=str(DEPOT_TOOLS))


def env_with_depot_tools() -> dict:
    e = os.environ.copy()
    e["PATH"] = f"{DEPOT_TOOLS}{os.pathsep}{e.get('PATH', '')}"
    # Note: don't set DEPOT_TOOLS_UPDATE=0 here — it gates the bootstrap
    # path lookup too.
    e["DEPOT_TOOLS_WIN_TOOLCHAIN"] = "0"
    return e


def step_gclient_sync():
    THIRD_PARTY.mkdir(exist_ok=True)
    gclient = THIRD_PARTY / ".gclient"
    if not gclient.exists():
        content = (
            "solutions = [\n"
            "  {\n"
            "    'name': 'v8',\n"
            f"    'url': '{V8_URL}',\n"
            "    'deps_file': 'DEPS',\n"
            "    'managed': False,\n"
            "    'custom_deps': {},\n"
            "    'custom_vars': {},\n"
            "  },\n"
            "]\n"
        )
        gclient.write_text(content)

    version = read_v8_version()
    env = env_with_depot_tools()

    # Fetch V8 source.
    if not (V8_DIR / ".git").is_dir():
        run(["gclient", "sync", "--no-history", "--shallow",
             f"--revision=v8@{version}"], cwd=str(THIRD_PARTY), env=env)
    else:
        # Already cloned; switch to requested tag and sync deps.
        run(["git", "fetch", "--depth=1", "origin", f"refs/tags/{version}:refs/tags/{version}"],
            cwd=str(V8_DIR), env=env)
        run(["git", "checkout", version], cwd=str(V8_DIR), env=env)
        run(["gclient", "sync", "--no-history", "--shallow"], cwd=str(THIRD_PARTY), env=env)


def step_apply_patches():
    series = PATCHES_V8 / "series"
    if not series.exists() or not series.read_text().strip():
        print("[skip] no patches in patches/v8/series")
        return
    applied = PATCHES_V8 / ".applied"
    already = set(applied.read_text().splitlines()) if applied.exists() else set()
    new_applied = list(already)
    for line in series.read_text().splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        if name in already:
            print(f"[skip] {name} already applied")
            continue
        patch_path = PATCHES_V8 / name
        run(["git", "apply", "--whitespace=nowarn", str(patch_path)],
            cwd=str(V8_DIR))
        new_applied.append(name)
    applied.write_text("\n".join(new_applied) + "\n")


def step_symlink_into_v8():
    """Mount napi-impl into V8 tree at v8/napi/ for unified GN build."""
    link = V8_DIR / "napi"
    if link.is_symlink() or link.exists():
        # If pointing to ROOT already, nothing to do.
        try:
            if link.resolve() == ROOT.resolve():
                print("[skip] v8/napi symlink already points to napi-impl")
                return
        except OSError:
            pass
        if link.is_symlink():
            link.unlink()
        else:
            shutil.rmtree(link)
    rel = os.path.relpath(ROOT, V8_DIR)
    link.symlink_to(rel, target_is_directory=True)
    print(f"[ok] symlink {link} -> {rel}")


def step_sync_llhttp():
    # llhttp is vendored under vendor/llhttp and tracked in git; no download
    # needed. Kept as a no-op so older callers don't fail.
    if (ROOT / "vendor" / "llhttp" / "src" / "llhttp.c").exists():
        print("[skip] llhttp vendored at vendor/llhttp")
        return
    llhttp = THIRD_PARTY / "llhttp"
    if (llhttp / ".git").is_dir():
        print("[skip] llhttp already synced (third_party)")
        return
    run(["git", "clone", "--depth=1", "--branch", "release/v9.2.1",
         "https://github.com/nodejs/llhttp.git", str(llhttp)])


def step_sync_napi_headers():
    # Headers are vendored in include/napi/ and tracked in git; skip the
    # network sync unless explicitly requested via NAPI_FORCE_HEADER_SYNC=1.
    if ((ROOT / "include" / "napi" / "js_native_api.h").exists()
            and not os.environ.get("NAPI_FORCE_HEADER_SYNC")):
        print("[skip] napi headers vendored at include/napi")
        return
    run([sys.executable, str(ROOT / "scripts" / "sync_napi_headers.py")])


def step_sync_napi_tests():
    run([sys.executable, str(ROOT / "scripts" / "sync_napi_tests.py")])


def step_gen_exports():
    run([sys.executable, str(ROOT / "scripts" / "gen_export_list.py")])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--skip-v8", action="store_true",
                    help="Skip gclient sync (use existing third_party/v8)")
    args = ap.parse_args()

    step_depot_tools()
    if not args.skip_v8:
        step_gclient_sync()
    step_apply_patches()
    step_symlink_into_v8()
    step_sync_napi_headers()
    step_sync_napi_tests()
    step_sync_llhttp()
    step_gen_exports()

    (ROOT / ".setup_stamp").write_text(read_v8_version() + "\n")
    print("\n[done] setup complete. Next: python3 scripts/build.py "
          "--engine=v8 --platform=mac --arch=arm64")


if __name__ == "__main__":
    main()
