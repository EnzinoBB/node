#!/usr/bin/env python3
"""Robustness test client (scenario net-huge-frame): opens connections to a node's p2p TCP port and
sends on each one a packet header announcing a large data size, then checks how the node treats them
and that its memory stays flat.

The p2p packet header is 77 bytes, little-endian: type (1 byte), data_size (8 bytes), sender id
(32 bytes), receiver id (32 bytes), reserved (4 bytes). The first packet of a connection must be a
registration (its data: address type, an IPv4 address and a port, 7 bytes).

Limits of the p2p fork: a registration may announce at most 64 bytes, any later packet at most
256 MiB; above that the node closes the connection at once. Below the limit it reads the data in
chunks of 1 MiB, so the memory it holds follows the bytes actually received, not the size announced.

Usage: huge_frame.py HOST PORT [options]
  --size N        announced data size, repeatable (default 2^40)
  --registered    send a valid registration first, then the header (a direct packet)
  --expect close  the node must close each connection within --timeout seconds (default)
  --expect hold   the node must keep each connection open for --timeout seconds (it waits for the
                  data), then close it on its idle timeout (10 s) within --idle seconds
  --connections N connections per size, all held open together (default 4)
  --pid PID       the node's pid: its resident memory (Linux /proc) is sampled during the test and
                  may not grow by more than --max-growth MiB (default 64)
Exit status: 0 when the node behaved as expected (and its memory stayed flat), 1 otherwise.
"""
import argparse
import os
import select
import socket
import struct
import sys
import time

DIRECT = 0
REGISTRATION = 2


def header(packet_type, data_size, receiver=None):
    return struct.pack("<BQ32s32sI", packet_type, data_size, os.urandom(32), receiver or os.urandom(32), 0)


def registration():
    data = struct.pack("<B4sH", 0, bytes([127, 0, 0, 1]), 1)  # AddrType::v4, 127.0.0.1, port 1
    return header(REGISTRATION, len(data)) + data


def rss_kib(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


class Peak:
    def __init__(self, pid):
        self.pid = pid
        self.value = rss_kib(pid) if pid else None

    def sample(self):
        if self.pid:
            r = rss_kib(self.pid)
            if r is not None and (self.value is None or r > self.value):
                self.value = r


def wait_closed(socks, seconds, peak):
    """Reads from socks until each is closed by the node or `seconds` pass; returns the closed count."""
    closed = 0
    deadline = time.monotonic() + seconds
    while socks and time.monotonic() < deadline:
        readable, _, _ = select.select(socks, [], [], 0.1)
        for s in readable:
            try:
                done = s.recv(65536) == b""
            except (ConnectionResetError, BrokenPipeError):
                done = True
            except BlockingIOError:
                done = False
            if done:
                closed += 1
                socks.remove(s)
                s.close()
        peak.sample()
    return closed


def run(a, size, peak):
    socks, errors = [], 0
    for _ in range(a.connections):
        try:
            s = socket.create_connection((a.host, a.port), timeout=5)
            payload = header(REGISTRATION, size)
            if a.registered:
                payload = registration() + header(DIRECT, size)
            s.sendall(payload)
            s.setblocking(False)
            socks.append(s)
        except OSError as e:
            print(f"huge-frame: connect/send to {a.host}:{a.port}: {e}")
            errors += 1
    what = f"{len(socks)} connection(s) announcing {size} bytes{' after a registration' if a.registered else ''}"
    ok = errors == 0
    if a.expect == "close":
        closed = wait_closed(socks, a.timeout, peak)
        print(f"huge-frame: {a.host}:{a.port}: {what}: {closed} closed by the node within {a.timeout:g} s")
        ok = ok and closed == a.connections
    else:
        early = wait_closed(socks, a.timeout, peak)
        late = wait_closed(socks, a.idle, peak)
        print(f"huge-frame: {a.host}:{a.port}: {what}: {early} closed within {a.timeout:g} s (expected 0, "
              f"the node waits for the data), {late} closed on the idle timeout within {a.idle:g} s more")
        ok = ok and early == 0 and late == a.connections
    for s in socks:
        s.close()
    return ok


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--size", type=int, action="append")
    ap.add_argument("--registered", action="store_true")
    ap.add_argument("--expect", choices=("close", "hold"), default="close")
    ap.add_argument("--connections", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--idle", type=float, default=15.0)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--max-growth", type=int, default=64, help="MiB")
    a = ap.parse_args()

    peak = Peak(a.pid)
    before = peak.value
    ok = True
    for size in a.size or [1 << 40]:
        ok = run(a, size, peak) and ok
    if a.pid:
        if before is None or peak.value is None:
            print(f"huge-frame: no memory reading for pid {a.pid}")
            ok = False
        else:
            growth = (peak.value - before) / 1024
            print(f"huge-frame: node memory {before / 1024:.0f} MiB before, peak {peak.value / 1024:.0f} MiB "
                  f"during the test (+{growth:.0f} MiB, limit {a.max_growth} MiB)")
            ok = ok and growth <= a.max_growth
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
