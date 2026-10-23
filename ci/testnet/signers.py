#!/usr/bin/env python3
"""Who signs the blocks: for every node, the rounds it was trusted in and the blocks it signed.

Usage: signers.py <work_dir> <nodes> <last_sequence> [--port 9090] [--alt-even]

Reads every block from node 1's API (PoolListGetStable): a block lists the trusted of its round
(confidants) and which of them signed it (realTrusted, one bit per trusted). With --alt-even the
even nodes run another binary (BIN_ALT, e.g. the official code): the share of signed rounds is also
given per group, to see whether one binary ends up outside the signers more often than the other.
Prints a report only; it never fails a run.
"""
import argparse
import os
import sys

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

from fund import api


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("nodes", type=int)
    parser.add_argument("top", type=int)
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--alt-even", action="store_true")
    args = parser.parse_args()

    keys = {}
    for i in range(1, args.nodes + 1):
        with open(os.path.join(args.work, f"n{i}", "NodePublic.txt")) as f:
            keys[base58.b58decode(f.read().strip())] = i

    client = make_client(api.API, host="127.0.0.1", port=args.port, proto_factory=TBinaryProtocolFactory(),
                         trans_factory=TBufferedTransportFactory(), timeout=120000)
    top = args.top
    trusted = {i: 0 for i in keys.values()}
    signed = {i: 0 for i in keys.values()}
    blocks = 0
    seq = top
    while seq > 0:
        result = client.PoolListGetStable(seq, 100)
        if not result.pools:
            break
        for pool in result.pools:
            if pool.poolNumber <= 0:
                continue
            blocks += 1
            for index, key in enumerate(pool.confidants or []):
                node = keys.get(bytes(key))
                if node is None:
                    continue
                trusted[node] += 1
                if (pool.realTrusted >> index) & 1:
                    signed[node] += 1
        seq = min(p.poolNumber for p in result.pools) - 1

    def share(nodes):
        t = sum(trusted[i] for i in nodes)
        s = sum(signed[i] for i in nodes)
        return t, s, (100.0 * s / t if t else 0.0)

    print(f"signers: {blocks} blocks read from n1, up to #{top}")
    for i in sorted(trusted):
        group = (" official" if i % 2 == 0 else " community") if args.alt_even else ""
        t, s, pct = share([i])
        print(f"signers: n{i}{group}: trusted in {t} rounds, signed {s} ({pct:.1f} %)")
    if args.alt_even:
        for name, nodes in (("community (odd)", [i for i in trusted if i % 2]), ("official (even)", [i for i in trusted if i % 2 == 0])):
            t, s, pct = share(nodes)
            print(f"signers: {name}: trusted in {t} rounds, signed {s} ({pct:.1f} %), {t - s} rounds trusted without signing")


if __name__ == "__main__":
    try:
        main()
    except Exception as e:  # a report only
        print(f"signers: report unavailable ({type(e).__name__}: {e})")
        sys.exit(0)
