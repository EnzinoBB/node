#!/usr/bin/env python3
"""Run token smart contracts on a contract executor without a node, and compare executors.

Usage: executor_harness.py --jar contract-executor.jar [--jar other.jar ...] --jdk JDK_DIR
                           [--work DIR] [--json OUT] [--only NAME,...] [--java-opt OPT ...]

For every jar the harness starts the executor with a settings.properties of its own and a stub of
the node's APIEXEC service (GetSeed, SmartContractGet, WalletBalanceGet, GetDateTime: fixed,
deterministic answers), then runs the scenarios in token_scenarios.py through the executor's
Thrift interface the way the node does: deploy (executeByteCode with an empty state), calls
(executeByteCode with the last state), getters (executeByteCodeMultiple, state not kept) and
getContractMethods (the token standard the API records).

Each step records the status code, the return value, a hash of every contract state the executor
returns and the emitted transactions. The first jar is the reference (the release executor): with
two or more jars, any answer that differs is a FAIL, because the node accepts a contract result
only if the confidants' executors agree. What only the node's API reads (the token standard of
getContractMethods) may differ. Every scenario also checks its own expectations
(an expectation that does not hold is reported as a finding, not as a difference).
"""
import argparse
import hashlib
import json
import os
import socket
import subprocess
import sys
import threading
import time

import base58
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client, make_server
from thriftpy2.transport import TBufferedTransportFactory

HERE = os.path.dirname(os.path.abspath(__file__))
IDL = os.path.abspath(os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
general = thriftpy2.load(os.path.join(IDL, "general.thrift"), module_name="general_thrift", include_dirs=[IDL])
api = thriftpy2.load(os.path.join(IDL, "api.thrift"), module_name="api_thrift", include_dirs=[IDL])
executor = thriftpy2.load(os.path.join(IDL, "executor.thrift"), module_name="executor_thrift", include_dirs=[IDL])
apiexec = thriftpy2.load(os.path.join(IDL, "apiexec.thrift"), module_name="apiexec_thrift", include_dirs=[IDL])

EXECUTOR_VERSION = 4          # cs::Executor::EXECUTOR_VERSION
EXECUTION_TIME = 10000        # ms; the node uses Consensus::TimeSmartContract (2 s), a cold JVM needs more
BLOCK_TIME_MS = 1_700_000_000_000


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def key(name):
    """A deterministic 32-byte address for a test actor."""
    return hashlib.sha256(name.encode()).digest()


def b58(addr):
    return base58.b58encode(addr).decode()


# ---------------------------------------------------------------------------------------------
# Variant helpers
# ---------------------------------------------------------------------------------------------
def v_str(s):
    return general.Variant(v_string=s)


def v_dec(s):
    return general.Variant(v_big_decimal=str(s))


def v_bytes(b):
    return general.Variant(v_byte_array=b)


def v_bool(b):
    return general.Variant(v_boolean=b)


def v_int(i):
    return general.Variant(v_int=i)


def variant_value(v):
    """A plain, JSON friendly value of a Variant (None for an unset one)."""
    if v is None:
        return None
    for name in ("v_null", "v_void", "v_boolean", "v_boolean_box", "v_byte", "v_byte_box", "v_short",
                 "v_short_box", "v_int", "v_int_box", "v_long", "v_long_box", "v_float", "v_float_box",
                 "v_double", "v_double_box", "v_string", "v_big_decimal"):
        value = getattr(v, name, None)
        if value is not None:
            return {name: value}
    if v.v_byte_array is not None:
        return {"v_byte_array": v.v_byte_array.hex()}
    if v.v_amount is not None:
        return {"v_amount": [v.v_amount.integral, v.v_amount.fraction]}
    for name in ("v_array", "v_list"):
        value = getattr(v, name, None)
        if value is not None:
            return {name: [variant_value(x) for x in value]}
    if v.v_map is not None:
        return {"v_map": sorted([json.dumps(variant_value(k)), json.dumps(variant_value(x))] for k, x in v.v_map.items())}
    if v.v_object is not None:
        return {"v_object": v.v_object.nameClass}
    return {}


def plain(v):
    """The bare value of a Variant: a string, a number or a bool."""
    value = variant_value(v) or {}
    return next(iter(value.values()), None)


def first_line(message, limit=300):
    message = (message or "").strip()
    # the executor sends the root cause stack trace; its first line is the exception
    return message.splitlines()[0][:limit] if message else ""


# ---------------------------------------------------------------------------------------------
# Stub of the node's APIEXEC service
# ---------------------------------------------------------------------------------------------
class NodeStub:
    """What a contract can ask the node: answers are fixed, so every executor sees the same."""

    def __init__(self):
        self.contracts = {}   # address -> (byteCodeObjects, state)
        self.balances = {}    # address -> (integral, fraction)
        self.calls = []

    def GetSeed(self, accessId):
        self.calls.append("GetSeed")
        return apiexec.GetSeedResult(status=general.APIResponse(code=0, message=""),
                                     seed=hashlib.sha256(b"seed%d" % accessId).digest())

    def SendTransaction(self, accessId, transaction):
        self.calls.append("SendTransaction")
        return apiexec.SendTransactionResult(status=general.APIResponse(code=0, message=""))

    def SmartContractGet(self, accessId, address):
        self.calls.append("SmartContractGet " + b58(address))
        if address not in self.contracts:
            return apiexec.SmartContractGetResult(status=general.APIResponse(code=1, message="no contract"),
                                                  byteCodeObjects=[], contractState=b"", stateCanModify=False)
        bco, state = self.contracts[address]
        return apiexec.SmartContractGetResult(status=general.APIResponse(code=0, message=""),
                                              byteCodeObjects=bco, contractState=state, stateCanModify=True)

    def WalletIdGet(self, accessId, address):
        self.calls.append("WalletIdGet")
        return api.WalletIdGetResult(status=general.APIResponse(code=0, message=""), walletId=1)

    def WalletBalanceGet(self, address):
        self.calls.append("WalletBalanceGet")
        integral, fraction = self.balances.get(address, (0, 0))
        return api.WalletBalanceGetResult(status=general.APIResponse(code=0, message=""),
                                          balance=general.Amount(integral=integral, fraction=fraction))

    def PoolGet(self, sequence):
        return apiexec.PoolGetResult(status=general.APIResponse(code=1, message="not in the stub"), pool=b"")

    def GetDateTime(self, accessId):
        self.calls.append("GetDateTime")
        return apiexec.GetDateTimeResult(status=general.APIResponse(code=0, message=""), timestamp=BLOCK_TIME_MS)


# ---------------------------------------------------------------------------------------------
# One executor process
# ---------------------------------------------------------------------------------------------
class Executor:
    def __init__(self, jar, jdk, work, java_opts=()):
        self.java_opts = list(java_opts)
        self.jar = os.path.abspath(jar)
        self.jdk = os.path.abspath(jdk)
        self.work = work
        self.stub = NodeStub()
        self.node_port = free_port()
        self.port = free_port()
        self.proc = None
        self.server = None
        self.access_id = 1000

    def start(self):
        os.makedirs(self.work, exist_ok=True)
        with open(os.path.join(self.work, "settings.properties"), "w") as f:
            f.write(f"node.api.host=127.0.0.1\nnode.api.port=1\ncontract.executor.port={self.port}\n"
                    f"contract.executor.node.api.port={self.node_port}\ncontract.executor.node.api.host=127.0.0.1\n"
                    f"contract.executor.read.client.timeout=0\njdk.path={self.jdk}\n")
        self.server = make_server(apiexec.APIEXEC, self.stub, "127.0.0.1", self.node_port,
                                  proto_factory=TBinaryProtocolFactory(), trans_factory=TBufferedTransportFactory())
        threading.Thread(target=self.server.serve, daemon=True).start()
        self.log = open(os.path.join(self.work, "executor.log"), "w")
        self.proc = subprocess.Popen([os.path.join(self.jdk, "bin", "java"), "-Xmx512m", *self.java_opts, "-jar", self.jar],
                                     cwd=self.work, stdout=self.log, stderr=subprocess.STDOUT)
        deadline = time.time() + 90
        while time.time() < deadline:
            if self.proc.poll() is not None:
                raise RuntimeError(f"executor {self.jar} exited, see {self.work}/executor.log")
            try:
                c = self.client()
                c.getExecutorBuildVersion(EXECUTOR_VERSION)
                c.close()
                return
            except Exception:
                time.sleep(1)
        raise RuntimeError(f"executor {self.jar} did not answer in 90 s")

    def client(self, timeout=120000):
        return make_client(executor.ContractExecutor, "127.0.0.1", self.port, proto_factory=TBinaryProtocolFactory(),
                           trans_factory=TBufferedTransportFactory(), timeout=timeout)

    def stop(self):
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self.server:
            try:
                self.server.close()
            except Exception:
                pass

    def restart(self):
        self.stop()
        self.stub = NodeStub()
        self.node_port = free_port()
        self.port = free_port()
        self.start()

    def next_access_id(self):
        self.access_id += 1
        return self.access_id


# ---------------------------------------------------------------------------------------------
# A contract under test, driven like the node drives it
# ---------------------------------------------------------------------------------------------
class Contract:
    def __init__(self, ex, name, source, deployer, recorder):
        self.ex = ex
        self.name = name
        self.source = source
        self.deployer = deployer
        self.address = key("contract:" + name)
        self.bco = None
        self.state = b""
        self.rec = recorder
        self.methods = None
        self.token_standard = None

    def compile(self):
        c = self.ex.client()
        r = c.compileSourceCode(self.source, EXECUTOR_VERSION)
        c.close()
        self.rec.step(f"{self.name}: compile", status=r.status.code, error=first_line(r.status.message),
                      classes=sorted(b.name for b in (r.byteCodeObjects or [])))
        if r.status.code != 0:
            return False
        self.bco = r.byteCodeObjects
        return True

    def get_methods(self):
        c = self.ex.client()
        r = c.getContractMethods(self.bco, EXECUTOR_VERSION)
        c.close()
        self.methods = {m.name: [a.type for a in m.arguments] for m in (r.methods or [])}
        self.token_standard = r.tokenStandard
        self.rec.step(f"{self.name}: getContractMethods", status=r.status.code, tokenStandard=r.tokenStandard,
                      methods=sorted(f"{m.name}({','.join(a.type for a in m.arguments)})" for m in (r.methods or [])))
        return r

    def _binary(self, state, can_modify=True):
        return executor.SmartContractBinary(contractAddress=self.address,
                                            object=general.ClassObject(byteCodeObjects=self.bco, instance=state),
                                            stateCanModify=can_modify)

    def deploy(self, timeout=None):
        if self.bco is None and not self.compile():
            return None
        c = self.ex.client(timeout or 120000)
        try:
            r = c.executeByteCode(self.ex.next_access_id(), self.deployer, self._binary(b""), [], EXECUTION_TIME,
                                  EXECUTOR_VERSION)
        except Exception as e:
            self.rec.step(f"{self.name}: deploy", transport_error=type(e).__name__)
            return None
        finally:
            c.close()
        return self._take(r, "deploy")

    def call(self, initiator, method, *params, label=None):
        """A transaction calling method: the node keeps the new state only when the call succeeds."""
        c = self.ex.client()
        try:
            r = c.executeByteCode(self.ex.next_access_id(), initiator, self._binary(self.state),
                                  [executor.MethodHeader(methodName=method, params=list(params))], EXECUTION_TIME,
                                  EXECUTOR_VERSION)
        finally:
            c.close()
        return self._take(r, label or f"{method}({', '.join(json.dumps(plain(p)) for p in params)})")

    def _take(self, r, label):
        if r.status.code != 0 or not r.results:
            self.rec.step(f"{self.name}: {label}", status=r.status.code, error=first_line(r.status.message))
            return {"ok": False, "status": r.status.code, "error": first_line(r.status.message)}
        res = r.results[0]
        states = {b58(a): hashlib.sha256(s).hexdigest()[:16] for a, s in (res.contractsState or {}).items()}
        emitted = [(b58(t.source), b58(t.target), t.amount.integral, t.amount.fraction) for t in (res.emittedTransactions or [])]
        ok = res.status.code == 0
        if ok:
            new_state = (res.contractsState or {}).get(self.address)
            if new_state:
                self.state = new_state
        self.rec.step(f"{self.name}: {label}", status=res.status.code, ret=variant_value(res.ret_val),
                      error=first_line(res.status.message) if not ok else "", states=states, emitted=emitted)
        return {"ok": ok, "status": res.status.code, "ret": plain(res.ret_val), "error": first_line(res.status.message),
                "states": states, "emitted": emitted, "raw_states": res.contractsState or {}}

    def get(self, method, *params, initiator=None, label=None, state=None):
        """A getter the way the API runs it: executeByteCodeMultiple, the state is not kept."""
        return self.get_many(method, [list(params)], initiator=initiator, label=label, state=state)[0]

    def get_many(self, method, params_list, initiator=None, label=None, state=None):
        c = self.ex.client()
        try:
            r = c.executeByteCodeMultiple(self.ex.next_access_id(), initiator or self.deployer,
                                          self._binary(self.state if state is None else state, False), method,
                                          params_list, EXECUTION_TIME, EXECUTOR_VERSION)
        finally:
            c.close()
        out = []
        if r.status.code != 0:
            self.rec.step(f"{self.name}: get {label or method}", status=r.status.code, error=first_line(r.status.message))
            return [{"ok": False, "error": first_line(r.status.message)}] * len(params_list)
        for res in r.results:
            out.append({"ok": res.status.code == 0, "ret": plain(res.ret_val), "error": first_line(res.status.message)})
        self.rec.step(f"{self.name}: get {label or method}", results=[[o["ok"], o["ret"] if o["ok"] else o["error"]] for o in out])
        return out

    def publish(self):
        """Make the current state visible to SmartContractGet (contract-to-contract calls)."""
        self.ex.stub.contracts[self.address] = (self.bco, self.state)


class Recorder:
    def __init__(self):
        self.steps = []
        self.findings = []
        self.scenario = ""

    def step(self, label, **data):
        self.steps.append({"scenario": self.scenario, "step": label, **data})

    def expect(self, condition, finding, severity="bug"):
        """An expectation of a scenario; a failed one is a finding about the executor's behaviour."""
        self.steps.append({"scenario": self.scenario, "expect": finding, "holds": bool(condition)})
        if not condition:
            self.findings.append({"scenario": self.scenario, "severity": severity, "finding": finding})
        return condition


# what only the node's API reads, never contract execution: an executor may improve it
API_ONLY = ("tokenStandard",)


def comparable(step):
    """What two executors must answer alike: the answers of deploys, calls and getters. The
    scenarios' own expectations are left out (a fixed executor meets more of them), and so are the
    fields only the API reads."""
    if "expect" in step:
        return None
    return json.dumps({k: v for k, v in step.items() if k not in API_ONLY}, sort_keys=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--jar", action="append", required=True)
    parser.add_argument("--jdk", required=True)
    parser.add_argument("--work", default="executor-harness-run")
    parser.add_argument("--json", default="")
    parser.add_argument("--only", default="")
    parser.add_argument("--java-opt", action="append", default=[],
                        help="a JVM option for every executor, e.g. -Dcontract.executor.unbounded.code.hold.ms=20000")
    args = parser.parse_args()

    sys.path.insert(0, HERE)
    import token_scenarios

    scenarios = token_scenarios.SCENARIOS
    if args.only:
        wanted = set(args.only.split(","))
        scenarios = [s for s in scenarios if s.__name__ in wanted]

    runs = []
    for n, jar in enumerate(args.jar):
        ex = Executor(jar, args.jdk, os.path.join(os.path.abspath(args.work), f"executor{n}"), args.java_opt)
        ex.start()
        c = ex.client()
        v = c.getExecutorBuildVersion(EXECUTOR_VERSION)
        c.close()
        print(f"== executor {n}: {jar} (commit {v.commitNumber} {v.commitHash[:10]})", flush=True)
        rec = Recorder()
        try:
            for scenario in scenarios:
                rec.scenario = scenario.__name__
                print(f"-- {scenario.__name__}: {(scenario.__doc__ or '').strip().splitlines()[0]}", flush=True)
                try:
                    scenario(ex, rec, Contract)
                except Exception as e:  # a scenario that breaks is a finding too
                    rec.step("scenario aborted", error=f"{type(e).__name__}: {e}")
                    rec.findings.append({"scenario": scenario.__name__, "severity": "harness",
                                         "finding": f"aborted: {type(e).__name__}: {e}"})
                    if ex.proc.poll() is not None:
                        ex.restart()
        finally:
            ex.stop()
        runs.append({"jar": jar, "commit": v.commitNumber, "steps": rec.steps, "findings": rec.findings})

    failures = 0
    ref = runs[0]
    for run in runs:
        print(f"\n== findings with {run['jar']}")
        for f in run["findings"]:
            print(f"  [{f['severity']}] {f['scenario']}: {f['finding']}")
    for other in runs[1:]:
        a = [c for c in map(comparable, ref["steps"]) if c is not None]
        b = [c for c in map(comparable, other["steps"]) if c is not None]
        diffs = [(i, x, y) for i, (x, y) in enumerate(zip(a, b)) if x != y]
        if len(a) != len(b):
            diffs.append((min(len(a), len(b)), f"{len(a)} steps", f"{len(b)} steps"))
        print(f"\n== {other['jar']} against {ref['jar']}: {len(diffs)} differing steps")
        for i, x, y in diffs[:40]:
            print(f"  step {i}:\n    ref:   {x[:400]}\n    other: {y[:400]}")
        failures += 1 if diffs else 0
    if args.json:
        with open(args.json, "w") as f:
            json.dump(runs, f, indent=1)
    sys.exit(1 if failures else 0)


if __name__ == "__main__":
    main()
