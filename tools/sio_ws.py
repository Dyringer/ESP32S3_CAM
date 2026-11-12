#!/usr/bin/env python3
"""
sio_ws.py - send smart_io control requests over the /io/ws WebSocket.

Author : Daniel Dyringer
Date   : 2026-11-01

Each request is a JSON object (SMART_IO2.md section 8.1); a missing "id" is
filled in. Every message received is printed, one JSON per line: responses
and the "update" messages pushed by the device. Binary stream frames
(SMART_IO2.md section 8.2) are decoded, one {"frame": ...} per line.

Stdlib only.

Usage:
    python3 tools/sio_ws.py --ip 192.168.50.134 '{"cmd":"hello"}'
    python3 tools/sio_ws.py --ip 192.168.50.134 --listen 2 \\
        '{"cmd":"config","gpio":4,"role":"output"}' '{"cmd":"write","gpio":4,"value":1}'
"""

import argparse
import base64
import json
import os
import socket
import struct
import sys
import time

DEFAULT_PORT = 80
WS_PATH = "/io/ws"
RESPONSE_TIMEOUT_S = 5.0

OPCODE_TEXT = 0x1
OPCODE_BINARY = 0x2
OPCODE_CLOSE = 0x8
OPCODE_PING = 0x9

FRAME_KIND_NAMES = ("digital", "series", "value", "bytes", "gap")
VARINT_MORE = 0x80


class WebSocketClient:
    """Minimal RFC 6455 client: masked sends, unfragmented receives."""

    def __init__(self, ip: str, port: int, path: str, timeout_s: float) -> None:
        self.socket = socket.create_connection((ip, port), timeout=timeout_s)
        self.buffer = b""
        key = base64.b64encode(os.urandom(16)).decode("ascii")
        request = (
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (path, ip, port, key)
        )
        self.socket.sendall(request.encode("ascii"))

        while b"\r\n\r\n" not in self.buffer:
            self.buffer += self._receive_some()

        header, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        if b" 101 " not in header.split(b"\r\n", 1)[0]:
            raise RuntimeError("WS handshake failed: %r" % header[:120])

    def _receive_some(self) -> bytes:
        data = self.socket.recv(4096)
        if not data:
            raise ConnectionError("connection closed by the device")
        return data

    def _receive_exact(self, length: int) -> bytes:
        while len(self.buffer) < length:
            self.buffer += self._receive_some()
        data, self.buffer = self.buffer[:length], self.buffer[length:]
        return data

    def send(self, opcode: int, payload: bytes) -> None:
        length = len(payload)
        if length < 126:
            header = struct.pack("!BB", 0x80 | opcode, 0x80 | length)
        elif length < 65536:
            header = struct.pack("!BBH", 0x80 | opcode, 0x80 | 126, length)
        else:
            header = struct.pack("!BBQ", 0x80 | opcode, 0x80 | 127, length)
        mask = os.urandom(4)
        masked = bytes(byte ^ mask[i % 4] for i, byte in enumerate(payload))
        self.socket.sendall(header + mask + masked)

    def receive(self, timeout_s: float) -> tuple[int, bytes]:
        self.socket.settimeout(timeout_s)
        first, second = self._receive_exact(2)
        opcode = first & 0x0F
        length = second & 0x7F
        if length == 126:
            (length,) = struct.unpack("!H", self._receive_exact(2))
        elif length == 127:
            (length,) = struct.unpack("!Q", self._receive_exact(8))
        payload = self._receive_exact(length)
        if opcode == OPCODE_PING:
            self.send(0xA, payload)
        return opcode, payload

    def close(self) -> None:
        try:
            self.send(OPCODE_CLOSE, b"")
        except OSError:
            pass
        self.socket.close()


class FrameReader:
    """Cursor over one binary message holding one or more frames."""

    def __init__(self, data: bytes) -> None:
        self.data = data
        self.offset = 0

    def done(self) -> bool:
        return self.offset >= len(self.data)

    def byte(self) -> int:
        value = self.data[self.offset]
        self.offset += 1
        return value

    def varint(self) -> int:
        value = 0
        shift = 0
        while True:
            current = self.byte()
            value |= (current & 0x7F) << shift
            shift += 7
            if (current & VARINT_MORE) == 0:
                return value

    def zigzag(self) -> int:
        value = self.varint()
        return (value >> 1) ^ -(value & 1)


def decode_block(reader: FrameReader) -> dict:
    block_id = reader.byte()
    kind = block_id >> 5
    block = {"kind": FRAME_KIND_NAMES[kind] if kind < len(FRAME_KIND_NAMES) else kind}
    if kind != 0:
        block["index"] = block_id & 0x1F
    if kind == 0:
        block["pins"] = reader.varint()
        block["levels"] = reader.varint()
        block["samples"] = [[reader.varint(), reader.varint()]
                            for _ in range(reader.varint())]
    elif kind == 1:
        block["dt_us"] = reader.zigzag()
        block["period_us"] = reader.varint()
        block["dv"] = [reader.zigzag() for _ in range(reader.varint())]
    elif kind == 2:
        block["samples"] = [[reader.zigzag(), reader.zigzag()]
                            for _ in range(reader.varint())]
    elif kind == 3:
        block["dt_us"] = reader.varint()
        count = reader.varint()
        block["data"] = reader.data[reader.offset:reader.offset + count].hex()
        reader.offset += count
    return block


def decode_frames(payload: bytes) -> list[dict]:
    reader = FrameReader(payload)
    frames = []
    while not reader.done():
        start_us = int.from_bytes(payload[reader.offset:reader.offset + 8], "little")
        reader.offset += 8
        duration_us = reader.varint()
        blocks = [decode_block(reader) for _ in range(reader.byte())]
        frames.append({"start_us": start_us, "duration_us": duration_us, "blocks": blocks})
    return frames


def print_message(opcode: int, payload: bytes) -> object:
    message = None
    if opcode == OPCODE_TEXT:
        text = payload.decode("utf-8", errors="replace")
        print(text, flush=True)
        try:
            message = json.loads(text)
        except json.JSONDecodeError:
            pass
    elif opcode == OPCODE_BINARY:
        try:
            for frame in decode_frames(payload):
                print(json.dumps({"frame": frame}, separators=(",", ":")), flush=True)
        except IndexError:
            print("<binary %d B, truncated frame>" % len(payload), flush=True)
    elif opcode == OPCODE_CLOSE:
        raise ConnectionError("device closed the WebSocket")
    return message


def run_request(client: WebSocketClient, request: dict) -> dict:
    """Sends request and prints messages until its response arrives."""
    client.send(OPCODE_TEXT, json.dumps(request, separators=(",", ":")).encode())
    deadline = time.monotonic() + RESPONSE_TIMEOUT_S
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError("no response to id %s" % request["id"])
        message = print_message(*client.receive(remaining))
        if isinstance(message, dict) and message.get("id") == request["id"]:
            return message


def listen(client: WebSocketClient, duration_s: float) -> None:
    deadline = time.monotonic() + duration_s
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            print_message(*client.receive(remaining))
        except socket.timeout:
            break


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    parser.add_argument("--ip", required=True)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument(
        "--listen",
        type=float,
        default=0.0,
        metavar="SECONDS",
        help="keep printing updates after the last response",
    )
    parser.add_argument("requests", nargs="*", help="JSON request objects")
    arguments = parser.parse_args()

    client = WebSocketClient(arguments.ip, arguments.port, WS_PATH, RESPONSE_TIMEOUT_S)
    failed = False
    try:
        for index, text in enumerate(arguments.requests, start=1):
            request = json.loads(text)
            request.setdefault("id", index)
            response = run_request(client, request)
            failed = failed or not response.get("ok", False)
        listen(client, arguments.listen)
    finally:
        client.close()
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
