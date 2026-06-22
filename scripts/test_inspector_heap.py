#!/usr/bin/env python3
"""Verify a heap (memory) snapshot can be taken over the inspector.

DevTools' "memory dump" is HeapProfiler.takeHeapSnapshot: V8 streams the
snapshot back as many HeapProfiler.addHeapSnapshotChunk notifications, then
acks the request. This exercises the fixed message loop with the largest,
most fragmented CDP traffic there is:

  1. WS handshake.
  2. HeapProfiler.enable.
  3. HeapProfiler.takeHeapSnapshot {reportProgress: true}.
  4. Collect addHeapSnapshotChunk notifications until the request is acked.
  5. Concatenate the chunks, parse the JSON, assert it is a real snapshot
     (snapshot.meta + a non-empty node list).
  6. Runner exits cleanly.

Reuses the WebSocket helpers from test_inspector.py.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import secrets
import socket
import subprocess
import sys
import time
from pathlib import Path

from test_inspector import ws_accept_key, ws_send, ws_recv

ROOT = Path(__file__).resolve().parent.parent


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9231)
    ap.add_argument("--platform", default="linux")
    ap.add_argument("--arch", default="x64")
    ap.add_argument("--config", default="release")
    args = ap.parse_args()

    runner = (ROOT / "third_party" / "v8" / "out"
              / f"napi-{args.platform}-{args.arch}-{args.config}" / "runner")
    rel = "Release" if args.platform == "mac" else f"Release_{args.platform}"
    binding = (ROOT / "test" / "js-native-api" / "test_general" / "build"
               / rel / "test_general.so")
    test_js = ROOT / "test" / "js-native-api" / "test_general" / "testNapiRun.js"
    for p in (runner, binding, test_js):
        if not p.exists():
            sys.exit(f"missing: {p}")

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(runner.parent) + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    env["DYLD_LIBRARY_PATH"] = str(runner.parent) + os.pathsep + env.get("DYLD_LIBRARY_PATH", "")

    proc = subprocess.Popen(
        [str(runner), str(binding), "test_general", str(test_js),
         f"--inspect={args.port},wait"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    time.sleep(0.5)
    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    key = base64.b64encode(secrets.token_bytes(16)).decode()
    sock.sendall((
        f"GET /napi-v8 HTTP/1.1\r\nHost: 127.0.0.1:{args.port}\r\n"
        f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    ).encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    if "101" not in resp.split(b"\r\n", 1)[0].decode() or ws_accept_key(key) not in resp.decode():
        sys.exit(f"handshake failed:\n{resp.decode(errors='replace')}")

    ws_send(sock, json.dumps({"id": 1, "method": "HeapProfiler.enable", "params": {}}).encode())
    ws_send(sock, json.dumps({"id": 2, "method": "HeapProfiler.takeHeapSnapshot",
                              "params": {"reportProgress": True}}).encode())

    sock.settimeout(15)
    chunks = []
    progress_seen = False
    acked = False
    for _ in range(100000):
        try:
            op, payload = ws_recv(sock)
        except (socket.timeout, IOError):
            break
        if op != 0x1:
            continue
        msg = json.loads(payload)
        method = msg.get("method")
        if method == "HeapProfiler.addHeapSnapshotChunk":
            chunks.append(msg["params"]["chunk"])
        elif method == "HeapProfiler.reportHeapSnapshotProgress":
            progress_seen = True
        elif msg.get("id") == 2:
            acked = True
            break

    sock.close()
    try:
        rc = proc.wait(timeout=5)
    except subprocess.TimeoutExpired:
        proc.kill()
        sys.exit("runner did not exit after heap snapshot")
    if rc != 0:
        err = proc.stderr.read().decode(errors="replace") if proc.stderr else ""
        sys.stderr.write("---- runner stderr ----\n" + err + "\n----\n")
        sys.exit(f"runner exited non-zero ({rc}) — possible crash")

    if not acked:
        sys.exit("takeHeapSnapshot was never acknowledged")
    if not chunks:
        sys.exit("received no heap snapshot chunks")
    blob = "".join(chunks)
    try:
        snap = json.loads(blob)
    except json.JSONDecodeError as e:
        sys.exit(f"snapshot JSON did not parse ({len(blob)} bytes): {e}")
    meta = snap.get("snapshot", {}).get("meta")
    node_count = snap.get("snapshot", {}).get("node_count")
    if not meta or not node_count:
        sys.exit(f"snapshot missing meta/node_count: keys={list(snap.keys())}")
    print(f"[ok] heap snapshot: {len(chunks)} chunks, {len(blob)} bytes, "
          f"{node_count} nodes, progress={'yes' if progress_seen else 'no'}")


if __name__ == "__main__":
    main()
