#!/usr/bin/env python3
"""Emit a BUILD_INFO.md with the exact options used for a given platform/arch.

Used by package_apple / package_android / package_cmake to embed build
provenance into each artifact, and by the GitHub Actions workflow to populate
the release notes.

Source of truth:
  - V8_VERSION                        : pinned V8 stable tag
  - config/v8_args.yml                : default + per-platform GN args
  - config/v8_args.local.yml          : user override (CI: usually absent)
  - third_party/v8/out/<...>/args.gn  : effective merged args
  - include/napi/.version_stamp       : node-api-headers tag
  - third_party/llhttp (HEAD)         : llhttp tag
  - git rev-parse HEAD                : repo commit
"""
from __future__ import annotations

import argparse
import datetime
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def git_sha() -> str:
    try:
        return subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()[:12]
    except Exception:
        return "unknown"


def read_text(p: Path, default: str = "") -> str:
    try:
        return p.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return default


def llhttp_tag() -> str:
    # llhttp is vendored under vendor/llhttp; version pinned in this file.
    if (ROOT / "vendor" / "llhttp" / "src" / "llhttp.c").exists():
        return "vendored release/v9.2.1"
    repo = ROOT / "third_party" / "llhttp"
    try:
        return subprocess.check_output(
            ["git", "describe", "--tags", "--always"],
            cwd=repo, text=True).strip()
    except Exception:
        return "unknown"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platforms", nargs="+", required=True,
                    metavar="platform/arch",
                    help="e.g. mac/arm64 mac/x86_64 ios/arm64 ios_sim/arm64")
    ap.add_argument("--version", default="dev",
                    help="release version string (no v prefix)")
    ap.add_argument("--out", required=True, type=Path,
                    help="output BUILD_INFO.md path")
    args = ap.parse_args()

    lines: list[str] = []
    lines.append(f"# napi_v8 build info — release `{args.version}`")
    lines.append("")
    lines.append(f"- Commit: `{git_sha()}`")
    lines.append(f"- Built: `{datetime.datetime.utcnow().isoformat()}Z`")
    lines.append(f"- V8: `{read_text(ROOT / 'V8_VERSION', 'unknown')}`")
    lines.append(f"- NAPI headers: `{read_text(ROOT / 'include' / 'napi' / '.version_stamp', 'unknown')}`")
    lines.append(f"- llhttp: `{llhttp_tag()}`")
    lines.append("")

    # Defaults YAML, verbatim.
    lines.append("## YAML config (`config/v8_args.yml`)")
    lines.append("```yaml")
    lines.append(read_text(ROOT / "config" / "v8_args.yml"))
    lines.append("```")
    lines.append("")

    # Effective args.gn per requested platform/arch.
    for spec in args.platforms:
        try:
            platform, arch = spec.split("/", 1)
        except ValueError:
            sys.exit(f"--platforms element must be platform/arch, got {spec}")
        argsgn = (ROOT / "third_party" / "v8" / "out"
                  / f"napi-{platform}-{arch}-release" / "args.gn")
        lines.append(f"## Effective GN args — `{platform}/{arch}`")
        lines.append("```")
        lines.append(read_text(argsgn, "(args.gn not found — build skipped)"))
        lines.append("```")
        lines.append("")

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text("\n".join(lines))
    print(f"[ok] wrote {args.out}")


if __name__ == "__main__":
    main()
