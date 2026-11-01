#!/usr/bin/env python3
"""Compare wallet histories on two nodes through their public APIs: the node keys of n1..nN and the
master key, each read with TransactionsGet (newest first, paged) and WalletTransactionsCountGet.

    compare_histories.py <work_dir> <nodes> <master_pub_b58> <port_a> <port_b> [--partial]

The first node is the reference. With --partial the second may lack the oldest transactions (it is
still fetching the older blocks, backward sync): its list must be the newest part of the reference's,
in the same order, never another transaction. Without it the lists must be equal. The last inner id
of each wallet (the wallet cache) must be equal in both cases.
"""
import argparse
import os
import sys

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

from fund import api


def history(client, pk):
    out = []
    offset = 0
    while True:
        result = client.TransactionsGet(pk, offset, 100)
        batch = [(t.id.poolSeq, t.id.index) for t in (result.transactions or [])]
        out.extend(batch)
        if len(batch) < 100 or offset > 100000:
            return out
        offset += len(batch)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("nodes", type=int)
    parser.add_argument("master")
    parser.add_argument("port_a", type=int)
    parser.add_argument("port_b", type=int)
    parser.add_argument("--partial", action="store_true")
    args = parser.parse_args()

    keys = {"master": base58.b58decode(args.master)}
    for i in range(1, args.nodes + 1):
        with open(os.path.join(args.work, f"n{i}", "NodePublic.txt")) as f:
            keys[f"n{i}"] = base58.b58decode(f.read().strip())

    def client(port):
        return make_client(api.API, host="127.0.0.1", port=port, proto_factory=TBinaryProtocolFactory(),
                           trans_factory=TBufferedTransportFactory(), timeout=120000)
    a, b = client(args.port_a), client(args.port_b)

    failed = False
    total_a = total_b = 0
    for name, pk in keys.items():
        ha, hb = history(a, pk), history(b, pk)
        ca = a.WalletTransactionsCountGet(pk).lastTransactionInnerId
        cb = b.WalletTransactionsCountGet(pk).lastTransactionInnerId
        total_a += len(ha)
        total_b += len(hb)
        if ca != cb:
            print(f"{name}: last inner id {cb} on the second node, {ca} on the reference")
            failed = True
        if args.partial:
            if hb != ha[:len(hb)]:
                wrong = next((i for i, (x, y) in enumerate(zip(ha, hb)) if x != y), len(ha))
                print(f"{name}: the second node's history is not the newest part of the reference's "
                      f"(first difference at entry {wrong}: {hb[wrong] if wrong < len(hb) else None} for "
                      f"{ha[wrong] if wrong < len(ha) else None})")
                failed = True
        elif ha != hb:
            print(f"{name}: {len(hb)} transactions on the second node, {len(ha)} on the reference "
                  f"(newest {hb[:2]} / {ha[:2]}, oldest {hb[-2:]} / {ha[-2:]})")
            failed = True
    print(f"histories: {len(keys)} wallets, {total_b} transactions on the second node, {total_a} on the reference"
          + (" (it may lack the oldest)" if args.partial else "") + (": differ" if failed else ": as expected"))
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
