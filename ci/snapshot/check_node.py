#!/usr/bin/env python3
"""Checks on a running node before its state is published as a snapshot.

  check_node.py wait-head --api HOST:PORT --timeout-min N
      Exit 0 once the node's last block has stayed within 3 rounds of the current round for 3 minutes
      (SyncStateGet), 1 on timeout.

  check_node.py history --api HOST:PORT --count N
      Compares the transaction history the node serves from its transaction index (TransactionsGet)
      with the monitor's, for N wallets with 1 to 50 transactions, counting blocks up to the node's
      last block only. Exit 1 on any difference.

  check_node.py balances --api HOST:PORT --count N [--self-test-mismatch]
      Compares the balances of N wallets with the public monitor, an independent source. Wallets come
      from the node (WalletsGet, at random); only those whose last transaction, per the monitor, is
      older than 24 hours and in a block the node has are compared, so both sides refer to the same
      state. Exit 1 on any mismatch, when the monitor cannot be reached, or when fewer than N/2
      wallets could be compared. --self-test-mismatch alters one balance to prove a mismatch fails.

Monitor API (https://monitor.credits.com/CreditsNetwork/Api), plain GET:
  ExtendedBalance?id=<address>                  -> {"balance": "<decimal CS>", ...}
  AccountTransactions?id=<address>&page=1&limit=1 -> {"transactions": [{"time": ISO, "blockNum": "<n>"}], ...}
"""
import argparse
import datetime
import json
import os
import random
import sys
import time
import urllib.request
from decimal import Decimal

import base58
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
MONITOR = "https://monitor.credits.com/CreditsNetwork/Api"


def client(api):
    host, port = api.rsplit(":", 1)
    mod = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])
    return make_client(mod.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                       trans_factory=TBufferedTransportFactory(), timeout=120000)


def amount(a):
    return Decimal(a.integral) + Decimal(a.fraction) / Decimal(10**18)


def monitor(path):
    req = urllib.request.Request(f"{MONITOR}/{path}", headers={"Accept": "application/json"})
    return json.load(urllib.request.urlopen(req, timeout=60))


def wait_head(args):
    node = None
    started = time.time()
    deadline = started + args.timeout_min * 60
    near_since = None
    api_up_since = None
    window = []   # (time, block) of the last --window-min minutes
    while time.time() < deadline:
        # a node that ended is not waited for
        if args.exit_file and os.path.exists(args.exit_file):
            print(f"FAIL: {open(args.exit_file).read().strip()}", flush=True)
            return 1
        try:
            # the node opens its API only once its database is loaded: connect (again) until it does
            node = node or client(args.api)
            s = node.SyncStateGet()
            gap = s.currRound - s.lastBlock
            api_up_since = api_up_since or time.time()
        except Exception as e:  # the node may still be starting
            print(f"sync state unavailable: {e}", flush=True)
            node = None
            gap = None
            if api_up_since is None and args.api_grace_min and time.time() - started > args.api_grace_min * 60:
                print(f"FAIL: the node's API is not up {args.api_grace_min} min after the start", flush=True)
                return 1
        if gap is not None:
            # the speed of the sync: a node far behind has to move, and at a speed in line with the
            # others (Linux: 30000 blocks/min and more); near the head the chain sets the pace
            now = time.time()
            window.append((now, s.lastBlock))
            while window and now - window[0][0] > args.window_min * 60 + 1:
                window.pop(0)
            if args.min_rate and gap > args.near_gap and now - window[0][0] >= args.window_min * 60:
                rate = (s.lastBlock - window[0][1]) / ((now - window[0][0]) / 60)
                print(f"speed over the last {args.window_min} min: {rate:.0f} blocks/min (at least {args.min_rate} wanted)", flush=True)
                if rate < args.min_rate:
                    print(f"FAIL: the sync is too slow: {rate:.0f} blocks/min, {gap} blocks behind", flush=True)
                    return 1
        if gap is not None and s.lastBlock > 0 and gap <= 3:
            near_since = near_since or time.time()
            if time.time() - near_since >= 180:
                print(f"at the head: block {s.lastBlock}, round {s.currRound}", flush=True)
                return 0
        else:
            near_since = None
            if gap is not None:
                print(f"block {s.lastBlock}, round {s.currRound}, {gap} behind", flush=True)
        time.sleep(30)
    print("timeout before reaching the head", flush=True)
    return 1


def balances(args):
    node = client(args.api)
    head = node.SyncStateGet().lastBlock
    total = node.WalletsGet(0, 1, 0, True).count
    picked, compared, mismatches = set(), 0, []
    cutoff = datetime.datetime.now(datetime.timezone.utc) - datetime.timedelta(hours=24)
    tries = 0
    while compared < args.count and tries < args.count * args.tries:
        tries += 1
        offset = random.randrange(max(total, 1))
        if offset in picked:
            continue
        picked.add(offset)
        w = node.WalletsGet(offset, 1, 0, True).wallets[0]
        address = base58.b58encode(w.address).decode()
        try:
            txs = monitor(f"AccountTransactions?id={address}&page=1&limit=1").get("transactions") or []
            if txs:
                last_time = datetime.datetime.fromisoformat(txs[0]["time"].replace("Z", "+00:00"))
                if last_time > cutoff or int(txs[0]["blockNum"]) > head:
                    continue  # recently active: the two sides may refer to different blocks
            theirs = Decimal(monitor(f"ExtendedBalance?id={address}")["balance"])
        except Exception as e:
            print(f"FAIL: the monitor cannot be reached: {e}", flush=True)
            return 1
        ours = amount(w.balance)
        if args.self_test_mismatch and compared == 0:
            ours += Decimal("0.001")
        compared += 1
        if ours != theirs:
            mismatches.append((address, ours, theirs))
    print(f"{compared} wallets compared at block {head} ({total} on the node), {len(mismatches)} mismatches", flush=True)
    for address, ours, theirs in mismatches:
        print(f"  {address}: node {ours}, monitor {theirs}", flush=True)
    if compared < (args.min_compared if args.min_compared is not None else args.count // 2):
        print("FAIL: too few wallets could be compared", flush=True)
        return 1
    return 1 if mismatches else 0


MONITOR_MAX_PAGES = 20


def monitor_blocks(address, head):
    """Blocks <= head of the monitor's transactions of a wallet, sorted, or None when more than
    MONITOR_MAX_PAGES pages would be needed. Pages hold the newest transactions first, so the walk
    goes from the last (oldest) page towards page 1 and stops after the first page whose newest
    transaction is above head."""
    first = monitor(f"AccountTransactions?id={address}&page=1&limit=100")
    last_page = int(first.get("lastPage") or 1)
    blocks = []
    page, data, walked = last_page, first, 0
    while page >= 1:
        if page != 1:
            if walked >= MONITOR_MAX_PAGES:
                return None
            data = monitor(f"AccountTransactions?id={address}&page={page}&limit=100")
        walked += 1
        txs = data.get("transactions") or []
        blocks += [int(t["blockNum"]) for t in txs if int(t["blockNum"]) <= head]
        if txs and int(txs[0]["blockNum"]) > head:
            break  # this page reaches past head, the newer ones hold nothing of interest
        page -= 1
    return sorted(blocks)


def history(args):
    node = client(args.api)
    head = node.SyncStateGet().lastBlock
    total = node.WalletsGet(0, 1, 0, True).count
    compared, bad, tries = 0, 0, 0
    while compared < args.count and tries < args.count * args.tries:
        tries += 1
        w = node.WalletsGet(random.randrange(max(total, 1)), 1, 0, True).wallets[0]
        if not 1 <= w.transactionsNumber <= 50:
            continue
        address = base58.b58encode(w.address).decode()
        try:
            theirs = monitor_blocks(address, head)
        except Exception as e:
            print(f"FAIL: the monitor cannot be reached: {e}", flush=True)
            return 1
        if theirs is None:
            continue  # too many pages to walk: not compared
        ours = sorted(t.id.poolSeq for t in (node.TransactionsGet(w.address, 0, 100).transactions or [])
                      if t.id.poolSeq <= head)
        compared += 1
        if ours != theirs:
            bad += 1
            print(f"  {address}: node blocks {ours[:8]}, monitor {theirs[:8]}", flush=True)
    print(f"{compared} wallet histories compared up to block {head}, {bad} differ", flush=True)
    if compared < (args.min_compared if args.min_compared is not None else args.count // 2):
        print("FAIL: too few wallets could be compared", flush=True)
        return 1
    return 1 if bad else 0


def main():
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="cmd", required=True)
    w = sub.add_parser("wait-head")
    w.add_argument("--api", required=True)
    w.add_argument("--timeout-min", type=int, default=90)
    w.add_argument("--exit-file", help="a file the caller writes when the node process ends: the wait fails at once")
    w.add_argument("--api-grace-min", type=int, default=0, help="minutes the node's API may take to come up (0 = no limit)")
    w.add_argument("--min-rate", type=int, default=0, help="blocks per minute the sync must keep while far behind (0 = no check)")
    w.add_argument("--window-min", type=int, default=2, help="minutes the speed is measured over")
    w.add_argument("--near-gap", type=int, default=3000, help="no speed check this close to the head, in blocks")
    b = sub.add_parser("balances")
    b.add_argument("--api", required=True)
    b.add_argument("--count", type=int, default=50)
    b.add_argument("--tries", type=int, default=6, help="wallets looked at per wallet compared, at most")
    b.add_argument("--self-test-mismatch", action="store_true")
    b.add_argument("--min-compared", type=int, help="fewest wallets that must be compared (default: half of --count)")
    h = sub.add_parser("history")
    h.add_argument("--api", required=True)
    h.add_argument("--count", type=int, default=20)
    h.add_argument("--tries", type=int, default=20)
    h.add_argument("--min-compared", type=int)
    args = parser.parse_args()
    sys.exit({"wait-head": wait_head, "balances": balances, "history": history}[args.cmd](args))


if __name__ == "__main__":
    main()
