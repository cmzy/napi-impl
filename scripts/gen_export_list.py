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

# Cross-engine embedding API (napi_v8/embedding.h) — every backend exports these.
# The napi_v8_ prefix on the tick is kept across engines so libnapi_<engine> is
# a drop-in ABI swap.
EMBEDDING_COMMON = [
    "napi_create_platform",
    "napi_destroy_platform",
    "napi_create_runtime",
    "napi_destroy_runtime",
    "napi_create_env",
    "napi_destroy_env",
    # Host-driven event-loop tick: pump foreground tasks + drain finalizers.
    "napi_v8_run_event_loop_tasks",
    # Interrupt JS execution on an env's isolate (runaway-worker teardown). Every
    # backend implements it with its own mechanism (V8 TerminateExecution / JSC
    # execution-time-limit / Hermes async-break / QuickJS interrupt handler).
    "napi_v8_terminate_execution",
    # Terminate-and-resume companions (embedding.h). cancel = clear a pending termination so the
    # isolate/context is reusable (V8 CancelTerminateExecution / JSC clears the watchdog terminate
    # flag → re-arm / Hermes best-effort); is_terminating reports whether a termination is pending.
    # Cross-engine like terminate itself (must be called on the owning thread, unlike terminate).
    "napi_v8_cancel_terminate_execution",
    "napi_v8_is_execution_terminating",
    # node_api_* extensions we implement beyond js_native_api proper.
    "node_api_post_finalizer",
    # Fast-call surface (napi/fast_call.h). Cross-engine: V8 takes the real fast
    # path, other backends export a slow-equivalent fallback, so the same
    # consumer source links everywhere.
    "napi_create_fast_function",
    "napi_create_fast_function_overloads",
    "napi_define_fast_accessor",
    "napi_fast_wrap",
    "napi_fast_unwrap",
    "napi_fast_value_unwrap",
    "napi_fast_value_is_nullish",
    "napi_fast_get_buffersource",
    "napi_fast_options_get_data",
    # Cross-engine compilation cache (napi/script_cache.h): real on V8 (code cache)
    # and Hermes (HBC); JSC/QuickJS default to compile+run. Every backend defines
    # it (real or via src/common/script_cache_fallback.cc), so it exports here.
    "napi_run_script_cached",
    "napi_free_script_cache",
]

# V8-only embedding extensions (inspector + SharedArrayBuffer); other engines do
# not implement these, so they are omitted from their export lists.
EMBEDDING_V8_ONLY = [
    "napi_v8_inspector_start",
    "napi_v8_inspector_stop",
    "napi_v8_inspector_wait_for_connection",
    # Inspector message loop (host-driven; napi_v8/inspector.h).
    "napi_v8_inspector_pump_messages",
    "napi_v8_inspector_is_paused",
    "napi_v8_inspector_wait",
    "napi_v8_inspector_set_pause_handler",
    "napi_v8_inspector_set_wake_handler",
    # Experimental core node-api calls implemented on v8 + jsc only (hermes/
    # quickjs do not provide them yet). SharedArrayBuffer info is read via the
    # standard napi_get_arraybuffer_info (extended to accept a SAB), so there is
    # no SAB-info symbol here.
    "node_api_create_sharedarraybuffer",
    "node_api_create_external_sharedarraybuffer",
    "node_api_is_sharedarraybuffer",
    "node_api_set_prototype",
    "node_api_create_object_with_properties",
]

# Structured-clone serialization (napi_v8/serialize.h) — V8 ValueSerializer-backed,
# V8 ONLY. Kept out of EMBEDDING_V8_ONLY because that list is shared with the jsc
# build, which does not implement these; exporting an unimplemented symbol there
# would break the jsc link.
EMBEDDING_V8_SERIALIZE = [
    "napi_v8_serialize",
    "napi_v8_deserialize",
    "napi_v8_free_serialized_data",
    # ArrayBuffer backing-store transfer (zero-copy). V8-only: JSC pins on
    # bytes-pointer access and Hermes ties AB memory to its runtime, so neither
    # can move a backing store to another isolate — see serialize.h.
    "napi_v8_take_backing_store",
    "napi_v8_adopt_backing_store",
    "napi_v8_free_backing_store",
]

# Back-compat alias (the V8 path used this single list).
EMBEDDING_SYMBOLS = EMBEDDING_COMMON + EMBEDDING_V8_ONLY + EMBEDDING_V8_SERIALIZE


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
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--engine", default="v8",
                    choices=["v8", "hermes", "jsc", "quickjs"],
                    help="emit napi_<engine>.{lds,exp,def}")
    args = ap.parse_args()

    # jsc implements the inspector + SharedArrayBuffer extensions (on JSC's own
    # RemoteInspector / SAB), so it exports the same set as v8; hermes/quickjs
    # do not. The ValueSerializer-backed serialize API is V8-only (jsc has no impl).
    if args.engine == "v8":
        embedding = EMBEDDING_COMMON + EMBEDDING_V8_ONLY + EMBEDDING_V8_SERIALIZE
    elif args.engine == "jsc":
        embedding = EMBEDDING_COMMON + EMBEDDING_V8_ONLY
    else:
        embedding = EMBEDDING_COMMON
    prefix = f"napi_{args.engine}"

    EXPORTS.mkdir(parents=True, exist_ok=True)
    syms = sorted(set(load_napi_symbols() + embedding))
    if not syms:
        print("[warn] no symbols collected; export files unchanged")
        return
    write_lds(syms, EXPORTS / f"{prefix}.lds")
    write_exp(syms, EXPORTS / f"{prefix}.exp")
    write_def(syms, EXPORTS / f"{prefix}.def")
    print(f"[ok] generated {len(syms)} symbols -> {EXPORTS}/{prefix}.{{lds,exp,def}}")


if __name__ == "__main__":
    main()
