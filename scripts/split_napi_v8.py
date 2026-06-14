#!/usr/bin/env python3
"""Split src/v8/js_native_api_v8.cc into per-feature category files.

The Node port is one big translation unit. To keep symmetry with our planned
per-category layout, we mechanically partition it:

  - src/v8/internals.cc   : everything in v8impl namespace + helper templates
                            (the first big block before any extern "C" napi_*)
  - src/v8/<cat>.cc       : the extern "C" napi_* / node_api_* functions
                            belonging to that category

Each per-category .cc starts with `#include "js_native_api_v8.h"` and uses the
v8impl helpers via inline definitions in the internals.cc.

Run once after touching js_native_api_v8.cc. Idempotent — safe to re-run.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SRC = ROOT / "src" / "v8" / "js_native_api_v8.cc"

# Map function name -> category (file basename without .cc).
CATEGORIES = {
    # Error / exception
    "napi_get_last_error_info": "error",
    "napi_fatal_error": "error",
    "napi_create_error": "error",
    "napi_create_type_error": "error",
    "napi_create_range_error": "error",
    "node_api_create_syntax_error": "error",
    "napi_throw": "error",
    "napi_throw_error": "error",
    "napi_throw_type_error": "error",
    "napi_throw_range_error": "error",
    "node_api_throw_syntax_error": "error",
    "napi_is_error": "error",
    "napi_is_exception_pending": "error",
    "napi_get_and_clear_last_exception": "error",

    # Values: singletons, numbers, typeof, coerce, strict_equals
    "napi_get_undefined": "value",
    "napi_get_null": "value",
    "napi_get_global": "value",
    "napi_get_boolean": "value",
    "napi_create_double": "value",
    "napi_create_int32": "value",
    "napi_create_uint32": "value",
    "napi_create_int64": "value",
    "napi_typeof": "value",
    "napi_get_value_double": "value",
    "napi_get_value_int32": "value",
    "napi_get_value_uint32": "value",
    "napi_get_value_int64": "value",
    "napi_get_value_bool": "value",
    "napi_strict_equals": "value",
    "napi_coerce_to_bool": "value",
    "napi_coerce_to_number": "value",
    "napi_coerce_to_object": "value",
    "napi_coerce_to_string": "value",

    # Strings
    "napi_create_string_latin1": "string",
    "napi_create_string_utf8": "string",
    "napi_create_string_utf16": "string",
    "node_api_create_external_string_latin1": "string",
    "node_api_create_external_string_utf16": "string",
    "node_api_create_property_key_latin1": "string",
    "node_api_create_property_key_utf8": "string",
    "node_api_create_property_key_utf16": "string",
    "napi_get_value_string_latin1": "string",
    "napi_get_value_string_utf8": "string",
    "napi_get_value_string_utf16": "string",

    # Symbol
    "napi_create_symbol": "symbol",
    "node_api_symbol_for": "symbol",

    # Object
    "napi_create_object": "object",
    "napi_get_prototype": "object",
    "napi_get_property_names": "object",
    "napi_set_property": "object",
    "napi_has_property": "object",
    "napi_get_property": "object",
    "napi_delete_property": "object",
    "napi_has_own_property": "object",
    "napi_set_named_property": "object",
    "napi_has_named_property": "object",
    "napi_get_named_property": "object",
    "napi_set_element": "object",
    "napi_has_element": "object",
    "napi_get_element": "object",
    "napi_delete_element": "object",
    "napi_define_properties": "object",
    "napi_get_all_property_names": "object",
    "napi_object_freeze": "object",
    "napi_object_seal": "object",

    # Array
    "napi_create_array": "array",
    "napi_create_array_with_length": "array",
    "napi_is_array": "array",
    "napi_get_array_length": "array",

    # Function
    "napi_create_function": "function",
    "napi_call_function": "function",
    "napi_new_instance": "function",
    "napi_instanceof": "function",
    "napi_get_cb_info": "function",
    "napi_get_new_target": "function",
    "napi_define_class": "function",

    # Wrap / external / type tag
    "napi_wrap": "wrap",
    "napi_unwrap": "wrap",
    "napi_remove_wrap": "wrap",
    "napi_create_external": "wrap",
    "napi_get_value_external": "wrap",
    "napi_add_finalizer": "wrap",
    "napi_type_tag_object": "wrap",
    "napi_check_object_type_tag": "wrap",
    "node_api_post_finalizer": "wrap",

    # Reference
    "napi_create_reference": "reference",
    "napi_delete_reference": "reference",
    "napi_reference_ref": "reference",
    "napi_reference_unref": "reference",
    "napi_get_reference_value": "reference",

    # Scopes
    "napi_open_handle_scope": "scope",
    "napi_close_handle_scope": "scope",
    "napi_open_escapable_handle_scope": "scope",
    "napi_close_escapable_handle_scope": "scope",
    "napi_escape_handle": "scope",

    # ArrayBuffer / TypedArray / DataView
    "napi_is_arraybuffer": "arraybuffer",
    "napi_create_arraybuffer": "arraybuffer",
    "napi_create_external_arraybuffer": "arraybuffer",
    "napi_get_arraybuffer_info": "arraybuffer",
    "napi_detach_arraybuffer": "arraybuffer",
    "napi_is_detached_arraybuffer": "arraybuffer",
    "napi_is_typedarray": "arraybuffer",
    "napi_create_typedarray": "arraybuffer",
    "napi_get_typedarray_info": "arraybuffer",
    "napi_create_dataview": "arraybuffer",
    "napi_is_dataview": "arraybuffer",
    "napi_get_dataview_info": "arraybuffer",

    # Promise
    "napi_create_promise": "promise",
    "napi_resolve_deferred": "promise",
    "napi_reject_deferred": "promise",
    "napi_is_promise": "promise",

    # Script
    "napi_run_script": "script",

    # Date
    "napi_create_date": "date",
    "napi_is_date": "date",
    "napi_get_date_value": "date",

    # BigInt
    "napi_create_bigint_int64": "bigint",
    "napi_create_bigint_uint64": "bigint",
    "napi_create_bigint_words": "bigint",
    "napi_get_value_bigint_int64": "bigint",
    "napi_get_value_bigint_uint64": "bigint",
    "napi_get_value_bigint_words": "bigint",

    # Instance data + version + memory
    "napi_set_instance_data": "instance",
    "napi_get_instance_data": "instance",
    "napi_get_version": "instance",
    "napi_adjust_external_memory": "instance",
}


def split():
    text = SRC.read_text()
    lines = text.splitlines(keepends=True)

    # Find boundaries: each function def starts with one of these signatures.
    # `napi_status` at line start, followed by `NAPI_CDECL? <name>(` or
    # `NAPI_CDECL\n<name>(` (multi-line).
    func_starts = []
    pat = re.compile(
        r"^napi_status\s+(?:NAPI_CDECL\s+)?(napi_[a-z0-9_]+|node_api_[a-z0-9_]+)\s*\("
    )
    # Also catch the two-line variant:  `napi_status NAPI_CDECL\n<name>(`
    two_line = re.compile(r"^napi_status\s+NAPI_CDECL\s*$")
    cont = re.compile(r"^(napi_[a-z0-9_]+|node_api_[a-z0-9_]+)\s*\(")

    for i, line in enumerate(lines):
        m = pat.match(line)
        if m:
            func_starts.append((i, m.group(1)))
            continue
        if two_line.match(line) and i + 1 < len(lines):
            m2 = cont.match(lines[i + 1])
            if m2:
                func_starts.append((i, m2.group(1)))

    if not func_starts:
        sys.exit("no extern-C napi_* function boundaries found")

    # Find the function-end by matching braces from the function-start.
    def find_end(start: int) -> int:
        depth = 0
        opened = False
        for j in range(start, len(lines)):
            for ch in lines[j]:
                if ch == "{":
                    depth += 1
                    opened = True
                elif ch == "}":
                    depth -= 1
                    if opened and depth == 0:
                        return j  # inclusive end line index
        return len(lines) - 1

    # The internals block = everything before the first extern-C napi_*.
    first = func_starts[0][0]
    internals_lines = lines[:first]
    internals = "".join(internals_lines)

    # Collect per-category bodies.
    bodies: dict[str, list[str]] = {}
    unknown: list[str] = []

    for idx, (start, name) in enumerate(func_starts):
        end = find_end(start)
        body = "".join(lines[start:end + 1])
        cat = CATEGORIES.get(name)
        if cat is None:
            unknown.append(name)
            cat = "misc"
        bodies.setdefault(cat, []).append(body)

    if unknown:
        print(f"[warn] unmapped functions sent to misc.cc: {unknown}",
              file=sys.stderr)

    # internals.cc is hand-maintained (v8impl class methods + env method);
    # the splitter just emits the per-category extern "C" files.

    # Write each category file.
    out_dir = ROOT / "src" / "v8"
    category_header = (
        "// Auto-generated by scripts/split_napi_v8.py.\n"
        "// Per-category extern \"C\" entry points; helpers in internals.cc.\n\n"
        "#include <algorithm>\n"
        "#include <climits>\n"
        "#include <cmath>\n"
        "#define NAPI_EXPERIMENTAL\n"
        "#include \"napi/js_native_api.h\"\n"
        "#include \"napi/node_api.h\"\n"
        "#include \"js_native_api_v8.h\"\n"
        "#include \"js_native_api_v8_impl.h\"\n\n"
        "#define CHECK_MAYBE_NOTHING(env, maybe, status)                                \\\n"
        "  RETURN_STATUS_IF_FALSE((env), !((maybe).IsNothing()), (status))\n\n"
        "#define CHECK_MAYBE_NOTHING_WITH_PREAMBLE(env, maybe, status)                  \\\n"
        "  RETURN_STATUS_IF_FALSE_WITH_PREAMBLE((env), !((maybe).IsNothing()), (status))\n\n"
        "#define CHECK_TO_NUMBER(env, context, result, src)                             \\\n"
        "  CHECK_TO_TYPE((env), Number, (context), (result), (src), napi_number_expected)\n\n"
        "#define CHECK_NEW_FROM_UTF8_LEN(env, result, str, len)                         \\\n"
        "  do {                                                                         \\\n"
        "    static_assert(static_cast<int>(NAPI_AUTO_LENGTH) == -1,                    \\\n"
        "                  \"Casting NAPI_AUTO_LENGTH to int must result in -1\");        \\\n"
        "    RETURN_STATUS_IF_FALSE(                                                    \\\n"
        "        (env), (len == NAPI_AUTO_LENGTH) || len <= INT_MAX, napi_invalid_arg); \\\n"
        "    RETURN_STATUS_IF_FALSE((env), (str) != nullptr, napi_invalid_arg);         \\\n"
        "    auto str_maybe = v8::String::NewFromUtf8((env)->isolate,                   \\\n"
        "                                             (str),                            \\\n"
        "                                             v8::NewStringType::kInternalized, \\\n"
        "                                             static_cast<int>(len));           \\\n"
        "    CHECK_MAYBE_EMPTY((env), str_maybe, napi_generic_failure);                 \\\n"
        "    (result) = str_maybe.ToLocalChecked();                                     \\\n"
        "  } while (0)\n\n"
        "#define CHECK_NEW_FROM_UTF8(env, result, str)                                  \\\n"
        "  CHECK_NEW_FROM_UTF8_LEN((env), (result), (str), NAPI_AUTO_LENGTH)\n\n"
        "#define CREATE_TYPED_ARRAY(                                                    \\\n"
        "    env, type, size_of_element, buffer, byte_offset, length, out)              \\\n"
        "  do {                                                                         \\\n"
        "    if ((size_of_element) > 1) {                                               \\\n"
        "      THROW_RANGE_ERROR_IF_FALSE(                                              \\\n"
        "          (env),                                                               \\\n"
        "          (byte_offset) % (size_of_element) == 0,                              \\\n"
        "          \"ERR_NAPI_INVALID_TYPEDARRAY_ALIGNMENT\",                             \\\n"
        "          \"start offset of \" #type                                             \\\n"
        "          \" should be a multiple of \" #size_of_element);                       \\\n"
        "    }                                                                          \\\n"
        "    THROW_RANGE_ERROR_IF_FALSE(                                                \\\n"
        "        (env),                                                                 \\\n"
        "        (length) * (size_of_element) + (byte_offset) <= buffer->ByteLength(),  \\\n"
        "        \"ERR_NAPI_INVALID_TYPEDARRAY_LENGTH\",                                  \\\n"
        "        \"Invalid typed array length\");                                         \\\n"
        "    (out) = v8::type::New((buffer), (byte_offset), (length));                  \\\n"
        "  } while (0)\n\n"
    )

    files_written = ["internals.cc"]
    for cat, funcs in sorted(bodies.items()):
        path = out_dir / f"{cat}.cc"
        path.write_text(category_header + "\n\n".join(funcs) + "\n")
        files_written.append(f"{cat}.cc")

    # Remove the original monolithic file (split now covers it).
    SRC.unlink()

    # Update sources.txt.
    (out_dir / "sources.txt").write_text(
        "napi_v8_engine.cc\n" + "\n".join(sorted(files_written)) + "\n"
    )

    print(f"[ok] split into {len(files_written)} files")
    for f in sorted(files_written):
        size = (out_dir / f).stat().st_size
        print(f"  {f:<20}  {size:>7} B")


if __name__ == "__main__":
    split()
