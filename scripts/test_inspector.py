#!/usr/bin/env python3
"""Smoke-test the V8 inspector wired into the runner.

Spawns the runner with --inspect=<port>,wait so it blocks until the
WebSocket connects, then drives a minimal CDP session:
  1. Connect WS to ws://127.0.0.1:<port>/anything
  2. Send Runtime.evaluate({expression: "1+2"})
  3. Assert the result is {value:3,type:'number'}

Uses only the standard library — base64+hashlib for the WS handshake and
struct for frame encoding.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import os
import secrets
import socket
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def ws_accept_key(client_key: str) -> str:
    h = hashlib.sha1((client_key + GUID).encode()).digest()
    return base64.b64encode(h).decode()


def ws_send(sock: socket.socket, payload: bytes, op: int = 0x1):
    # Client must mask.
    fin = 0x80
    head = bytes([fin | op])
    mask = secrets.token_bytes(4)
    n = len(payload)
    if n < 126:
        head += bytes([0x80 | n])
    elif n <= 0xFFFF:
        head += bytes([0x80 | 126]) + struct.pack(">H", n)
    else:
        head += bytes([0x80 | 127]) + struct.pack(">Q", n)
    head += mask
    masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    sock.sendall(head + masked)


def recv_n(sock: socket.socket, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise IOError("connection closed")
        buf += chunk
    return buf


def ws_recv(sock: socket.socket) -> tuple[int, bytes]:
    h = recv_n(sock, 2)
    op = h[0] & 0x0F
    masked = h[1] & 0x80
    n = h[1] & 0x7F
    if n == 126:
        n = struct.unpack(">H", recv_n(sock, 2))[0]
    elif n == 127:
        n = struct.unpack(">Q", recv_n(sock, 8))[0]
    if masked:
        mask = recv_n(sock, 4)
    payload = recv_n(sock, n)
    if masked:
        payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
    return op, payload


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9229)
    ap.add_argument("--platform", default="mac")
    ap.add_argument("--arch", default="x86_64")
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
    env["DYLD_LIBRARY_PATH"] = str(runner.parent) + os.pathsep + env.get(
        "DYLD_LIBRARY_PATH", "")

    proc = subprocess.Popen(
        [str(runner), str(binding), "test_general", str(test_js),
         f"--inspect={args.port},wait"],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    # Give the listener a moment to bind.
    time.sleep(0.5)

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5)
    key = base64.b64encode(secrets.token_bytes(16)).decode()
    req = (
        f"GET /napi-v8 HTTP/1.1\r\n"
        f"Host: 127.0.0.1:{args.port}\r\n"
        f"Upgrade: websocket\r\n"
        f"Connection: Upgrade\r\n"
        f"Sec-WebSocket-Key: {key}\r\n"
        f"Sec-WebSocket-Version: 13\r\n\r\n"
    )
    sock.sendall(req.encode())

    resp = b""
    while b"\r\n\r\n" not in resp:
        resp += sock.recv(4096)
    lines = resp.split(b"\r\n\r\n", 1)[0].decode().split("\r\n")
    if "101" not in lines[0]:
        sys.exit(f"handshake failed:\n{resp.decode(errors='replace')}")
    expected_accept = ws_accept_key(key)
    if expected_accept not in resp.decode():
        sys.exit("Sec-WebSocket-Accept mismatch")

    # Send Runtime.enable (some V8 versions need it) then Runtime.evaluate.
    ws_send(sock, json.dumps({
        "id": 1, "method": "Runtime.enable", "params": {}
    }).encode())
    ws_send(sock, json.dumps({
        "id": 2, "method": "Runtime.evaluate",
        "params": {"expression": "1+2"}
    }).encode())

    sock.settimeout(5)
    saw_result = False
    for _ in range(20):
        try:
            op, payload = ws_recv(sock)
        except (socket.timeout, IOError):
            break
        if op != 0x1:
            continue
        msg = json.loads(payload)
        if msg.get("id") == 2:
            res = msg.get("result", {}).get("result", {})
            if res.get("value") == 3 and res.get("type") == "number":
                saw_result = True
                break
            sys.exit(f"unexpected Runtime.evaluate result: {msg}")

    sock.close()
    proc.terminate()
    try:
        proc.wait(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()

    if not saw_result:
        sys.exit("did not receive Runtime.evaluate result")
    print("[ok] inspector Runtime.evaluate('1+2') = 3")


if __name__ == "__main__":
    main()
