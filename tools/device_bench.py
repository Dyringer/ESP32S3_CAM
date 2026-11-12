#!/usr/bin/env python3
"""
device_bench.py — benchmark a running DeskBuddy device over WiFi.

Author : Daniel Dyringer
Date   : 2026-10-25

PC-side tooling (not firmware). Stdlib only, no venv needed; runs on Linux (WSL) and
Windows. Measures latency, iperf TCP/UDP upload, MJPEG download, OTA write speed and
memory usage, then prints a summary.

The OTA test uploads 1 MB of synthetic data and aborts: it measures receive plus flash
erase/write like a real update, but never switches the boot partition or reboots. It
does overwrite part of the inactive OTA slot (the previous firmware).

Usage:
    python tools/device_bench.py 192.168.50.146
"""

import argparse
import json
import socket
import statistics
import struct
import sys
import threading
import time
import urllib.error
import urllib.request
from collections.abc import Callable
from dataclasses import dataclass, field
from typing import TypeVar

HTTP_PORT = 80
IPERF_PORT = 5001
HTTP_TIMEOUT_S = 5.0

PING_COUNT = 50
PING_INTERVAL_S = 0.05
PING_TIMEOUT_S = 1.0

TCP_TEST_SECONDS = 10.0
TCP_CHUNK_BYTES = 16384

UDP_RATES_MBPS = (5.0, 10.0, 20.0, 40.0)
UDP_TEST_SECONDS = 3.0
UDP_PAYLOAD_BYTES = 1460
UDP_LOSS_LIMIT_PERCENT = 1.0

STREAM_TEST_SECONDS = 10.0

OTA_PROBE_BYTES = 1024 * 1024
OTA_DECLARED_BYTES = 2 * 1024 * 1024
OTA_CHUNK_BYTES = 4096
# Small send buffer so sendall() is paced by the device, not by the PC's socket buffer.
OTA_SEND_BUFFER_BYTES = 16384
IMAGE_HEADER_MAGIC = 0xE9

StepResult = TypeVar("StepResult")


# ---- Results -------------------------------------------------------------------------


@dataclass
class MemorySnapshot:
    phase: str
    internal_free: int
    internal_min_free: int
    internal_largest: int
    psram_free: int
    cpu_percent: list[int]


@dataclass
class LatencyResult:
    method: str
    samples_ms: list[float | None]


@dataclass
class TcpResult:
    device_bytes: int
    seconds: float
    retransmissions: int | None
    cpu_percent: list[int]


@dataclass
class UdpStep:
    offered_mbps: float
    delivered_mbps: float
    loss_percent: float


@dataclass
class StreamResult:
    seconds: float
    total_bytes: int
    frame_sizes: list[int]
    frame_rates_bytes_per_s: list[float]
    cpu_percent: list[int]


@dataclass
class OtaResult:
    bytes_sent: int
    seconds: float


@dataclass
class BenchResults:
    firmware: str = "?"
    build: str = "?"
    link: str = "?"
    latency: LatencyResult | None = None
    tcp: TcpResult | None = None
    udp: list[UdpStep] = field(default_factory=list)
    stream: StreamResult | None = None
    ota: OtaResult | None = None
    memory: list[MemorySnapshot] = field(default_factory=list)
    errors: dict[str, str] = field(default_factory=dict)


# ---- Device HTTP API -----------------------------------------------------------------


def http_get_json(host: str, path: str) -> dict:
    url = f"http://{host}:{HTTP_PORT}{path}"
    with urllib.request.urlopen(url, timeout=HTTP_TIMEOUT_S) as response:
        return json.load(response)


def http_post(host: str, path: str, body: dict | None = None) -> None:
    data = json.dumps(body).encode() if body is not None else b""
    request = urllib.request.Request(
        f"http://{host}:{HTTP_PORT}{path}",
        data=data,
        method="POST",
        headers={"Content-Type": "application/json"},
    )
    with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT_S) as response:
        response.read()


def label_value(items: list[dict], label: str) -> str:
    for item in items:
        if item.get("label") == label:
            return str(item.get("value", "?"))
    return "?"


def read_memory(host: str, phase: str) -> MemorySnapshot:
    data = http_get_json(host, "/sysinfo/data")
    heaps = {heap["name"]: heap for heap in data.get("heaps", [])}
    internal = heaps.get("Internal SRAM", {})
    psram = heaps.get("PSRAM", {})
    return MemorySnapshot(
        phase=phase,
        internal_free=int(internal.get("free", 0)),
        internal_min_free=int(internal.get("min_free", 0)),
        internal_largest=int(internal.get("largest", 0)),
        psram_free=int(psram.get("free", 0)),
        cpu_percent=list(data.get("cpu", [])),
    )


def read_cpu(host: str) -> list[int]:
    return list(http_get_json(host, "/sysinfo/data").get("cpu", []))


def sample_later(
    delay_s: float, sampler: Callable[[], object], target: list
) -> threading.Timer:
    """Runs sampler() once after delay_s in the background and appends its result."""

    def run() -> None:
        try:
            target.append(sampler())
        except (OSError, ValueError):
            pass

    timer = threading.Timer(delay_s, run)
    timer.start()
    return timer


def iperf_stop(host: str) -> None:
    try:
        http_post(host, "/network/iperf/stop")
    except urllib.error.HTTPError:
        pass  # 409 when no test is running


def iperf_start_server(host: str, protocol: str, seconds: float) -> None:
    iperf_stop(host)
    http_post(
        host,
        "/network/iperf",
        {"mode": "server", "protocol": protocol, "port": IPERF_PORT,
         "time": int(seconds) + 5},
    )
    time.sleep(1.0)


def iperf_device_bytes(host: str) -> int:
    iperf = http_get_json(host, "/network/data").get("iperf", {})
    return int(iperf.get("total_bytes", 0))


# ---- Tests ---------------------------------------------------------------------------


def ping_icmp(host: str) -> list[float | None]:
    """Unprivileged ICMP echo (Linux/macOS datagram sockets). Raises OSError if absent."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_ICMP)
    sock.settimeout(PING_TIMEOUT_S)
    samples: list[float | None] = []
    try:
        for sequence in range(1, PING_COUNT + 1):
            packet = struct.pack("!BBHHH", 8, 0, 0, 0, sequence) + b"deskbuddy-bench!"
            sent_at = time.monotonic()
            sock.sendto(packet, (host, 0))
            latency_ms = None
            try:
                while latency_ms is None:
                    reply, _ = sock.recvfrom(1024)
                    if struct.unpack_from("!H", reply, 6)[0] == sequence:
                        latency_ms = (time.monotonic() - sent_at) * 1000.0
            except socket.timeout:
                pass
            samples.append(latency_ms)
            time.sleep(max(0.0, PING_INTERVAL_S - (time.monotonic() - sent_at)))
    finally:
        sock.close()
    return samples


def ping_tcp_connect(host: str) -> list[float | None]:
    """Fallback without ICMP: time of the TCP handshake to the HTTP port."""
    samples: list[float | None] = []
    for _ in range(PING_COUNT):
        started = time.monotonic()
        try:
            with socket.create_connection((host, HTTP_PORT), timeout=PING_TIMEOUT_S):
                samples.append((time.monotonic() - started) * 1000.0)
        except OSError:
            samples.append(None)
        time.sleep(max(0.0, PING_INTERVAL_S - (time.monotonic() - started)))
    return samples


def run_latency(host: str) -> LatencyResult:
    try:
        return LatencyResult("ICMP echo", ping_icmp(host))
    except OSError:
        return LatencyResult("TCP connect", ping_tcp_connect(host))


def tcp_retransmissions(sock: socket.socket) -> int | None:
    if not (sys.platform.startswith("linux") and hasattr(socket, "TCP_INFO")):
        return None
    info = sock.getsockopt(socket.IPPROTO_TCP, socket.TCP_INFO, 104)
    return struct.unpack_from("I", info, 100)[0]


def run_tcp_upload(host: str) -> TcpResult:
    iperf_start_server(host, "tcp", TCP_TEST_SECONDS)
    cpu_samples: list[list[int]] = []
    timer = sample_later(TCP_TEST_SECONDS / 2, lambda: read_cpu(host), cpu_samples)
    retransmissions = None
    chunk = bytes(TCP_CHUNK_BYTES)
    started = time.monotonic()
    try:
        with socket.create_connection((host, IPERF_PORT), timeout=10) as sock:
            while time.monotonic() - started < TCP_TEST_SECONDS:
                sock.sendall(chunk)
            retransmissions = tcp_retransmissions(sock)
        elapsed = time.monotonic() - started
        time.sleep(1.5)
        device_bytes = iperf_device_bytes(host)
    finally:
        timer.cancel()
        iperf_stop(host)
    return TcpResult(device_bytes, elapsed, retransmissions,
                     cpu_samples[0] if cpu_samples else [])


def send_udp_paced(host: str, rate_mbps: float, seconds: float) -> int:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    payload = bytes(UDP_PAYLOAD_BYTES)
    interval_s = UDP_PAYLOAD_BYTES * 8 / (rate_mbps * 1e6)
    started = time.monotonic()
    sent = 0
    index = 0
    try:
        while time.monotonic() - started < seconds:
            delay = started + index * interval_s - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            sock.sendto(payload, (host, IPERF_PORT))
            sent += UDP_PAYLOAD_BYTES
            index += 1
    finally:
        sock.close()
    return sent


def run_udp_upload(host: str) -> list[UdpStep]:
    steps: list[UdpStep] = []
    for rate in UDP_RATES_MBPS:
        iperf_start_server(host, "udp", UDP_TEST_SECONDS)
        try:
            sent = send_udp_paced(host, rate, UDP_TEST_SECONDS)
            time.sleep(1.5)
            received = iperf_device_bytes(host)
        finally:
            iperf_stop(host)
        time.sleep(1.0)
        loss = max(0.0, 100.0 * (1.0 - received / sent)) if sent else 100.0
        steps.append(UdpStep(rate, received * 8 / UDP_TEST_SECONDS / 1e6, loss))
    return steps


def read_stream(host: str, seconds: float) -> tuple[bytearray, list[tuple[int, float]]]:
    """Reads the MJPEG stream; returns the body and (end offset, arrival time) marks."""
    with socket.create_connection((host, HTTP_PORT), timeout=10) as sock:
        sock.sendall(f"GET /camera/stream HTTP/1.1\r\nHost: {host}\r\n\r\n".encode())
        received = bytearray()
        while b"\r\n\r\n" not in received:
            chunk = sock.recv(4096)
            if not chunk:
                raise ConnectionError("stream closed before headers")
            received += chunk
        header, _, body = bytes(received).partition(b"\r\n\r\n")
        status_line = header.split(b"\r\n", 1)[0].decode(errors="replace")
        if " 200 " not in status_line:
            raise ConnectionError(f"stream refused: {status_line}")
        buffer = bytearray(body)
        marks = [(len(buffer), time.monotonic())]
        started = time.monotonic()
        while time.monotonic() - started < seconds:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
            marks.append((len(buffer), time.monotonic()))
    return buffer, marks


def arrival_time(marks: list[tuple[int, float]], offset: int) -> float:
    for end, stamp in marks:
        if end > offset:
            return stamp
    return marks[-1][1]


def run_mjpeg(host: str) -> StreamResult:
    cpu_samples: list[list[int]] = []
    cpu_timer = sample_later(STREAM_TEST_SECONDS / 2, lambda: read_cpu(host), cpu_samples)
    try:
        buffer, marks = read_stream(host, STREAM_TEST_SECONDS)
    finally:
        cpu_timer.cancel()
    seconds = marks[-1][1] - marks[0][1]
    frames: list[tuple[int, int]] = []
    search_from = 0
    marker = b"Content-Length: "
    while True:
        start = buffer.find(marker, search_from)
        if start < 0:
            break
        line_end = buffer.find(b"\r\n", start)
        body_start = buffer.find(b"\r\n\r\n", start)
        if line_end < 0 or body_start < 0:
            break
        length = int(buffer[start + len(marker):line_end])
        body_start += 4
        if body_start + length > len(buffer):
            break  # last frame cut off by the end of the test
        frames.append((body_start, length))
        search_from = body_start + length
    rates = []
    for body_start, length in frames:
        transfer_s = arrival_time(marks, body_start + length - 1) - arrival_time(
            marks, body_start)
        if transfer_s > 0.001:
            rates.append(length / transfer_s)
    return StreamResult(
        seconds=seconds,
        total_bytes=len(buffer),
        frame_sizes=[length for _, length in frames],
        frame_rates_bytes_per_s=rates,
        cpu_percent=cpu_samples[0] if cpu_samples else [],
    )


def run_ota_write(host: str) -> OtaResult:
    payload = bytearray(OTA_CHUNK_BYTES)
    payload[0] = IMAGE_HEADER_MAGIC
    sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, OTA_SEND_BUFFER_BYTES)
    sock.settimeout(20)
    sent = 0
    try:
        sock.connect((host, HTTP_PORT))
        sock.sendall(
            (f"POST /ota/upload HTTP/1.1\r\nHost: {host}\r\n"
             f"Content-Type: application/octet-stream\r\n"
             f"Content-Length: {OTA_DECLARED_BYTES}\r\n\r\n").encode())
        started = time.monotonic()
        try:
            while sent < OTA_PROBE_BYTES:
                sock.sendall(payload)
                sent += len(payload)
                payload[0] = 0
        except OSError as error:
            raise ConnectionError(
                f"device stopped the upload after {sent // 1024} KB ({error}); "
                f"an update may be pending a reboot") from error
        elapsed = time.monotonic() - started
    finally:
        sock.close()  # incomplete body: the device aborts the update
    time.sleep(2.0)
    return OtaResult(sent, elapsed)


# ---- Report --------------------------------------------------------------------------


def kilobytes(value: int) -> str:
    return f"{value / 1024:.0f} KB"


def megabits(bytes_per_s: float) -> str:
    return f"{bytes_per_s * 8 / 1e6:.1f} Mbit/s"


def cpu_text(cpu: list[int]) -> str:
    return f"CPU {'/'.join(f'{load}%' for load in cpu)}" if cpu else ""


def summary_latency(result: LatencyResult) -> str:
    answered = sorted(sample for sample in result.samples_ms if sample is not None)
    lost = len(result.samples_ms) - len(answered)
    loss = 100.0 * lost / len(result.samples_ms)
    if not answered:
        return f"no replies ({result.method})"
    p90 = answered[min(len(answered) - 1, int(len(answered) * 0.9))]
    return (f"median {statistics.median(answered):.1f} ms, p90 {p90:.1f} ms, "
            f"max {answered[-1]:.0f} ms, loss {loss:.0f}%  "
            f"({len(result.samples_ms)} x {result.method})")


def summary_tcp(result: TcpResult) -> str:
    rate = result.device_bytes / result.seconds
    text = f"{megabits(rate)} ({rate / 1024:.0f} KB/s)"
    if result.retransmissions is not None:
        text += f", {result.retransmissions} retransmissions"
    return f"{text}  {cpu_text(result.cpu_percent)}".rstrip()


def summary_udp(steps: list[UdpStep]) -> list[str]:
    lines = [f"{step.offered_mbps:5.0f} Mbit/s offered -> {step.delivered_mbps:5.1f} "
             f"delivered, loss {step.loss_percent:4.1f}%" for step in steps]
    clean = [step.offered_mbps for step in steps
             if step.loss_percent < UDP_LOSS_LIMIT_PERCENT]
    peak = max(step.delivered_mbps for step in steps)
    verdict = (f"loss-free up to {max(clean):.0f} Mbit/s, " if clean else "") + \
        f"peak {peak:.1f} Mbit/s"
    return [verdict] + lines


def summary_stream(result: StreamResult) -> list[str]:
    frames = len(result.frame_sizes)
    if frames == 0:
        return ["no complete frames received"]
    average_rate = result.total_bytes / result.seconds if result.seconds else 0.0
    lines = [
        f"{frames} frames in {result.seconds:.1f} s = {frames / result.seconds:.1f} fps",
        f"frame size avg {kilobytes(int(statistics.mean(result.frame_sizes)))}, "
        f"stream average {megabits(average_rate)}  {cpu_text(result.cpu_percent)}"
        .rstrip(),
    ]
    if result.frame_rates_bytes_per_s:
        median_rate = statistics.median(result.frame_rates_bytes_per_s)
        lines.append(f"per-frame transfer median {megabits(median_rate)} "
                     f"({median_rate / 1024:.0f} KB/s) — link speed while sending")
    return lines


def print_summary(host: str, results: BenchResults) -> None:
    width = 90
    print()
    print("=" * width)
    print(f" DeskBuddy benchmark  {host}")
    print(f" Firmware {results.firmware}  ·  {results.build}")
    print(f" Link     {results.link}")
    print("=" * width)
    rows: list[tuple[str, list[str]]] = []
    if results.latency:
        rows.append(("Latency", [summary_latency(results.latency)]))
    if results.tcp:
        rows.append(("TCP upload", [summary_tcp(results.tcp)]))
    if results.udp:
        rows.append(("UDP upload", summary_udp(results.udp)))
    if results.stream:
        rows.append(("MJPEG download", summary_stream(results.stream)))
    if results.ota:
        rate = results.ota.bytes_sent / results.ota.seconds
        rows.append(("OTA write", [
            f"{rate / 1024:.1f} KB/s  ({kilobytes(results.ota.bytes_sent)} in "
            f"{results.ota.seconds:.1f} s, aborted, no reboot)"]))
    for name, error in results.errors.items():
        rows.append((name, [f"FAILED: {error}"]))
    for name, lines in rows:
        print(f" {name:<16}{lines[0]}")
        for line in lines[1:]:
            print(f" {'':<16}{line}")
    if results.memory:
        print("-" * width)
        print(f" {'Memory (internal)':<22}{'free':>10}{'min free*':>11}{'largest':>10}"
              f"{'PSRAM free':>13}")
        for snapshot in results.memory:
            print(f" {snapshot.phase:<22}{kilobytes(snapshot.internal_free):>10}"
                  f"{kilobytes(snapshot.internal_min_free):>11}"
                  f"{kilobytes(snapshot.internal_largest):>10}"
                  f"{snapshot.psram_free / 1024 / 1024:>10.2f} MB")
        print(" * lowest since boot")
    print("=" * width)


# ---- Main ----------------------------------------------------------------------------


def run_step(
    results: BenchResults, name: str, action: Callable[[], StepResult]
) -> StepResult | None:
    print(f"  {name} ...", flush=True)
    try:
        return action()
    except (OSError, ValueError, KeyError, ConnectionError) as error:
        results.errors[name] = str(error) or type(error).__name__
        return None


def remember_memory(results: BenchResults, host: str, phase: str) -> None:
    try:
        results.memory.append(read_memory(host, phase))
    except (OSError, ValueError):
        pass


def main() -> int:
    parser = argparse.ArgumentParser(description="Benchmark a DeskBuddy device.")
    parser.add_argument("ip", help="device IP address")
    host = parser.parse_args().ip
    results = BenchResults()
    try:
        ota_info = http_get_json(host, "/ota/info").get("info", [])
        link = http_get_json(host, "/network/data").get("link", [])
    except (OSError, ValueError) as error:
        print(f"Device {host} not reachable: {error}")
        return 1
    results.firmware = label_value(ota_info, "Firmware")
    results.build = label_value(ota_info, "Build")
    results.link = (f"RSSI {label_value(link, 'RSSI')}, channel "
                    f"{label_value(link, 'Channel')}, {label_value(link, 'PHY')}")
    print(f"Benchmarking {host} (about 2 minutes) ...")
    remember_memory(results, host, "start")
    results.latency = run_step(results, "Latency", lambda: run_latency(host))
    results.tcp = run_step(results, "TCP upload", lambda: run_tcp_upload(host))
    remember_memory(results, host, "after TCP upload")
    results.udp = run_step(results, "UDP upload", lambda: run_udp_upload(host)) or []
    remember_memory(results, host, "after UDP upload")
    results.stream = run_step(results, "MJPEG download", lambda: run_mjpeg(host))
    remember_memory(results, host, "after MJPEG download")
    results.ota = run_step(results, "OTA write", lambda: run_ota_write(host))
    remember_memory(results, host, "after OTA write")
    print_summary(host, results)
    return 0 if not results.errors else 2


if __name__ == "__main__":
    sys.exit(main())
