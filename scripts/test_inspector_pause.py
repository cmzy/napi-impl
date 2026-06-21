#!/usr/bin/env python3
"""Regression test for the V8 inspector pause/resume message loop.

The pre-fix implementation dispatched CDP on the socket thread and its
runMessageLoopOnPause only slept (never pumped), so the first real pause
deadlocked or crashed. This drives a genuine pause and proves the loop now
runs on the V8 thread and resumes cleanly:

  1. WS handshake.
  2. Debugger.enable.
  3. Runtime.evaluate({expression: "debugger; 6*7"}) -> V8 pauses inside the
     evaluate and calls runMessageLoopOnPause on the V8 thread.
  4. Expect a Debugger.paused notification.
  5. Debugger.resume -> expect Debugger.resumed, then the evaluate result == 42.
  6. Runner exits cleanly (no hang / crash).

Triggering the pause via an evaluate of a `debugger;` expression is fully
client-driven, so it does not race the runner's own script execution.

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
    ap.add_argument("--port", type=int, default=9230)
    ap.add_argument("--platform", default="linux")
    ap.add_argument("--arch", default="x64")
    ap.add_argument("--config", default="release")
    args = ap.parse_args()

    runner = (ROOT / "third_party" / "v8" / "out"
              / f"napi-{args.platform}-{args.arch}-{args.config}" / "runner")
    binding = (ROOT / "test" / "js-native-api" / "test_general" / "build"
               / "Release" / "test_general.so")
    test_js = ROOT / "test" / "js-native-api" / "test_general" / "testNapiRun.js"

    for p in (runner, binding, test_js):
        if not p.exists():
            sys.exit(f"missing: {p}")

    env = os.environ.copy()
    env["LD_LIBRARY_PATH"] = str(runner.parent) + os.pathsep + env.get(
        "LD_LIBRARY_PATH", "")
    env["DYLD_LIBRARY_PATH"] = str(runner.parent) + os.pathsep + env.get(
        "DYLD_LIBRARY_PATH", "")

    proc = subprocess.Popen(
        [str(runner), str(binding), "test_general", str(test_js),
         f"--inspect={args.port},wait"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    time.sleep(0.5)
    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    key = base64.b64encode(secrets.token_bytes(16)).decode()
    sock.sendall((
        f"GET /napi-v8 HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{args.port}\r\n"
        f"Upgrade: websocket\r\nConnection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n\r\n"
    ).encode())
    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    if "101" not in resp.split(b"\r\n", 1)[0].decode():
        sys.exit(f"handshake failed:\n{resp.decode(errors='replace')}")
    if ws_accept_key(key) not in resp.decode():
        sys.exit("Sec-WebSocket-Accept mismatch")

    ws_send(sock, json.dumps(
        {"id": 1, "method": "Debugger.enable", "params": {}}).encode())
    ws_send(sock, json.dumps(
        {"id": 2, "method": "Runtime.evaluate",
         "params": {"expression": "debugger; 6*7", "returnByValue": True}}
    ).encode())

    sock.settimeout(8)
    saw_paused = False
    saw_resumed = False
    saw_result = False
    sent_resume = False
    for _ in range(80):
        try:
            op, payload = ws_recv(sock)
        except (socket.timeout, IOError):
            break
        if op != 0x1:
            continue
        msg = json.loads(payload)
        method = msg.get("method")
        if method == "Debugger.paused":
            saw_paused = True
            if not sent_resume:
                sent_resume = True
                ws_send(sock, json.dumps(
                    {"id": 3, "method": "Debugger.resume", "params": {}}).encode())
        elif method == "Debugger.resumed":
            saw_resumed = True
        elif msg.get("id") == 2:
            res = msg.get("result", {}).get("result", {})
            if res.get("value") == 42:
                saw_result = True
        if saw_paused and saw_resumed and saw_result:
            break

    sock.close()
    try:
        rc = proc.wait(timeout=5)  # must exit on its own (no deadlock)
    except subprocess.TimeoutExpired:
        proc.kill()
        sys.exit("runner did not exit — pause loop deadlocked")

    if not saw_paused:
        sys.exit("never received Debugger.paused (pause loop not entered)")
    if not saw_resumed:
        sys.exit("never received Debugger.resumed (resume not dispatched)")
    if not saw_result:
        sys.exit("never received evaluate result == 42 after resume")
    if rc != 0:
        sys.exit(f"runner exited non-zero ({rc}) — possible crash")
    print("[ok] inspector pause/resume: debugger; -> paused -> resume -> 42")


if __name__ == "__main__":
    main()
