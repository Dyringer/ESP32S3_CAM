#!/usr/bin/env python3
"""
fs_push.py - manage the DeskBuddy on-device file storage over WiFi.

Author : Daniel Dyringer
Date   : 2026-10-25

Brief  : Host-side client for the http_panel file manager API (/files/list,
         /files/upload, /files/download, /files/delete, /files/mkdir). Supports
         listing, free space, upload (single file or recursive directory),
         download, delete and mkdir. Stdlib only.

Usage:
    python3 tools/fs_push.py --ip 192.168.1.42 ls [remote_dir]
    python3 tools/fs_push.py --ip 192.168.1.42 df
    python3 tools/fs_push.py --ip 192.168.1.42 put photo.jpg [/photos/photo.jpg]
    python3 tools/fs_push.py --ip 192.168.1.42 put -r ./assets /assets
    python3 tools/fs_push.py --ip 192.168.1.42 get /photos/photo.jpg [local_path]
    python3 tools/fs_push.py --ip 192.168.1.42 rm /photos/photo.jpg
    python3 tools/fs_push.py --ip 192.168.1.42 mkdir /photos
"""

import argparse
import http.client
import json
import os
import posixpath
import sys
import time
import urllib.parse
from dataclasses import dataclass
from typing import Optional

DEFAULT_PORT = 80
REQUEST_TIMEOUT_S = 15
TRANSFER_TIMEOUT_S = 120
CHUNK_SIZE = 8192
HTTP_CONFLICT = 409
BUSY_HINT = "another transfer or OTA update may be running, or the target is a directory"


class DeviceError(Exception):
    """Raised when the device answers with a non-2xx status or is unreachable."""


@dataclass(frozen=True)
class Target:
    ip: str
    port: int


def parse_error_text(body: bytes) -> str:
    text = body.decode("utf-8", errors="replace").strip()
    message = text

    try:
        parsed = json.loads(text)

        if isinstance(parsed, dict) and ("error" in parsed):
            message = str(parsed["error"])
    except ValueError:
        pass

    return message


def raise_for_status(
    response: http.client.HTTPResponse, body: bytes, busy_hint: bool = False
) -> None:
    if not (200 <= response.status < 300):
        message = "HTTP %d: %s" % (response.status, parse_error_text(body))

        if busy_hint and (response.status == HTTP_CONFLICT):
            message += " (%s)" % BUSY_HINT

        raise DeviceError(message)


def normalise_remote_path(remote_path: str) -> str:
    stripped = remote_path.rstrip("/")

    return stripped if stripped != "" else "/"


def build_url(endpoint: str, remote_path: str) -> str:
    quoted = urllib.parse.quote(normalise_remote_path(remote_path), safe="/")

    return "%s?path=%s" % (endpoint, quoted)


def simple_request(
    target: Target, method: str, endpoint: str, remote_path: str
) -> bytes:
    connection = http.client.HTTPConnection(
        target.ip, target.port, timeout=REQUEST_TIMEOUT_S
    )

    try:
        connection.request(method, build_url(endpoint, remote_path))
        response = connection.getresponse()
        body = response.read()
        raise_for_status(response, body)
    except (OSError, http.client.HTTPException) as error:
        raise DeviceError("request failed: %s" % error) from error
    finally:
        connection.close()

    return body


def format_size(size: int) -> str:
    value = float(size)
    unit = "B"

    for next_unit in ("KB", "MB", "GB"):
        if value < 1024.0:
            break

        value /= 1024.0
        unit = next_unit

    text = "%d B" % size if unit == "B" else "%.1f %s" % (value, unit)

    return text


def print_progress(label: str, done: int, total: Optional[int], start_s: float) -> None:
    elapsed_s = max(time.monotonic() - start_s, 1e-6)
    rate_kbps = done / elapsed_s / 1024.0

    if (total is not None) and (total > 0):
        percent = int(100 * done / total)
        line = "\r  %s %3d%% (%d / %d bytes)  %7.1f KB/s" % (
            label,
            percent,
            done,
            total,
            rate_kbps,
        )
    else:
        line = "\r  %s %d bytes  %7.1f KB/s" % (label, done, rate_kbps)

    sys.stdout.write(line)
    sys.stdout.flush()


def list_directory(target: Target, remote_dir: str) -> dict:
    body = simple_request(target, "GET", "/files/list", remote_dir)

    try:
        listing = json.loads(body)
    except ValueError as error:
        raise DeviceError("invalid JSON in list response") from error

    return listing


def command_ls(target: Target, args: argparse.Namespace) -> None:
    listing = list_directory(target, args.remote_dir)
    entries = listing.get("entries", [])

    print("%-5s %12s  %s" % ("type", "size", "name"))

    for entry in entries:
        is_dir = bool(entry.get("dir", False))
        kind = "dir" if is_dir else "file"
        size = "-" if is_dir else str(entry.get("size", 0))
        name = str(entry.get("name", "")) + ("/" if is_dir else "")
        print("%-5s %12s  %s" % (kind, size, name))

    print(
        "%s: %d entries, %s used of %s"
        % (
            listing.get("path", args.remote_dir),
            len(entries),
            format_size(int(listing.get("used", 0))),
            format_size(int(listing.get("total", 0))),
        )
    )


def command_df(target: Target, args: argparse.Namespace) -> None:
    listing = list_directory(target, "/")
    total = int(listing.get("total", 0))
    used = int(listing.get("used", 0))
    free = max(total - used, 0)

    print("total: %s (%d bytes)" % (format_size(total), total))
    print("used : %s (%d bytes)" % (format_size(used), used))
    print("free : %s (%d bytes)" % (format_size(free), free))


def upload_file(target: Target, local_path: str, remote_path: str) -> None:
    size = os.path.getsize(local_path)
    connection = http.client.HTTPConnection(
        target.ip, target.port, timeout=TRANSFER_TIMEOUT_S
    )
    send_failure: Optional[OSError] = None

    print("%s -> %s (%d bytes)" % (local_path, remote_path, size))

    try:
        connection.putrequest("POST", build_url("/files/upload", remote_path))
        connection.putheader("Content-Type", "application/octet-stream")
        connection.putheader("Content-Length", str(size))
        connection.endheaders()

        sent = 0
        start_s = time.monotonic()

        try:
            with open(local_path, "rb") as source:
                chunk = source.read(CHUNK_SIZE)

                while len(chunk) > 0:
                    connection.send(chunk)
                    sent += len(chunk)
                    print_progress("upload", sent, size, start_s)
                    chunk = source.read(CHUNK_SIZE)
        except (BrokenPipeError, ConnectionResetError) as error:
            # The device may reject early (413, 409, ...) and close; its answer
            # is more useful than the broken pipe, so try to read it below.
            send_failure = error

        print()

        response = connection.getresponse()
        body = response.read()
        elapsed_s = max(time.monotonic() - start_s, 1e-6)
        raise_for_status(response, body, busy_hint=True)

        print("  done in %.1f s, %.1f KB/s" % (elapsed_s, size / elapsed_s / 1024.0))
    except (OSError, http.client.HTTPException) as error:
        detail = send_failure if send_failure is not None else error
        raise DeviceError("upload failed: %s" % detail) from error
    finally:
        connection.close()


def command_put(target: Target, args: argparse.Namespace) -> None:
    if args.recursive:
        put_directory(target, args.local_path, args.remote_path)
    else:
        if not os.path.isfile(args.local_path):
            raise DeviceError("not a file: %s" % args.local_path)

        remote_path = args.remote_path
        basename = os.path.basename(args.local_path)

        if remote_path is None:
            remote_path = "/" + basename
        elif remote_path.endswith("/"):
            remote_path = posixpath.join(normalise_remote_path(remote_path), basename)
        else:
            remote_path = normalise_remote_path(remote_path)

        upload_file(target, args.local_path, remote_path)


def make_directory(target: Target, remote_path: str, tolerate_existing: bool) -> None:
    try:
        simple_request(target, "POST", "/files/mkdir", remote_path)
    except DeviceError as error:
        if not (tolerate_existing and str(error).startswith("HTTP 409")):
            raise


def ensure_remote_directories(target: Target, remote_dir: str) -> None:
    current = ""

    for segment in [part for part in remote_dir.split("/") if part != ""]:
        current = current + "/" + segment
        make_directory(target, current, tolerate_existing=True)


def put_directory(target: Target, local_dir: str, remote_dir: Optional[str]) -> None:
    if not os.path.isdir(local_dir):
        raise DeviceError("not a directory: %s" % local_dir)

    if remote_dir is None:
        raise DeviceError("put -r needs a remote directory")

    remote_root = "/" + remote_dir.strip("/")
    file_count = 0

    ensure_remote_directories(target, remote_root)

    for current_dir, directory_names, file_names in os.walk(local_dir):
        relative = os.path.relpath(current_dir, local_dir)
        relative_parts = [] if relative == "." else relative.split(os.sep)
        remote_current = posixpath.join(remote_root, *relative_parts)

        visible_directories = []
        for name in sorted(directory_names):
            if name.startswith("."):
                hidden_path = os.path.join(current_dir, name)
                print("warning: skipping hidden directory %s" % hidden_path)
            else:
                visible_directories.append(name)

        directory_names[:] = visible_directories

        for name in visible_directories:
            make_directory(
                target, posixpath.join(remote_current, name), tolerate_existing=True
            )

        for name in sorted(file_names):
            local_file = os.path.join(current_dir, name)

            if name.startswith("."):
                print("warning: skipping hidden file %s (device rejects it)" % local_file)
            else:
                upload_file(target, local_file, posixpath.join(remote_current, name))
                file_count += 1

    print("uploaded %d files to %s" % (file_count, remote_root))


def command_get(target: Target, args: argparse.Namespace) -> None:
    remote_path = args.remote_path
    local_path = args.local_path

    if local_path is None:
        local_path = posixpath.basename(remote_path.rstrip("/"))

    if local_path == "":
        raise DeviceError("cannot derive a local file name; give local_path")

    temporary_path = local_path + ".part"
    connection = http.client.HTTPConnection(
        target.ip, target.port, timeout=TRANSFER_TIMEOUT_S
    )
    completed = False

    try:
        connection.request("GET", build_url("/files/download", remote_path))
        response = connection.getresponse()

        if not (200 <= response.status < 300):
            raise_for_status(response, response.read(), busy_hint=True)

        length_header = response.getheader("Content-Length")
        total = int(length_header) if length_header is not None else None
        received = 0
        start_s = time.monotonic()

        print("%s -> %s" % (remote_path, local_path))

        with open(temporary_path, "wb") as destination:
            chunk = response.read(CHUNK_SIZE)

            while len(chunk) > 0:
                destination.write(chunk)
                received += len(chunk)
                print_progress("download", received, total, start_s)
                chunk = response.read(CHUNK_SIZE)

        print()
        elapsed_s = max(time.monotonic() - start_s, 1e-6)
        os.replace(temporary_path, local_path)
        completed = True
        print(
            "  %d bytes in %.1f s, %.1f KB/s"
            % (received, elapsed_s, received / elapsed_s / 1024.0)
        )
    except (OSError, http.client.HTTPException) as error:
        raise DeviceError("download failed: %s" % error) from error
    finally:
        connection.close()

        if (not completed) and os.path.exists(temporary_path):
            os.remove(temporary_path)


def command_rm(target: Target, args: argparse.Namespace) -> None:
    simple_request(target, "POST", "/files/delete", args.remote_path)
    print("removed %s" % args.remote_path)


def command_mkdir(target: Target, args: argparse.Namespace) -> None:
    make_directory(target, args.remote_path, tolerate_existing=False)
    print("created %s" % args.remote_path)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Manage the DeskBuddy on-device file storage over WiFi."
    )
    parser.add_argument("--ip", required=True, help="Device IP address")
    parser.add_argument(
        "--port", type=int, default=DEFAULT_PORT, help="HTTP port (default 80)"
    )
    subparsers = parser.add_subparsers(dest="command", required=True)

    ls_parser = subparsers.add_parser("ls", help="List a remote directory")
    ls_parser.add_argument("remote_dir", nargs="?", default="/")
    ls_parser.set_defaults(handler=command_ls)

    df_parser = subparsers.add_parser("df", help="Show used / total / free space")
    df_parser.set_defaults(handler=command_df)

    put_parser = subparsers.add_parser("put", help="Upload a file or directory")
    put_parser.add_argument(
        "-r", "--recursive", action="store_true", help="Upload a directory tree"
    )
    put_parser.add_argument("local_path")
    put_parser.add_argument(
        "remote_path",
        nargs="?",
        default=None,
        help="Remote file path (default /<basename>); remote directory with -r",
    )
    put_parser.set_defaults(handler=command_put)

    get_parser = subparsers.add_parser("get", help="Download a file")
    get_parser.add_argument("remote_path")
    get_parser.add_argument("local_path", nargs="?", default=None)
    get_parser.set_defaults(handler=command_get)

    rm_parser = subparsers.add_parser("rm", help="Delete a file or empty directory")
    rm_parser.add_argument("remote_path")
    rm_parser.set_defaults(handler=command_rm)

    mkdir_parser = subparsers.add_parser("mkdir", help="Create a directory")
    mkdir_parser.add_argument("remote_path")
    mkdir_parser.set_defaults(handler=command_mkdir)

    return parser


def main() -> int:
    args = build_parser().parse_args()
    target = Target(ip=args.ip, port=args.port)
    result = 0

    try:
        args.handler(target, args)
    except DeviceError as error:
        print("error: %s" % error, file=sys.stderr)
        result = 1

    return result


if __name__ == "__main__":
    sys.exit(main())
