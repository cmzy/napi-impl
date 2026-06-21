#!/usr/bin/env python3
"""Generate platform-specific symbol export lists.

Inputs:
  include/napi/js_native_api.def — upstream js_native_api symbols
  include/napi/node_api.def      — upstream node_api symbols (not exported,
                                    we don't implement node_api extensions)
  EMBEDDING_SYMBOLS              — symbols we declare in napi_v8/embedding.h

Outputs (gn/exports/):
  napi_v8.lds — Linux/Android version script
  napi_v8.exp — macOS/iOS Mach-O exported_symbols_list
  napi_v8.def — Windows MSVC .def

M1 only exports js_native_api + our embedding API. node_api extensions
(async_work, threadsafe_function, ...) are not implemented and not exported.
"""
from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE = ROOT / "include" / "napi"
EXPORTS = ROOT / "gn" / "exports"

# Our own embedding API (no upstream equivalent).
EMBEDDING_SYMBOLS = [
    "napi_create_platform",
    "napi_destroy_platform",
    "napi_create_runtime",
    "napi_destroy_runtime",
    "napi_create_env",
    "napi_destroy_env",
    "napi_v8_inspector_start",
    "napi_v8_inspector_stop",
    "napi_v8_inspector_wait_for_connection",
    # Inspector message loop (host-driven; napi_v8/inspector.h).
    "napi_v8_inspector_pump_messages",
    "napi_v8_inspector_is_paused",
    "napi_v8_inspector_wait",
    "napi_v8_inspector_set_pause_handler",
    "napi_v8_inspector_set_wake_handler",
    # Host-driven event-loop tick: pump foreground tasks + drain finalizers
    # (napi_v8/embedding.h).
    "napi_v8_run_event_loop_tasks",
    # SharedArrayBuffer extensions (napi_v8/sab.h).
    "napi_v8_create_shared_arraybuffer",
    "napi_v8_is_shared_arraybuffer",
    "napi_v8_get_shared_arraybuffer_info",
    # node_api_* extensions we implement beyond js_native_api proper.
    "node_api_post_finalizer",
]


def parse_def(path: Path) -> list[str]:
    """Parse a Windows-style .def file, returning the EXPORTS list."""
    if not path.exists():
        return []
    syms = []
    in_exports = False
    for line in path.read_text().splitlines():
        s = line.strip()
        if not s or s.startswith(";"):
            continue
        if s.upper().startswith("EXPORTS"):
            in_exports = True
            continue
        if not in_exports:
            continue
        if s.upper().startswith("NAME") or s.upper().startswith("LIBRARY"):
            in_exports = False
            continue
        # ignore '@N' ordinals / PRIVATE flags
        syms.append(s.split()[0])
    return syms


def load_napi_symbols() -> list[str]:
    js = parse_def(INCLUDE / "js_native_api.def")
    if not js:
        print(f"[warn] {INCLUDE / 'js_native_api.def'} missing or empty; "
              "run scripts/sync_napi_headers.py first")
    return js


def write_lds(syms: list[str], path: Path):
    body = "{\n  global:\n"
    body += "".join(f"    {s};\n" for s in syms)
    body += "  local:\n    *;\n};\n"
    path.write_text(body)


def write_exp(syms: list[str], path: Path):
    path.write_text("".join(f"_{s}\n" for s in syms))


def write_def(syms: list[str], path: Path):
    body = "EXPORTS\n" + "".join(f"  {s}\n" for s in syms)
    path.write_text(body)


def main():
    EXPORTS.mkdir(parents=True, exist_ok=True)
    syms = sorted(set(load_napi_symbols() + EMBEDDING_SYMBOLS))
    if not syms:
        print("[warn] no symbols collected; export files unchanged")
        return
    write_lds(syms, EXPORTS / "napi_v8.lds")
    write_exp(syms, EXPORTS / "napi_v8.exp")
    write_def(syms, EXPORTS / "napi_v8.def")
    print(f"[ok] generated {len(syms)} symbols -> {EXPORTS}/napi_v8.{{lds,exp,def}}")


if __name__ == "__main__":
    main()
