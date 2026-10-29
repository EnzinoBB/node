#!/usr/bin/env python3
"""Robustness test client (scenario net-huge-frame): opens connections to a node's p2p TCP port and
sends on each one packet header announcing a data size above the p2p frame limit, then checks that
the node closes every connection and that its memory stays flat.

The p2p packet header is 77 bytes, little-endian: type (1 byte), data_size (8 bytes), sender id
(32 bytes), receiver id (32 bytes), reserved (4 bytes). A node with the frame limit (256 MiB)
closes the connection at once, before reading or allocating anything for the data.

Usage: huge_frame.py HOST PORT [--size N ...] [--connections N] [--timeout S] [--pid PID]
  --size         announced data size, repeatable (default 2^40)
  --connections  connections per size, all held open together (default 4)
  --timeout      seconds the node has to close them (default 5; the p2p idle timeout is 10)
  --pid          the node's pid: its resident memory (Linux /proc) is sampled during the test and
                 may not grow by more than --max-growth MiB (default 64)
Exit status: 0 when the node closed every connection in time (and its memory stayed flat), 1 otherwise.
"""
import argparse
import os
import select
import socket
import struct
import sys
import time

REGISTRATION = 2  # the first packet of a connection must be a registration


def header(data_size):
    return struct.pack("<BQ32s32sI", REGISTRATION, data_size, os.urandom(32), os.urandom(32), 0)


def rss_kib(pid):
    try:
        with open(f"/proc/{pid}/status") as f:
            for line in f:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


def run(host, port, size, count, timeout, pid):
    """Returns (closed, open_after_timeout, errors, max_rss_kib)."""
    socks, errors = [], 0
    for _ in range(count):
        try:
            s = socket.create_connection((host, port), timeout=timeout)
            s.sendall(header(size))
            s.setblocking(False)
            socks.append(s)
        except OSError as e:
            print(f"huge-frame: connect/send to {host}:{port}: {e}")
            errors += 1
    closed, max_rss = 0, rss_kib(pid) if pid else None
    deadline = time.monotonic() + timeout
    while socks and time.monotonic() < deadline:
        readable, _, _ = select.select(socks, [], [], 0.1)
        for s in readable:
            try:
                done = s.recv(4096) == b""
            except (ConnectionResetError, BrokenPipeError):
                done = True
            except BlockingIOError:
                done = False
            if done:
                closed += 1
                socks.remove(s)
                s.close()
        if pid:
            r = rss_kib(pid)
            if r is not None and (max_rss is None or r > max_rss):
                max_rss = r
    still_open = len(socks)
    for s in socks:
        s.close()
    return closed, still_open, errors, max_rss


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("host")
    ap.add_argument("port", type=int)
    ap.add_argument("--size", type=int, action="append")
    ap.add_argument("--connections", type=int, default=4)
    ap.add_argument("--timeout", type=float, default=5.0)
    ap.add_argument("--pid", type=int)
    ap.add_argument("--max-growth", type=int, default=64, help="MiB")
    a = ap.parse_args()
    sizes = a.size or [1 << 40]

    before = rss_kib(a.pid) if a.pid else None
    ok = True
    peak = before
    for size in sizes:
        closed, still_open, errors, max_rss = run(a.host, a.port, size, a.connections, a.timeout, a.pid)
        print(f"huge-frame: {a.host}:{a.port}: {closed} of {a.connections} connection(s) announcing "
              f"{size} bytes closed by the node, {still_open} still open after {a.timeout:g} s, {errors} error(s)")
        if closed != a.connections:
            ok = False
        if max_rss is not None and (peak is None or max_rss > peak):
            peak = max_rss
    if a.pid:
        if before is None or peak is None:
            print(f"huge-frame: no memory reading for pid {a.pid}")
            ok = False
        else:
            growth = (peak - before) / 1024
            print(f"huge-frame: node memory {before / 1024:.0f} MiB before, peak {peak / 1024:.0f} MiB "
                  f"during the test (+{growth:.0f} MiB, limit {a.max_growth} MiB)")
            if growth > a.max_growth:
                ok = False
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
