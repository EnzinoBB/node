#!/usr/bin/env python3
"""Fund the nodes of a private test network and keep some transfers flowing.

Usage: fund.py <work_dir> <nodes> <master_seed_b58> [--port 9090] [--amount 60000] [--load-seconds 0]

The genesis funds of a CREDITS_TESTNET build go to the key given in CS_TESTNET_GENESIS_KEY; this
script holds its seed. It sends <amount> CS from it to every node (above the 50'000 minimum stake),
then, for --load-seconds, small random transfers between nodes, through the Thrift API of node 1.

Transaction building and signing are adapted from BK's tools/tps_gen/tps_gen.py (akaitrade/node).
"""
import argparse
import math
import os
import random
import struct
import sys
import time

import base58
import nacl.signing
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
api = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])
general = thriftpy2.load(os.path.join(IDL, "general.thrift"), module_name="general_thrift", include_dirs=[IDL])

CURRENCY_CS = 1
TX_TYPE_TRANSFER = 0


def encode_max_fee(value):
    """Mirror of csdb::AmountCommission(double) -> uint16 bit pattern."""
    if value <= 0:
        return 0
    v = abs(value)
    expf = math.log10(v)
    expi = int(expf + 0.5) if expf >= 0 else int(expf - 0.5)
    v /= 10 ** expi
    if v >= 1.0:
        v *= 0.1
        expi += 1
    exp = (expi + 18) & 0x1F
    frac = int(round(v * 1024)) & 0x3FF
    return (exp << 10) | frac


MAX_FEE = encode_max_fee(1.0)


class Account:
    def __init__(self, seed):
        self.sk = nacl.signing.SigningKey(seed)
        self.pk = bytes(self.sk.verify_key)
        self.next_inner_id = None


def pack_inner_id(inner_id):
    return inner_id.to_bytes(6, "little")  # source and target given as public keys, not wallet ids


def transfer(src, dst_pk, amount_int, amount_frac=0):
    payload = b"".join([
        pack_inner_id(src.next_inner_id),
        src.pk,
        dst_pk,
        struct.pack("<iQ", amount_int, amount_frac),
        struct.pack("<H", MAX_FEE),
        struct.pack("<B", CURRENCY_CS),
        struct.pack("<B", 0),  # no user fields
    ])
    t = api.Transaction()
    t.id = src.next_inner_id
    t.source = src.pk
    t.target = dst_pk
    t.amount = general.Amount(integral=amount_int, fraction=amount_frac)
    t.balance = general.Amount(integral=0, fraction=0)
    t.currency = CURRENCY_CS
    t.signature = src.sk.sign(payload).signature
    t.fee = api.AmountCommission(commission=MAX_FEE)
    t.timeCreation = int(time.time() * 1000)
    t.userFields = b""
    t.type = TX_TYPE_TRANSFER
    t.poolNumber = 0
    return t


def send(client, src, dst_pk, amount_int, amount_frac=0):
    if src.next_inner_id is None:
        last = client.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0
        src.next_inner_id = int(last) + 1
    result = client.TransactionFlow(transfer(src, dst_pk, amount_int, amount_frac))
    ok = result.status.code == 0
    if ok:
        src.next_inner_id += 1
    return ok, result.status.message


def node_account(work, i):
    secret = base58.b58decode(open(os.path.join(work, f"n{i}", "NodePrivate.txt")).read().strip())
    return Account(secret[:32])


def balance(client, pk):
    b = client.WalletBalanceGet(pk).balance
    return b.integral + b.fraction / 1e18


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("nodes", type=int)
    parser.add_argument("master_seed")
    parser.add_argument("--port", type=int, default=9090)
    parser.add_argument("--amount", type=int, default=60000)
    parser.add_argument("--load-seconds", type=int, default=0)
    args = parser.parse_args()

    client = make_client(api.API, host="127.0.0.1", port=args.port, proto_factory=TBinaryProtocolFactory(),
                         trans_factory=TBufferedTransportFactory(), timeout=120000)
    master = Account(base58.b58decode(args.master_seed))
    nodes = [node_account(args.work, i) for i in range(1, args.nodes + 1)]
    print(f"master balance before funding: {balance(client, master.pk):.4f}", flush=True)

    failures = 0
    for i, node in enumerate(nodes, 1):
        ok, message = send(client, master, node.pk, args.amount)
        print(f"fund n{i}: {'ok' if ok else 'FAILED ' + message}", flush=True)
        failures += 0 if ok else 1

    sent = 0
    deadline = time.time() + args.load_seconds
    while time.time() < deadline:
        src, dst = random.sample(nodes, 2)
        ok, message = send(client, src, dst.pk, random.randint(1, 50))
        sent += 1 if ok else 0
        if not ok:
            print(f"load transfer failed: {message}", flush=True)
            failures += 1
        time.sleep(1)
    print(f"load transfers sent: {sent}", flush=True)

    for i, node in enumerate(nodes, 1):
        print(f"balance n{i}: {balance(client, node.pk):.4f}", flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
