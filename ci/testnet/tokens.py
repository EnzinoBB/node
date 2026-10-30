#!/usr/bin/env python3
"""Token smart contracts on the private test network, through the node's API as a wallet uses it.

Usage: tokens.py <work_dir> <nodes> [--from-node N] [--via N] [--json OUT] [--strict-api]

The wallet of node N (default: the last node; fund.py gave it 60'000 CS, and run.sh starts this
script once fund.py's load transfers are over, so nothing else signs with it) funds three wallets
(Alice, Bob, Carol), deploys a v1 token (ExtensionTokenStandard,
BigDecimal amounts, ci/contracts/tokens/TokenV1.java) and a v0 token (ExtensionStandard, string
amounts, TokenV0.java), then runs what token users do: transfers, approve and transferFrom,
calls that must fail, a call with the amount in the type the other standard uses, a CS payment to
the token (payable), a sale back for CS (a transaction the contract emits), burn and register.

Checks:
- consensus: every call returns what the token's rules say, the balances read on every node
  (getters through TransactionFlow with forgetNewState) agree, and every node holds the same state
  of both tokens; the CS of the sale reaches the seller;
- the token API (TokensListGet, TokenInfoGet, TokenBalancesGet, TokenHoldersGet, TokenTransfersGet,
  TokenTransactionsGet): what it reports against the balances the contract holds. A node built
  without the token cache (every build but RelMonitor/RelWallet) answers "This node doesn't provide
  such info"; that is reported. API mismatches are findings; with --strict-api they fail the run.

Transactions go through node --via (default 1; port 9090, node i > 1 answers on 9100 + i), whose
token API is checked: with a node that was restarted, this checks that its API still reports the
results of contract calls after a quick start.
"""
import argparse
import hashlib
import json
import os
import sys
import time

import base58
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory

from contracts import CURRENCY_CS, SMART_MAX_FEE, TT_EXECUTE, deploy, smart_transaction
from fund import Client, api, balance, general, node_account, send
from fund import Account

HERE = os.path.dirname(os.path.abspath(__file__))
TOKENS_DIR = os.path.join(HERE, "..", "contracts", "tokens")


def b58(pk):
    return base58.b58encode(pk).decode()


def v_str(s):
    return general.Variant(v_string=s)


def v_dec(s):
    return general.Variant(v_big_decimal=str(s))


def v_bool(b):
    return general.Variant(v_boolean=b)


def plain(v):
    if v is None:
        return None
    for name in ("v_string", "v_big_decimal", "v_boolean", "v_int", "v_long", "v_double", "v_void"):
        value = getattr(v, name, None)
        if value is not None:
            return value
    return None


class Run:
    def __init__(self, nodes, strict_api, via=1):
        self.via = via
        self.nodes = nodes
        self.strict_api = strict_api
        self.failures = 0
        self.findings = []
        self.log = []

    def client(self, i=None):
        i = self.via if i is None else i
        port = 9090 if i == 1 else 9100 + i
        return make_client(api.API, host="127.0.0.1", port=port, proto_factory=TBinaryProtocolFactory(),
                           trans_factory=TBufferedTransportFactory(), timeout=300000)

    def check(self, ok, what, kind="consensus"):
        """kind: consensus (fails the run), api (a finding; fails with --strict-api), info"""
        tag = "ok" if ok else ("FAILED" if kind == "consensus" or (kind == "api" and self.strict_api) else "FINDING")
        print(f"[{kind}] {what}: {tag}", flush=True)
        self.log.append({"kind": kind, "check": what, "ok": bool(ok)})
        if not ok:
            if tag == "FAILED":
                self.failures += 1
            else:
                self.findings.append(f"[{kind}] {what}")
        return ok

    def invoke(self, src, contract, method, params, label, amount=0):
        """A contract call signed by src; returns (status code, return value, message)."""
        deadline = time.time() + 120
        c = self.client()
        while True:
            last = c.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0
            src.next_inner_id = int(last) + 1
            sci = api.SmartContractInvocation(method=method, params=params, usedContracts=[], forgetNewState=False,
                                              smartContractDeploy=None, version=1)
            result = c.TransactionFlow(smart_transaction(src, contract, sci, TT_EXECUTE))
            msg = result.status.message or ""
            if result.status.code == 0 or ("not syncronized" not in msg and "Transaction in progress" not in msg) \
                    or time.time() >= deadline:
                break
            time.sleep(5)
        value = plain(result.smart_contract_result)
        print(f"  {label}: status {result.status.code} {msg.strip()[:160]!r}, returned {value!r}", flush=True)
        return result.status.code, value, msg

    def read(self, i, src, contract, method, params):
        sci = api.SmartContractInvocation(method=method, params=params, usedContracts=[], forgetNewState=True,
                                          smartContractDeploy=None, version=1)
        t = api.Transaction(id=0, source=src.pk, target=contract, amount=general.Amount(integral=0, fraction=0),
                            balance=general.Amount(integral=0, fraction=0), currency=CURRENCY_CS, signature=bytes(64),
                            smartContract=sci, fee=api.AmountCommission(commission=SMART_MAX_FEE),
                            timeCreation=int(time.time() * 1000), type=TT_EXECUTE)
        r = self.client(i).TransactionFlow(t)
        return plain(r.smart_contract_result) if r.status.code == 0 else None

    def read_everywhere(self, src, contract, method, params, expected, label, timeout=180):
        """The getter on every node until each returns expected."""
        got = {}
        for i in range(1, self.nodes + 1):
            deadline = time.time() + timeout
            while True:
                try:
                    got[i] = self.read(i, src, contract, method, params)
                except Exception as e:
                    got[i] = f"unreachable ({type(e).__name__})"
                if got[i] == expected or time.time() >= deadline:
                    break
                time.sleep(5)
        return self.check(all(v == expected for v in got.values()), f"{label} = {expected!r} on every node (got {got})")


def wrong_type_call(run, src, contract, params, passed, what):
    """A call no method of the contract matches fails on every node and its fee is paid: the API
    refuses it before it reaches the network (a node without that check sends it on)."""
    c = run.client()
    before = c.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0
    s, r, m = run.invoke(src, contract, "transfer", params, what)
    refused = s != 0 and "no method matches" in m and f"the call passes {passed}" in m
    time.sleep(10)
    after = c.WalletTransactionsCountGet(src.pk).lastTransactionInnerId or 0
    run.check(refused and after == before, f"{what}: refused by the API, nothing sent (inner id {before} -> {after})",
              kind="api")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("work")
    parser.add_argument("nodes", type=int)
    parser.add_argument("--from-node", type=int, default=0)
    parser.add_argument("--via", type=int, default=1)
    parser.add_argument("--json", default="")
    parser.add_argument("--strict-api", action="store_true")
    args = parser.parse_args()
    run = Run(args.nodes, args.strict_api, args.via)

    master = node_account(args.work, args.from_node or args.nodes)
    print(f"token owner: the wallet of n{args.from_node or args.nodes}, {b58(master.pk)}", flush=True)
    alice = Account(hashlib.sha256(master.pk + b"token alice").digest())
    bob = Account(hashlib.sha256(master.pk + b"token bob").digest())
    carol = Account(hashlib.sha256(master.pk + b"token carol").digest())
    funder = Client(9090 if args.via == 1 else 9100 + args.via, 300)
    master.next_inner_id = None
    for who in (alice, bob, carol):
        ok, message = send(funder, master, who.pk, 200)
        print(f"fund {b58(who.pk)[:8]}: {'ok' if ok else 'FAILED ' + str(message)}", flush=True)
    deadline = time.time() + 300
    while time.time() < deadline and any(balance(funder, w.pk) < 200 for w in (alice, bob, carol)):
        time.sleep(5)

    with open(os.path.join(TOKENS_DIR, "TokenV1.java")) as f:
        v1_source = f.read()
    with open(os.path.join(TOKENS_DIR, "TokenV0.java")) as f:
        v0_source = f.read()
    v1 = deploy(run.client(), master, "TokenV1", v1_source)
    v0 = deploy(run.client(), master, "TokenV0", v0_source)

    # ---- v1: BigDecimal amounts ---------------------------------------------------------------
    print("TokenV1 calls", flush=True)
    s, r, _ = run.invoke(master, v1, "transfer", [v_str(b58(alice.pk)), v_dec("1000")], "master -> alice 1000")
    run.check(s == 0 and r is True, "v1 transfer with a BigDecimal amount")
    wrong_type_call(run, master, v1, [v_str(b58(bob.pk)), v_str("5")], "v_string",
                    "v1 transfer with the amount as a string (the executor finds no transfer(String, String))")
    s, r, _ = run.invoke(alice, v1, "transfer", [v_str(b58(bob.pk)), v_dec("100.5")], "alice -> bob 100.5")
    run.check(s == 0 and r is True, "v1 transfer by a holder")
    s, r, _ = run.invoke(alice, v1, "transfer", [v_str(b58(bob.pk)), v_dec("999999")], "alice -> bob 999999")
    run.check(r is not True, "v1 transfer above the balance fails")
    s, r, _ = run.invoke(master, v1, "approve", [v_str(b58(bob.pk)), v_dec("50")], "master approves bob 50")
    s, r, _ = run.invoke(bob, v1, "transferFrom", [v_str(b58(master.pk)), v_str(b58(carol.pk)), v_dec("20")],
                         "bob moves 20 master -> carol")
    run.check(s == 0 and r is True, "v1 transferFrom within the allowance")
    s, r, _ = run.invoke(carol, v1, "register", [], "carol registers")
    # a CS payment to the token: the node calls payable(BigDecimal, byte[])
    master.next_inner_id = None
    alice.next_inner_id = None
    ok, message = send(funder, alice, v1, 10)
    print(f"  alice pays 10 CS to TokenV1: {'ok' if ok else 'FAILED ' + str(message)}", flush=True)
    time.sleep(20)
    s, r, _ = run.invoke(bob, v1, "sell", [v_dec("2")], "bob sells 2 tokens for CS")
    run.check(s == 0 and r is True, "v1 sell (the contract emits a CS transaction)")
    s, r, _ = run.invoke(master, v1, "burn", [v_dec("1000")], "master burns 1000")

    expected_v1 = {b58(alice.pk): "909.500", b58(bob.pk): "98.500", b58(carol.pk): "20.000"}
    for who, value in expected_v1.items():
        run.read_everywhere(master, v1, "balanceOf", [v_str(who)], value, f"TokenV1 balanceOf {who[:8]}")
    run.read_everywhere(master, v1, "totalSupply", [], "9999000.000", "TokenV1 totalSupply")

    # ---- v0: string amounts --------------------------------------------------------------------
    print("TokenV0 calls", flush=True)
    s, r, _ = run.invoke(master, v0, "transfer", [v_str(b58(alice.pk)), v_str("10.5")], "master -> alice \"10.5\"")
    run.check(s == 0 and r is True, "v0 transfer with a string amount")
    wrong_type_call(run, master, v0, [v_str(b58(bob.pk)), v_dec("1")], "v_big_decimal",
                    "v0 transfer with the amount as a BigDecimal (the executor finds no transfer(String, BigDecimal))")
    s, r, _ = run.invoke(alice, v0, "approve", [v_str(b58(bob.pk)), v_str("5")], "alice approves bob 5")
    s, r, _ = run.invoke(bob, v0, "transferFrom", [v_str(b58(alice.pk)), v_str(b58(bob.pk)), v_str("5")],
                         "bob moves 5 alice -> bob")
    run.check(s == 0 and r is True, "v0 transferFrom")
    run.read_everywhere(master, v0, "balanceOf", [v_str(b58(alice.pk))], "5.50", "TokenV0 balanceOf alice")
    run.read_everywhere(master, v0, "balanceOf", [v_str(b58(bob.pk))], "5.00", "TokenV0 balanceOf bob")

    # ---- CS of the sale ------------------------------------------------------------------------
    bob_cs = balance(funder, bob.pk)
    run.check(bob_cs > 200, f"the 2 CS of the sale reached Bob (balance {bob_cs})")

    # ---- same state on every node ----------------------------------------------------------------
    time.sleep(30)
    for name, contract in (("TokenV1", v1), ("TokenV0", v0)):
        states = {}
        for i in range(1, args.nodes + 1):
            try:
                got = run.client(i).SmartContractGet(contract)
                states[i] = hashlib.sha256(got.smartContract.objectState).hexdigest()[:16] if got.status.code == 0 else "error"
            except Exception as e:
                states[i] = f"unreachable ({type(e).__name__})"
        run.check(len(set(states.values())) == 1, f"{name} state is the same on every node ({states})")

    # ---- the token API -----------------------------------------------------------------------------
    print(f"token API on node {args.via}", flush=True)
    c = run.client()
    lst = c.TokensListGet(0, 100, 0, False, api.TokenFilters(name="", code="", tokenStandard=0))
    if lst.status.code != 0:
        run.check(False, f"TokensListGet answers ({lst.status.message!r}): the token API needs a node built with the "
                         f"token cache", kind="api")
    else:
        found = {b58(t.address): t for t in (lst.tokens or [])}
        for name, contract, std in (("TokenV1", v1, 4), ("TokenV0", v0, 2)):
            t = found.get(b58(contract))
            run.check(t is not None, f"TokensListGet lists {name}", kind="api")
            if t is not None:
                run.check(t.tokenStandard == std, f"{name} token standard {std} (got {t.tokenStandard})", kind="api")
                run.check(t.code and t.name, f"{name} name and symbol ({t.name!r}, {t.code!r})", kind="api")
        for who, label in ((alice, "alice"), (bob, "bob"), (carol, "carol")):
            tb = c.TokenBalancesGet(who.pk)
            got = {b58(b.token): b.balance for b in (tb.balances or [])}
            print(f"  TokenBalancesGet {label}: {got}", flush=True)
            want = expected_v1.get(b58(who.pk))
            have = got.get(b58(v1))
            run.check(have is not None and want is not None and float(have) == float(want),
                      f"TokenBalancesGet {label}: TokenV1 balance {want} (got {have})", kind="api")
        info = c.TokenInfoGet(v1)
        if info.status.code == 0:
            t = info.token
            print(f"  TokenInfoGet TokenV1: holders {t.holdersCount} transfers {t.transfersCount} "
                  f"transactions {t.transactionsCount} supply {t.totalSupply}", flush=True)
            run.check(t.holdersCount >= 4, f"TokenV1 holders count >= 4 (got {t.holdersCount})", kind="api")
            run.check(t.transfersCount >= 3, f"TokenV1 transfers count >= 3 (got {t.transfersCount})", kind="api")
            run.check(t.totalSupply and float(t.totalSupply) == 9999000.0, f"TokenV1 total supply (got {t.totalSupply!r})",
                      kind="api")
        else:
            run.check(False, f"TokenInfoGet answers ({info.status.message!r})", kind="api")
        tr = c.TokenTransfersGet(v1, 0, 100)
        print(f"  TokenTransfersGet TokenV1: status {tr.status.code}, count {tr.count}, "
              f"listed {len(tr.transfers or [])}", flush=True)
        run.check(tr.status.code == 0 and len(tr.transfers or []) >= 3,
                  f"TokenTransfersGet lists the v1 transfers (got {len(tr.transfers or [])})", kind="api")
        # a page of 2 transfers: the failed transfer in between takes no place in it
        page = c.TokenTransfersGet(v1, 0, 2)
        run.check(len(page.transfers or []) == 2, f"TokenTransfersGet with limit 2 lists 2 transfers (got {len(page.transfers or [])})",
                  kind="api")
        if tr.transfers:
            one = c.TokenTransferGet(v1, tr.transfers[0].transaction)
            run.check(one.status.code == 0 and len(one.transfers or []) == 1 and one.count == 1,
                      "TokenTransferGet of a transfer returns it", kind="api")
        txs = c.TokenTransactionsGet(v1, 0, 100)
        approve = next((t for t in (txs.transactions or []) if t.method == "approve"), None)
        if approve is not None:
            other = c.TokenTransferGet(v1, approve.transaction)
            run.check(other.status.code != 0 and not other.transfers, "TokenTransferGet of an approve() is not a transfer",
                      kind="api")
        holders = c.TokenHoldersGet(v1, 0, 100, 0, True)
        print(f"  TokenHoldersGet TokenV1: status {holders.status.code} "
              f"{[(b58(h.holder)[:8], h.balance) for h in (holders.holders or [])]}", flush=True)
        run.check(holders.status.code == 0 and len(holders.holders or []) >= 4, "TokenHoldersGet lists the v1 holders",
                  kind="api")

    print(f"\n{run.failures} failures, {len(run.findings)} findings", flush=True)
    for f in run.findings:
        print("  " + f)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(run.log, f, indent=1)
    sys.exit(1 if run.failures else 0)


if __name__ == "__main__":
    main()
