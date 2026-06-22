#!/usr/bin/env python3
"""Environment setup for the Hermes NAPI backend (CMake track).

Performs:
  1. Clone microsoft/hermes-windows to third_party/hermes-windows at the commit
     pinned in HERMES_VERSION (or reuse an existing checkout).
  2. Apply patches in patches/hermes/series (idempotent via .applied stamp).
  3. Sync node-api-headers into include/napi/ (shared with the V8 track).

Re-run safely: each step is idempotent. Build is a separate step
(scripts/build.py --engine=hermes).
"""
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"
HERMES_DIR = THIRD_PARTY / "hermes-windows"
PATCHES_HERMES = ROOT / "patches" / "hermes"

HERMES_URL = "https://github.com/microsoft/hermes-windows.git"


def run(cmd, **kw):
    print(f"$ {' '.join(str(c) for c in cmd)}", flush=True)
    subprocess.check_call(cmd, **kw)


def read_hermes_version() -> str:
    for line in (ROOT / "HERMES_VERSION").read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            return line
    raise RuntimeError("HERMES_VERSION file is empty")


def step_clone():
    THIRD_PARTY.mkdir(exist_ok=True)
    commit = read_hermes_version()
    if not (HERMES_DIR / ".git").is_dir():
        # Shallow fetch of just the pinned commit (GitHub allows SHA fetches).
        HERMES_DIR.mkdir(parents=True, exist_ok=True)
        run(["git", "init", "-q"], cwd=str(HERMES_DIR))
        run(["git", "remote", "add", "origin", HERMES_URL], cwd=str(HERMES_DIR))
        run(["git", "fetch", "--depth=1", "origin", commit], cwd=str(HERMES_DIR))
        run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=str(HERMES_DIR))
    else:
        head = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=str(HERMES_DIR)).decode().strip()
        if head != commit:
            run(["git", "fetch", "--depth=1", "origin", commit], cwd=str(HERMES_DIR))
            run(["git", "checkout", "-q", "FETCH_HEAD"], cwd=str(HERMES_DIR))
        else:
            print(f"[skip] hermes-windows already at {commit[:12]}")


def step_apply_patches():
    series = PATCHES_HERMES / "series"
    if not series.exists() or not series.read_text(encoding="utf-8").strip():
        print("[skip] no patches in patches/hermes/series")
        return
    applied = PATCHES_HERMES / ".applied"
    already = set(applied.read_text(encoding="utf-8").splitlines()) if applied.exists() else set()
    new_applied = list(already)
    for line in series.read_text(encoding="utf-8").splitlines():
        name = line.strip()
        if not name or name.startswith("#"):
            continue
        if name in already:
            print(f"[skip] {name} already applied")
            continue
        patch = PATCHES_HERMES / name
        if not patch.exists():
            raise SystemExit(f"[error] patch listed in series not found: {patch}")
        # Idempotent regardless of stamp: if the patch reverse-applies cleanly it
        # is already in the tree (e.g. a manual edit or a prior partial run).
        already_in_tree = subprocess.call(
            ["git", "apply", "--reverse", "--check", str(patch)],
            cwd=str(HERMES_DIR),
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL) == 0
        if already_in_tree:
            print(f"[skip] {name} already present in tree")
        else:
            run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=str(HERMES_DIR))
            print(f"[applied] {name}")
        new_applied.append(name)
    applied.write_text("\n".join(new_applied) + "\n", encoding="utf-8")


def main():
    step_clone()
    step_apply_patches()
    # NB: the public napi headers (include/napi/) are shared across engines and
    # managed by the V8 setup path (scripts/setup.py / sync_napi_headers.py). The
    # Hermes setup deliberately does not touch them — re-syncing here would pin a
    # different node-api-headers tag and silently change the V8 ABI too.
    print("\n[done] Hermes setup complete. Build with:")
    print("  python3 scripts/build.py --engine=hermes --platform=linux --arch=x86_64")


if __name__ == "__main__":
    main()
