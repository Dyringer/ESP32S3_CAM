#!/usr/bin/env python3
"""
ota_push.py — push a firmware .bin to a running ESP32-S3 CAM over WiFi.

Author : Daniel Dyringer
Date   : 2026-08-13

PC-side tooling (not firmware).
Uploads the raw image body to POST /ota/upload on the device's http_panel,
then (unless --no-reboot) triggers a reboot via the /ota/ws WebSocket action
used by the OTA web page, and polls GET / a few times to confirm the device
comes back up.

Stdlib only — no requests, no third-party deps.
Uses http.client for the upload (streaming from disk, one Content-Length
header, no multipart — same raw-body contract as ota_page.html's XHR
upload) and a minimal handshake-only WebSocket client for the reboot action,
since installing a WS library just to send one JSON frame isn't worth the
dependency.

Usage:
    python tools/ota_push.py --ip 192.168.1.42 --file build/esp32s3_cam.bin
    python tools/ota_push.py --ip 192.168.1.42 --file build/esp32s3_cam.bin --no-reboot
"""

import argparse
import base64
import http.client
import os
import socket
import sys
import time

DEFAULT_PORT = 80
UPLOAD_TIMEOUT_S = 60
POLL_ATTEMPTS = 10
POLL_DELAY_S = 1.5


def upload(ip, port, path):
    size = os.path.getsize(path)
    conn = http.client.HTTPConnection(ip, port, timeout=UPLOAD_TIMEOUT_S)

    print("Uploading %s (%d bytes) to %s:%d%s ..." % (path, size, ip, port, "/ota/upload"))

    conn.putrequest("POST", "/ota/upload")
    conn.putheader("Content-Type", "application/octet-stream")
    conn.putheader("Content-Length", str(size))
    conn.endheaders()

    sent = 0
    chunk_size = 4096
    last_pct = -1

    with open(path, "rb") as f:
        while True:
            chunk = f.read(chunk_size)
            if not chunk:
                break
            conn.send(chunk)
            sent += len(chunk)
            pct = int(100 * sent / size)
            if pct != last_pct:
                sys.stdout.write("\r  %3d%% (%d / %d bytes)" % (pct, sent, size))
                sys.stdout.flush()
                last_pct = pct

    print()

    resp = conn.getresponse()
    body = resp.read().decode("utf-8", errors="replace")
    conn.close()

    if resp.status != 200:
        print("Upload failed: HTTP %d — %s" % (resp.status, body))
        return False

    print("Upload OK: %s" % body)
    return True


def _ws_handshake_and_send_reboot(ip, port, timeout_s=5.0):
    """Minimal RFC 6455 client: connect, upgrade, send one text frame
    ({"action":"reboot"}), then close. No frame parsing needed — the OTA
    endpoint doesn't send a reply we need to wait for."""
    key = base64.b64encode(os.urandom(16)).decode("ascii")
    sock = socket.create_connection((ip, port), timeout=timeout_s)
    sock.settimeout(timeout_s)

    try:
        request = (
            "GET /ota/ws HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n" % (ip, port, key)
        )
        sock.sendall(request.encode("ascii"))

        response = sock.recv(4096)
        if b"101" not in response.split(b"\r\n", 1)[0]:
            raise RuntimeError("WS handshake failed: %r" % response[:120])

        payload = b'{"action":"reboot"}'
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))

        # FIN=1, opcode=1 (text); MASK=1, len<126 (payload is well under that)
        frame = bytes([0x81, 0x80 | len(payload)]) + mask + masked
        sock.sendall(frame)
    finally:
        sock.close()


def trigger_reboot(ip, port):
    print("Triggering reboot ...")
    try:
        _ws_handshake_and_send_reboot(ip, port)
    except OSError as e:
        # The device may drop the connection immediately as it reboots —
        # that's the expected outcome, not a failure of this step.
        print("  (connection dropped during reboot trigger: %s — expected)" % e)


def poll_alive(ip, port):
    print("Waiting for device to come back up ...")
    for attempt in range(1, POLL_ATTEMPTS + 1):
        time.sleep(POLL_DELAY_S)
        try:
            conn = http.client.HTTPConnection(ip, port, timeout=3)
            conn.request("GET", "/")
            resp = conn.getresponse()
            resp.read()
            conn.close()
            if resp.status == 200:
                print("  Device is back up (attempt %d/%d)." % (attempt, POLL_ATTEMPTS))
                return True
        except OSError:
            pass
        sys.stdout.write(".")
        sys.stdout.flush()

    print()
    print("  Device did not respond after %d attempts." % POLL_ATTEMPTS)
    return False


def main():
    parser = argparse.ArgumentParser(description="Push a firmware .bin to ESP32-S3 CAM over WiFi.")
    parser.add_argument("--ip", required=True, help="Device IP address")
    parser.add_argument("--file", required=True, help="Path to the firmware .bin")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT, help="HTTP port (default 80)")
    parser.add_argument("--no-reboot", action="store_true",
                         help="Skip the post-upload reboot trigger; leave the update staged")
    args = parser.parse_args()

    if not os.path.isfile(args.file):
        print("File not found: %s" % args.file)
        return 1

    if not upload(args.ip, args.port, args.file):
        return 1

    if args.no_reboot:
        print("Update staged — reboot manually via the web page or console when ready.")
        return 0

    trigger_reboot(args.ip, args.port)

    if not poll_alive(args.ip, args.port):
        return 1

    return 0


if __name__ == "__main__":
    sys.exit(main())
