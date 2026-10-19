#!/usr/bin/env python3
"""Fund the nodes of a private test network and keep some transfers flowing.

Usage: fund.py <work_dir> <nodes> <master_seed_b58> [--port 9090] [--amount 60000] [--load-seconds 0]
               [--dpos-at SEQ --reward-round SEQ] [--min-stake CS] [--delegations]

The genesis funds of a CREDITS_TESTNET build go to the key given in CS_TESTNET_GENESIS_KEY; this
script holds its seed. It sends <amount> CS from it to every node (above the 50'000 minimum stake),
then, for --load-seconds, small random transfers between nodes, through the Thrift API of node 1.

With --dpos-at and/or --reward-round the master key, which is also the starter key of the test network
(CS_TESTNET_STARTER_KEY), sends two special "managing" transactions: order 9 moves StartingDPOS to
--dpos-at, and order 37 turns mining and staking on with a 1 CS block reward from --reward-round.
Trusted nodes then earn rewards split by stake, as on mainnet. --min-stake sends order 22, which
sets the minimum stake of a trusted node (keep it below the funded amount).

With --delegations every node delegates to the next one twice: 1'000 CS without a time limit and
500 CS until --delegation-seconds from now (a timed delegation, which the nodes release on their
own once a block is past that time). Halfway through the load phase, the even nodes withdraw the
delegation without a time limit.

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
from thriftpy2.transport import TBufferedTransportFactory, TTransportException

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
api = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])
general = thriftpy2.load(os.path.join(IDL, "general.thrift"), module_name="general_thrift", include_dirs=[IDL])

CURRENCY_CS = 1
TX_TYPE_TRANSFER = 0
UF_DELEGATED = 5         # cs::trx_uf::sp::delegated
UF_MANAGING = 7          # cs::trx_uf::sp::managing
UF_TYPE_INTEGER = 1      # csdb::UserField::Integer
UF_TYPE_STRING = 2       # csdb::UserField::String
DELEGATE = 1             # cs::trx_uf::sp::de::legate
DELEGATE_WITHDRAW = 2    # cs::trx_uf::sp::de::legated_withdraw (a value >= 4 is a UTC expiry time)
SPECIAL_TARGET = bytes(31) + b"\x07"  # the target mainnet special transactions use (1111...118)


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


def transfer(src, dst_pk, amount_int, amount_frac=0, managing=None, delegated=None):
    # signed bytes: Transaction::to_byte_stream_for_sig(); a string user field signs as u32 size + bytes,
    # an integer one as u64
    if managing is not None:
        fields_for_sig = struct.pack("<BI", 1, len(managing)) + managing
    elif delegated is not None:
        fields_for_sig = struct.pack("<BQ", 1, delegated)
    else:
        fields_for_sig = struct.pack("<B", 0)
    payload = b"".join([
        pack_inner_id(src.next_inner_id),
        src.pk,
        dst_pk,
        struct.pack("<iQ", amount_int, amount_frac),
        struct.pack("<H", MAX_FEE),
        struct.pack("<B", CURRENCY_CS),
        fields_for_sig,
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
    # API encoding (APIHandler): flag 0, count, then id u32, type u8, u32 size, bytes
    if managing is not None:
        t.userFields = struct.pack("<BBIBI", 0, 1, UF_MANAGING, UF_TYPE_STRING, len(managing)) + managing
    elif delegated is not None:
        t.userFields = struct.pack("<BBIBQ", 0, 1, UF_DELEGATED, UF_TYPE_INTEGER, delegated)
    else:
        t.userFields = b""
    t.type = TX_TYPE_TRANSFER
    t.poolNumber = 0
    return t


class Client:
    """The API of one node, reopened when the node restarts (crash and power-cut scenarios).

    A read waits for the node to come back (up to --reconnect-seconds). A transfer is not repeated:
    if the node went away while it was sent it may or may not have been accepted, so the sender's
    inner id is read again before its next transfer and the attempt reports the node unreachable.
    """

    def __init__(self, port, reconnect_seconds):
        self.port = port
        self.reconnect_seconds = reconnect_seconds
        self.conn = None

    def _open(self):
        if self.conn is None:
            self.conn = make_client(api.API, host="127.0.0.1", port=self.port, proto_factory=TBinaryProtocolFactory(),
                                    trans_factory=TBufferedTransportFactory(), timeout=120000)
        return self.conn

    def _drop(self):
        if self.conn is not None:
            try:
                self.conn.close()
            except Exception:
                pass
        self.conn = None

    def call(self, name, *args, retry=True):
        give_up = time.time() + self.reconnect_seconds
        while True:
            try:
                return getattr(self._open(), name)(*args)
            except (TTransportException, ConnectionError, OSError) as e:
                self._drop()
                if not retry or time.time() >= give_up:
                    raise
                print(f"API {name}: {e}, retrying", flush=True)
                time.sleep(5)

    def __getattr__(self, name):
        return lambda *args: self.call(name, *args)


class Unreachable(Exception):
    pass


def send(client, src, dst_pk, amount_int, amount_frac=0, managing=None, delegated=None):
    if src.next_inner_id is None:
        last = client.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0
        src.next_inner_id = int(last) + 1
    src.last_sent_id = src.next_inner_id
    try:
        result = client.call("TransactionFlow", transfer(src, dst_pk, amount_int, amount_frac, managing, delegated),
                             retry=False)
    except (TTransportException, ConnectionError, OSError) as e:
        src.next_inner_id = None
        raise Unreachable(str(e))
    ok = result.status.code == 0
    if ok:
        src.next_inner_id += 1
    else:
        # a transfer still in progress may land later with this inner id: read the next one again
        src.next_inner_id = None
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
    parser.add_argument("--dpos-at", type=int, default=0)
    parser.add_argument("--reward-round", type=int, default=0)
    parser.add_argument("--min-stake", type=int, default=0)
    parser.add_argument("--syncro-round", type=int, default=0)
    parser.add_argument("--delegations", action="store_true")
    parser.add_argument("--delegation-seconds", type=int, default=240)
    parser.add_argument("--reconnect-seconds", type=int, default=600)
    args = parser.parse_args()

    client = Client(args.port, args.reconnect_seconds)
    master = Account(base58.b58decode(args.master_seed))
    nodes = [node_account(args.work, i) for i in range(1, args.nodes + 1)]
    print(f"master balance before funding: {balance(client, master.pk):.4f}", flush=True)

    failures = 0
    for i, node in enumerate(nodes, 1):
        ok, message = send(client, master, node.pk, args.amount)
        print(f"fund n{i}: {'ok' if ok else 'FAILED ' + message}", flush=True)
        failures += 0 if ok else 1

    # Node::processSpecialInfo: u16 order, then the order's fields (cs::IDataStream, little endian);
    # each order is optional, without order 9 StartingDPOS stays at its default (10'000)
    orders = []
    if args.dpos_at:
        orders.append(("order 9 (StartingDPOS = %d)" % args.dpos_at, struct.pack("<HQ", 9, args.dpos_at)))
    if args.reward_round:
        # staking+mining, reward 1.0, coeff 0
        orders.append(("order 37 (rewards from round %d)" % args.reward_round,
                       struct.pack("<HBQiQiQ", 37, 3, args.reward_round, 1, 0, 0, 0)))
    if args.min_stake:
        orders.append(("order 22 (MinStakeValue = %d)" % args.min_stake, struct.pack("<HiQ", 22, args.min_stake, 0)))
    if args.syncro_round:
        # Consensus::syncroChangeRound: from that round the sync answers (AwaitAnswer, NoSuchBlocks...) count
        orders.append(("order 35 (syncroChangeRound = %d)" % args.syncro_round, struct.pack("<HQ", 35, args.syncro_round)))
    if orders:
        for name, cmd in orders:
            ok, message = send(client, master, SPECIAL_TARGET, 0, managing=cmd)
            print(f"{name}: {'ok' if ok else 'FAILED ' + message}", flush=True)
            failures += 0 if ok else 1

    def delegate(name, src, dst, amount, value):
        ok, message = send(client, src, dst.pk, amount, delegated=value)
        print(f"{name}: {'ok' if ok else 'FAILED ' + message}", flush=True)
        return 0 if ok else 1

    pairs = [(i, nodes[i - 1], nodes[i % len(nodes)]) for i in range(1, len(nodes) + 1)]
    if args.delegations:
        # a delegation is checked against the confirmed balance, so wait for the funding to land
        wait_until = time.time() + 180
        while time.time() < wait_until and any(balance(client, n.pk) < args.amount for n in nodes):
            time.sleep(5)
        expiry = int(time.time()) + args.delegation_seconds
        for i, src, dst in pairs:
            failures += delegate(f"n{i} delegates 1000 to n{i % len(nodes) + 1}", src, dst, 1000, DELEGATE)
            failures += delegate(f"n{i} delegates 500 to n{i % len(nodes) + 1} until {expiry}",
                                 src, dst, 500, expiry)

    print("master transactions done", flush=True)  # run.sh: the master key is free for contracts.py

    sent = 0
    unreachable = 0
    pending = 0
    unsynced = 0
    start = time.time()
    deadline = start + args.load_seconds
    withdrawn = not args.delegations
    while time.time() < deadline:
        if not withdrawn and time.time() >= start + args.load_seconds / 2:
            withdrawn = True
            for i, src, dst in pairs[1::2]:
                failures += delegate(f"n{i} withdraws 1000 from n{i % len(nodes) + 1}",
                                     src, dst, 1000, DELEGATE_WITHDRAW)
        src, dst = random.sample(nodes, 2)
        if getattr(src, "wait_for", None) is not None:
            # a transfer of this sender is still in progress: until it is recorded (or 60 s pass),
            # its inner id is not known, and reusing it would get the next transfer rejected
            last = int(client.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0)
            if last < src.wait_for and time.time() < src.wait_until:
                time.sleep(1)
                continue
            src.wait_for = None
            src.next_inner_id = None
        try:
            ok, message = send(client, src, dst.pk, random.randint(1, 50))
        except Unreachable as e:
            # the node is down (a crash or power-cut scenario): not a rejected transfer
            unreachable += 1
            print(f"load transfer not sent, node unreachable: {e}", flush=True)
            time.sleep(5)
            continue
        sent += 1 if ok else 0
        if not ok and message.startswith("Transaction in progress"):
            # the API stopped waiting for consensus (a node was just killed): the outcome is unknown
            pending += 1
            src.wait_for, src.wait_until = src.last_sent_id, time.time() + 60
            print(f"load transfer still in progress: {message}", flush=True)
        elif not ok and "not syncronized" in message:
            # the node refuses transfers while it catches up after a restart (power-cut scenario)
            unsynced += 1
            print(f"load transfer refused while the node catches up: {message}", flush=True)
        elif not ok:
            print(f"load transfer failed: {message}", flush=True)
            failures += 1
        time.sleep(1)
    print(f"load transfers sent: {sent}, not sent while the node was unreachable: {unreachable}, "
          f"still in progress when the API stopped waiting: {pending}, refused while catching up: {unsynced}", flush=True)

    for i, node in enumerate(nodes, 1):
        print(f"balance n{i}: {balance(client, node.pk):.4f}", flush=True)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
