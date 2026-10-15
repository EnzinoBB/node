#!/usr/bin/env python3
"""Send random UDP datagrams to the nodes' p2p ports for a while.

Usage: garbage.py <base_port> <nodes> <seconds> <rate_per_second>

Half the datagrams are pure noise, half start with bytes taken from the start of a real datagram
shape (a small header, then noise), so that both the first parsing step and the deeper decoding
of a message see malformed input. A node must ignore all of it and stay in consensus.
"""
import os
import random
import socket
import sys
import time


def main():
    base, nodes, seconds, rate = (int(a) for a in sys.argv[1:5])
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    rng = random.Random(int.from_bytes(os.urandom(8), "little"))
    sent = 0
    deadline = time.time() + seconds
    while time.time() < deadline:
        start = time.time()
        for _ in range(rate):
            port = base + rng.randint(1, nodes)
            size = rng.choice((1, 2, 7, 16, 64, 300, 1024, 1400, rng.randint(1, 1400)))
            data = os.urandom(size)
            if rng.random() < 0.5 and size > 4:
                # a plausible small header: a flags byte and a length that may or may not match
                data = bytes([rng.choice((0, 1, 2, 4, 8, 0x10, 0x20, 0x40, 0x80))]) + data[1:]
            try:
                sock.sendto(data, ("127.0.0.1", port))
                sent += 1
            except OSError:
                pass
        time.sleep(max(0.0, 1.0 - (time.time() - start)))
    print(f"garbage: {sent} random datagrams sent to {nodes} nodes in {seconds} s", flush=True)


if __name__ == "__main__":
    main()
