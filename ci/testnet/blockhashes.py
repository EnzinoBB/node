#!/usr/bin/env python3
"""Compare the blocks of several nodes through their public APIs: the hash of every 100th block up to
the lowest head, and of that head, must be the same on every node as on the first one given.

Used by run.sh with BIN_ALT_OFFICIAL=1, where the official nodes log no state digest: holding the
same blocks as the community nodes is what they are checked on.

    blockhashes.py --head 1100 1:9090 2:9102 3:9103 ...
"""
import argparse
import sys

from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

from fund import api


def block_hash(client, seq):
    result = client.PoolListGetStable(seq, 1)
    for pool in result.pools or []:
        if pool.poolNumber == seq:
            return bytes(pool.hash).hex()
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--head", type=int, required=True, help="lowest head of the nodes compared")
    parser.add_argument("nodes", nargs="+", help="node:port, the first is the reference")
    args = parser.parse_args()

    sequences = list(range(100, args.head, 100)) + [args.head]
    reference = None
    failed = False
    for entry in args.nodes:
        node, port = entry.split(":")
        try:
            client = make_client(api.API, host="127.0.0.1", port=int(port), proto_factory=TBinaryProtocolFactory(),
                                 trans_factory=TBufferedTransportFactory(), timeout=60000)
            hashes = {seq: block_hash(client, seq) for seq in sequences}
        except Exception as e:  # a node that does not answer is a failure, with the reason
            print(f"n{node}: API on port {port} not answering: {e}")
            failed = True
            continue
        if reference is None:
            reference = hashes
            print(f"blocks: n{node} is the reference, {len(sequences)} blocks up to #{args.head}")
            continue
        missing = [seq for seq, h in hashes.items() if h is None]
        differ = [seq for seq, h in hashes.items() if h is not None and h != reference[seq]]
        if missing or differ:
            failed = True
            print(f"n{node}: {len(differ)} blocks differ from the reference (first {differ[:3]}), "
                  f"{len(missing)} not found (first {missing[:3]})")
        else:
            print(f"n{node}: the same {len(sequences)} blocks as the reference")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
