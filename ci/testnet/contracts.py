#!/usr/bin/env python3
"""Deploy smart contracts on the private test network and call them across the restart test.

Usage: contracts.py <work_dir> <nodes> <master_seed_b58> [--calls-before 3] [--calls-after 5]
                    [--ready-file PATH] [--wait-file PATH] [--wait-timeout 1800]
                    [--check-nodes N] [--final-file PATH]

The master key deploys Counter, whose next() adds one to its state and returns it, and calls it
--calls-before times. It then creates --ready-file and, once --wait-file exists (run.sh creates
both around the restart test, so the restarted node has caught up), calls it --calls-after times
more. Every call must return the next value, and at the end every node must report the same
Counter state.

It also deploys Caller, whose callCounter() reaches Counter through invokeExternalContract, and
calls it once; the result is only reported. Contract-to-contract calls do not work with the
mainnet node and executor: the node refuses methods declaring @UsingContract
(Violations::SubsequentCall), and executor build 1518 fails an undeclared invokeExternalContract
with an AccessControlException (its configuration is created lazily inside the contract sandbox).

Then, once --final-file exists (run.sh creates it when every node reached its target sequence),
it compares the contract states on nodes 1 to --check-nodes (default <nodes>; run.sh adds a node
that joined later).

Transactions go through node 1's API (port 9090), so a test scenario must never kill, partition or
slow node 1: a call during an outage ends the script. Only a transaction node 1 refuses because it
is not synchronized yet (after a power cut) is sent again, for up to 120 s: nothing was accepted.
The final check reads every node's API (node i > 1 listens on 9100 + i).
"""
import argparse
import hashlib
import json
import os
import statistics
import struct
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory
from thriftpy2.utils import serialize

from fund import Account, api, encode_max_fee, general, node_account, pack_inner_id

CURRENCY_CS = 1
TT_DEPLOY = 1   # api.TransactionType: contract deployment
TT_EXECUTE = 2  # api.TransactionType: contract execution
SMART_MAX_FEE = encode_max_fee(10.0)
COUNTER = """import com.credits.scapi.v0.SmartContract;

public class Counter extends SmartContract {
    private int value;

    public Counter() {
        super();
    }

    public int next() {
        value += 1;
        return value;
    }

    public int get() {
        return value;
    }
}
"""

# API v3, as current contracts are written: payable() is mandatory, and the state holds instances of
# an inner class in a map, so deserializing the state and running the call both go through classes
# other than the contract's root class
LEDGER = """import com.credits.scapi.v3.SmartContract;

import java.math.BigDecimal;
import java.util.HashMap;
import java.util.Map;

public class Ledger extends SmartContract {
    private final Map<String, Entry> entries = new HashMap<>();
    private int total;

    public static class Entry implements java.io.Serializable {
        private int count;

        int add() {
            count += 1;
            return count;
        }
    }

    public Ledger() {
        super();
    }

    public int record() {
        entries.computeIfAbsent("k" + (total % 3), k -> new Entry()).add();
        total += 1;
        return total;
    }

    public int entries() {
        return entries.values().stream().mapToInt(e -> e.count).sum();
    }

    @Override
    public String payable(BigDecimal amount, byte[] userData) {
        return "ok";
    }
}
"""

# no @UsingContract: the node refuses any call to a method that declares one (Violations::SubsequentCall,
# checked by the API and by IterValidator), so the call is a plain invokeExternalContract
CALLER = """import com.credits.scapi.v0.SmartContract;

public class Caller extends SmartContract {
    private int calls;
    private int last;

    public Caller() {
        super();
    }

    public int callCounter() {
        last = (Integer) invokeExternalContract("%s", "get");
        calls += 1;
        return last;
    }
}
"""

# the same call declared with @UsingContract: the node refuses it (Violations::SubsequentCall)
CALLER_DECLARED = """import com.credits.scapi.annotations.UsingContract;
import com.credits.scapi.v0.SmartContract;

public class CallerDeclared extends SmartContract {
    private int calls;
    private int last;

    public CallerDeclared() {
        super();
    }

    @UsingContract(address = "%s", method = "get")
    public int callCounter() {
        last = (Integer) invokeExternalContract("%s", "get");
        calls += 1;
        return last;
    }
}
"""


def write_sources(directory):
    """Write the test contracts as Java files, for a compile check before the network starts."""
    placeholder = "11111111111111111111111111111111"
    with open(os.path.join(directory, "Counter.java"), "w") as f:
        f.write(COUNTER)
    with open(os.path.join(directory, "Caller.java"), "w") as f:
        f.write(CALLER % placeholder)
    with open(os.path.join(directory, "CallerDeclared.java"), "w") as f:
        f.write(CALLER_DECLARED % (placeholder, placeholder))
    with open(os.path.join(directory, "Ledger.java"), "w") as f:
        f.write(LEDGER)


def invocation(method="", used=(), deploy=None):
    # every non-optional field set, so thriftpy2 writes the same bytes as the node's
    # cs::Serializer::serialize, which the signature covers (user field 0, deploy::Code)
    return api.SmartContractInvocation(method=method, params=[], usedContracts=list(used),
                                       forgetNewState=False, smartContractDeploy=deploy, version=1)


def smart_transaction(src, target, sci, tx_type):
    code = serialize(sci, TBinaryProtocolFactory())
    payload = b"".join([
        pack_inner_id(src.next_inner_id),
        src.pk,
        target,
        struct.pack("<iQ", 0, 0),
        struct.pack("<H", SMART_MAX_FEE),
        struct.pack("<B", CURRENCY_CS),
        struct.pack("<BI", 1, len(code)) + code,  # one user field: u32 size + bytes
    ])
    t = api.Transaction()
    t.id = src.next_inner_id
    t.source = src.pk
    t.target = target
    t.amount = general.Amount(integral=0, fraction=0)
    t.balance = general.Amount(integral=0, fraction=0)
    t.currency = CURRENCY_CS
    t.signature = src.sk.sign(payload).signature
    t.smartContract = sci
    t.fee = api.AmountCommission(commission=SMART_MAX_FEE)
    t.timeCreation = int(time.time() * 1000)
    t.type = tx_type
    return t


def read_transaction(src, contract, method):
    """A getter: TransactionFlow with forgetNewState runs the method on the node's own executor
    against the contract's current state and returns its result; nothing goes to consensus, no
    transaction, no fee, no signature check (api/src/apihandler.cpp). A constant signature keeps the
    node's getter bookkeeping from growing."""
    sci = api.SmartContractInvocation(method=method, params=[], usedContracts=[], forgetNewState=True,
                                      smartContractDeploy=None, version=1)
    t = api.Transaction()
    t.id = 0
    t.source = src.pk
    t.target = contract
    t.amount = general.Amount(integral=0, fraction=0)
    t.balance = general.Amount(integral=0, fraction=0)
    t.currency = CURRENCY_CS
    t.signature = bytes(64)
    t.smartContract = sci
    t.fee = api.AmountCommission(commission=SMART_MAX_FEE)
    t.timeCreation = int(time.time() * 1000)
    t.type = TT_EXECUTE
    return t


def read_value(client, src, contract, method):
    """One getter call: (value or None, latency ms, status message)."""
    t0 = time.time()
    result = client.TransactionFlow(read_transaction(src, contract, method))
    ms = (time.time() - t0) * 1000
    # an executor that is not reachable answers status 0 without a result
    value = result.smart_contract_result.v_int if result.status.code == 0 and result.smart_contract_result else None
    return value, ms, result.status.message


def next_inner_id(client, account):
    last = client.WalletTransactionsCountGet(account.pk).lastTransactionInnerId or 0
    account.next_inner_id = int(last) + 1


def deploy(client, master, name, source):
    # the node's executor JVM may still be starting (many nodes start theirs at once): the API
    # answers with a transport error then, so the compile is tried again for a while
    deadline = time.time() + 600
    while True:
        compiled = client.SmartContractCompile(source)
        if compiled.status.code == 0 or time.time() >= deadline or "socket" not in (compiled.status.message or ""):
            break
        print(f"compile {name}: executor not reachable yet ({compiled.status.message!r}), retrying", flush=True)
        time.sleep(10)
    if compiled.status.code != 0:
        sys.exit(f"compile {name} FAILED: {compiled.status.message}")
    next_inner_id(client, master)
    # SmartContracts::get_valid_smart_address: blake2s(deployer key, 6-byte inner id, byte code)
    code = b"".join(bco.byteCode for bco in compiled.byteCodeObjects)
    address = hashlib.blake2s(master.pk + master.next_inner_id.to_bytes(6, "little") + code).digest()
    sci = invocation(deploy=api.SmartContractDeploy(sourceCode=source, byteCodeObjects=compiled.byteCodeObjects,
                                                    hashState="", tokenStandard=compiled.tokenStandard, lang=0))
    result = client.TransactionFlow(smart_transaction(master, address, sci, TT_DEPLOY))
    print(f"deploy {name} {base58.b58encode(address).decode()}: "
          f"{'ok' if result.status.code == 0 else 'FAILED ' + result.status.message}", flush=True)
    if result.status.code != 0:
        sys.exit(1)
    deadline = time.time() + 300
    while time.time() < deadline:
        if client.SmartContractGet(address).status.code == 0:
            return address
        time.sleep(5)
    sys.exit(f"{name} not deployed after 300 s")


def call(client, master, contract, method, label, expected=None):
    deadline = time.time() + 120
    while True:
        next_inner_id(client, master)
        sci = invocation(method=method)
        result = client.TransactionFlow(smart_transaction(master, contract, sci, TT_EXECUTE))
        # refused before it was accepted (api/src/apihandler.cpp "Node is not syncronized"): send it again
        if result.status.code == 0 or "not syncronized" not in (result.status.message or "") or time.time() >= deadline:
            break
        print(f"{label}: node not synchronized yet, sending again", flush=True)
        time.sleep(5)
    value = result.smart_contract_result.v_int if result.smart_contract_result else None
    ok = result.status.code == 0 and value == expected
    outcome = ("ok" if ok else "FAILED") if expected is not None else "reported"
    print(f"{label}: {outcome} (status {result.status.code} {result.status.message!r}, returned {value})", flush=True)
    return ok


def percentile(values, p):
    if not values:
        return None
    values = sorted(values)
    return values[min(len(values) - 1, int(round(p / 100 * (len(values) - 1))))]


def load(args, client_for, master, counter, nodes):
    """args.load_seconds of Counter.next writes (args.write_rate per second in all, one writer
    thread per funded node account, each through its own node's API) and Counter.get reads
    (args.read_rate per second, spread over every node). Returns (writes that returned a value,
    writes that failed, writes consensus rejected, 1 if more than 1 % of the reads failed)."""
    stats = {"write": [], "read": []}
    errors = {"write": 0, "read": 0}
    # concurrent calls of one contract from several accounts: consensus takes some and rejects the
    # others in a round; a rejected call is an answer, not a fault, and is counted on its own
    rejected = [0]
    lock = threading.Lock()
    stop_at = time.time() + args.load_seconds
    writers = list(range(1, nodes + 1))

    def writer(i):
        account = node_account(args.work, i)
        period = len(writers) / args.write_rate if args.write_rate > 0 else None
        while period and time.time() < stop_at:
            t0 = time.time()
            ok = False
            try:
                c = client_for(i)
                next_inner_id(c, account)
                sci = invocation(method="next")
                result = c.TransactionFlow(smart_transaction(account, counter, sci, TT_EXECUTE))
                ok = result.status.code == 0 and result.smart_contract_result is not None
                msg = result.status.message
            except Exception as e:
                msg = type(e).__name__
            ms = (time.time() - t0) * 1000
            with lock:
                if ok:
                    stats["write"].append(ms)
                # refused by consensus (concurrent calls of one contract in a round), or by the API while
                # the account's previous call is still in progress: answers, not faults
                elif "rejected by consensus" in (msg or "") or "Transaction in progress" in (msg or ""):
                    rejected[0] += 1
                else:
                    errors["write"] += 1
                    if errors["write"] <= 5:
                        print(f"load write via n{i} FAILED: {msg!r}", flush=True)
            time.sleep(max(0.0, period - (time.time() - t0)))

    def reader(k):
        period = 1.0 / args.read_rate * 4 if args.read_rate > 0 else None
        n = k
        while period and time.time() < stop_at:
            t0 = time.time()
            i = n % nodes + 1
            n += 4
            try:
                value, ms, msg = read_value(client_for(i), master, counter, "get")
                ok = value is not None
            except Exception as e:
                ok, ms, msg = False, (time.time() - t0) * 1000, type(e).__name__
            with lock:
                if ok:
                    stats["read"].append(ms)
                else:
                    errors["read"] += 1
                    if errors["read"] <= 5:
                        print(f"load read via n{i} FAILED: {msg!r}", flush=True)
            time.sleep(max(0.0, period - (time.time() - t0)))

    print(f"load: {args.load_seconds} s, {args.write_rate} writes/s from {len(writers)} accounts, "
          f"{args.read_rate} reads/s on {nodes} nodes", flush=True)
    threads = [threading.Thread(target=writer, args=(i,)) for i in writers] + \
              [threading.Thread(target=reader, args=(k,)) for k in range(4)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()

    summary = {"nodes": nodes, "seconds": args.load_seconds, "writes_rejected": rejected[0]}
    for kind in ("write", "read"):
        ms = stats[kind]
        sent = len(ms) + errors[kind] + (rejected[0] if kind == "write" else 0)
        summary[kind] = {"sent": sent, "ok": len(ms), "failed": errors[kind],
                         "rate": round(len(ms) / args.load_seconds, 2),
                         "p50_ms": percentile(ms, 50), "p95_ms": percentile(ms, 95), "max_ms": max(ms) if ms else None}
        print(f"load {kind}s: {len(ms)} ok of {sent} ({summary[kind]['rate']}/s), latency "
              f"p50 {summary[kind]['p50_ms'] or 0:.0f} ms, p95 {summary[kind]['p95_ms'] or 0:.0f} ms, "
              f"max {summary[kind]['max_ms'] or 0:.0f} ms", flush=True)
    print(f"load writes refused (by consensus for concurrent calls of one contract, or while the account's "
          f"previous call was in progress): {rejected[0]}", flush=True)
    if args.stats_file:
        with open(args.stats_file, "w") as f:
            json.dump(summary, f, indent=1)
    read_sent = summary["read"]["sent"]
    bad_reads = 1 if read_sent and errors["read"] > 0.01 * read_sent else 0
    return len(stats["write"]), errors["write"], rejected[0], bad_reads


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("nodes", type=int)
    parser.add_argument("master_seed")
    parser.add_argument("--calls-before", type=int, default=3)
    parser.add_argument("--calls-after", type=int, default=5)
    parser.add_argument("--ready-file", default="")
    parser.add_argument("--wait-file", default="")
    parser.add_argument("--wait-timeout", type=int, default=1800)
    parser.add_argument("--check-nodes", type=int, default=0)
    parser.add_argument("--final-file", default="")
    parser.add_argument("--load-seconds", type=int, default=0)
    parser.add_argument("--write-rate", type=float, default=1.0)
    parser.add_argument("--read-rate", type=float, default=5.0)
    parser.add_argument("--stats-file", default="")
    args = parser.parse_args()
    check_nodes = args.check_nodes or args.nodes

    def client_for(i):
        port = 9090 if i == 1 else 9100 + i
        return make_client(api.API, host="127.0.0.1", port=port, proto_factory=TBinaryProtocolFactory(),
                           trans_factory=TBufferedTransportFactory(), timeout=300000)

    master = Account(base58.b58decode(args.master_seed))
    counter = deploy(client_for(1), master, "Counter", COUNTER)
    ledger = deploy(client_for(1), master, "Ledger", LEDGER)
    caller = deploy(client_for(1), master, "Caller", CALLER % base58.b58encode(counter).decode())
    counter_b58 = base58.b58encode(counter).decode()
    declared = deploy(client_for(1), master, "CallerDeclared", CALLER_DECLARED % (counter_b58, counter_b58))
    # contract-to-contract calls fail on mainnet today; what matters is that every node fails them the
    # same way, also a node that restarted from a checkpoint taken after the callee was deployed
    callers = (("Caller", caller), ("CallerDeclared", declared))

    def call_callers(when, via):
        for name, contract in callers:
            call(client_for(via), master, contract, "callCounter", f"{name} contract-to-contract call {when} via n{via}")

    call_callers("before restart", 1)

    failures = 0
    calls = 0
    for n in range(args.calls_before):
        calls += 1
        failures += 0 if call(client_for(1), master, counter, "next", f"call {n + 1} before restart", calls) else 1
        failures += 0 if call(client_for(1), master, ledger, "record", f"Ledger call {calls} before restart", calls) else 1
    def read_all(contract, method, expected, label, timeout=60):
        """The getter on every node, until each returns `expected` (a lagging node needs a round or
        two); prints one line, returns the number of nodes that did not."""
        def one(i):
            deadline = time.time() + timeout
            first_ms = None
            value, msg = None, ""
            while True:
                try:
                    value, ms, msg = read_value(client_for(i), master, contract, method)
                    first_ms = ms if first_ms is None else first_ms
                except Exception as e:  # a node without a reachable API
                    value, msg = None, type(e).__name__
                if value == expected or time.time() >= deadline:
                    return i, value, first_ms, msg
                time.sleep(2)
        with ThreadPoolExecutor(max_workers=check_nodes) as pool:
            results = list(pool.map(one, range(1, check_nodes + 1)))
        bad = [r for r in results if r[1] != expected]
        cells = " ".join(f"n{i}={v}" + (f"/{ms:.0f}ms" if ms is not None else "") for i, v, ms, _ in results)
        print(f"read {label}: {'ok' if not bad else 'FAILED'} (expected {expected}) {cells}", flush=True)
        for i, v, _, msg in bad:
            print(f"  n{i}: {v!r} {msg!r}", flush=True)
        return len(bad)

    failures += read_all(counter, "get", calls, "Counter.get before restart")
    failures += read_all(ledger, "entries", calls, "Ledger.entries before restart")

    # reads change nothing: no transaction on the contract, the same state, the same inner id
    def footprint():
        c = client_for(1)
        got = c.SmartContractGet(counter).smartContract
        return got.transactionsCount, hashlib.sha256(got.objectState).hexdigest(), \
            c.WalletTransactionsCountGet(master.pk).lastTransactionInnerId
    before = footprint()
    for _ in range(20):
        read_value(client_for(1), master, counter, "get")
    after = footprint()
    print(f"20 reads on n1: contract transactions, state, master inner id {before} -> {after}: "
          f"{'unchanged' if before == after else 'CHANGED'}", flush=True)
    failures += 0 if before == after else 1

    if args.ready_file:
        open(args.ready_file, "w").close()
    if args.wait_file:
        deadline = time.time() + args.wait_timeout
        while not os.path.exists(args.wait_file) and time.time() < deadline:
            time.sleep(5)
        print(f"wait file {'found' if os.path.exists(args.wait_file) else 'MISSING'}", flush=True)
    for n in range(args.calls_after):
        calls += 1
        failures += 0 if call(client_for(1), master, counter, "next", f"call {n + 1} after restart", calls) else 1
        failures += 0 if call(client_for(1), master, ledger, "record", f"Ledger call {calls} after restart", calls) else 1
    failures += 0 if call(client_for(1), master, ledger, "entries", "Ledger entries", calls) else 1
    failures += read_all(counter, "get", calls, "Counter.get after restart")
    failures += read_all(ledger, "entries", calls, "Ledger.entries after restart")

    if args.load_seconds > 0:
        writes_ok, writes_failed, writes_rejected, bad_reads = load(args, client_for, master, counter, check_nodes)
        failures += writes_failed + bad_reads
        # a write that failed on its way back (a timeout) may still have landed: the counter is
        # between the confirmed writes and all of them, and every node must read that same value
        time.sleep(30)
        value = None
        for _ in range(10):
            try:
                value = read_value(client_for(1), master, counter, "get")[0]
            except Exception:
                value = None
            if value is not None:
                break
            time.sleep(5)
        low, high = calls + writes_ok, calls + writes_ok + writes_failed + writes_rejected
        in_range = value is not None and low <= value <= high
        print(f"Counter after the load: {value} (confirmed writes give {low}..{high}, unconfirmed ones may land): "
              f"{'ok' if in_range else 'FAILED'}",
              flush=True)
        failures += 0 if in_range else 1
        calls = value if value is not None else calls
        failures += read_all(counter, "get", calls, "Counter.get after the load", timeout=120)
    call_callers("after restart", 1)
    call_callers("after restart", 2)

    if args.final_file:
        deadline = time.time() + args.wait_timeout
        while not os.path.exists(args.final_file) and time.time() < deadline:
            time.sleep(5)
        print(f"final file {'found' if os.path.exists(args.final_file) else 'MISSING'}", flush=True)
    time.sleep(60)  # let the last state reach every node
    for name, contract in (("Counter", counter), ("Ledger", ledger)) + callers:
        states = {}
        for i in range(1, (args.check_nodes or args.nodes) + 1):
            try:
                got = client_for(i).SmartContractGet(contract)
                states[i] = hashlib.sha256(got.smartContract.objectState).hexdigest()[:16] if got.status.code == 0 else "error"
            except Exception as e:  # a node without a reachable API counts as a mismatch
                states[i] = f"unreachable ({type(e).__name__})"
        print(f"{name} state per node: {states}", flush=True)
        if len(set(states.values())) != 1:
            print(f"FAIL: nodes disagree on the {name} state", flush=True)
            failures += 1
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
