#!/usr/bin/env python3
"""Deploy smart contracts on the private test network and call them across the restart test.

Usage: contracts.py <work_dir> <nodes> <master_seed_b58> [--calls-before 3] [--calls-after 5]
                    [--ready-file PATH] [--wait-file PATH] [--wait-timeout 1800]

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

Transactions go through node 1's API (port 9090); the final check reads every node's API
(node i > 1 listens on 9100 + i).
"""
import argparse
import hashlib
import os
import struct
import sys
import time

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory
from thriftpy2.utils import serialize

from fund import Account, api, encode_max_fee, general, pack_inner_id

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


def write_sources(directory):
    """Write the test contracts as Java files, for a compile check before the network starts."""
    placeholder = "11111111111111111111111111111111"
    with open(os.path.join(directory, "Counter.java"), "w") as f:
        f.write(COUNTER)
    with open(os.path.join(directory, "Caller.java"), "w") as f:
        f.write(CALLER % placeholder)


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


def next_inner_id(client, account):
    last = client.WalletTransactionsCountGet(account.pk).lastTransactionInnerId or 0
    account.next_inner_id = int(last) + 1


def deploy(client, master, name, source):
    compiled = client.SmartContractCompile(source)
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
    next_inner_id(client, master)
    sci = invocation(method=method)
    result = client.TransactionFlow(smart_transaction(master, contract, sci, TT_EXECUTE))
    value = result.smart_contract_result.v_int if result.smart_contract_result else None
    ok = result.status.code == 0 and value == expected
    outcome = ("ok" if ok else "FAILED") if expected is not None else "reported"
    print(f"{label}: {outcome} (status {result.status.code} {result.status.message!r}, returned {value})", flush=True)
    return ok


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
    args = parser.parse_args()

    def client_for(i):
        port = 9090 if i == 1 else 9100 + i
        return make_client(api.API, host="127.0.0.1", port=port, proto_factory=TBinaryProtocolFactory(),
                           trans_factory=TBufferedTransportFactory(), timeout=300000)

    master = Account(base58.b58decode(args.master_seed))
    counter = deploy(client_for(1), master, "Counter", COUNTER)
    caller = deploy(client_for(1), master, "Caller", CALLER % base58.b58encode(counter).decode())
    call(client_for(1), master, caller, "callCounter", "contract-to-contract call (known not to work)")

    failures = 0
    calls = 0
    for n in range(args.calls_before):
        calls += 1
        failures += 0 if call(client_for(1), master, counter, "next", f"call {n + 1} before restart", calls) else 1
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

    time.sleep(60)  # let the last state reach every node
    states = {}
    for i in range(1, args.nodes + 1):
        try:
            got = client_for(i).SmartContractGet(counter)
            states[i] = hashlib.sha256(got.smartContract.objectState).hexdigest()[:16] if got.status.code == 0 else "error"
        except Exception as e:  # a node without a reachable API counts as a mismatch
            states[i] = f"unreachable ({type(e).__name__})"
    print(f"Counter state per node: {states}", flush=True)
    if len(set(states.values())) != 1:
        print("FAIL: nodes disagree on the Counter state", flush=True)
        failures += 1
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
