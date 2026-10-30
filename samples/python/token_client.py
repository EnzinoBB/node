#!/usr/bin/env python3
"""Call a Credits smart contract from the command line, with arguments of the types it declares.

  token_client.py [--api 127.0.0.1:9090] methods <contract>
  token_client.py [--api ...] get  <contract> <method> [args...]           read, free, nothing is sent
  token_client.py [--api ...] --seed <base58 private key> call <contract> <method> [args...]

Arguments are given as text and converted to the type the method declares, which the executor
requires (a BigDecimal amount must be a v_big_decimal, not a v_string): String, BigDecimal, int,
long, boolean, double and their wrappers, byte[] as hex. With overloads, the method with as many
arguments as given is used.

  token_client.py methods 9xJ...TOKEN
  token_client.py get 9xJ...TOKEN balanceOf 4Fv...WALLET
  token_client.py --seed 3Ab...KEY call 9xJ...TOKEN transfer 4Fv...WALLET 12.5

Needs thriftpy2, base58 and pynacl (pip install thriftpy2 base58 pynacl) and the Thrift definitions
of the node (third-party/thrift-interface-definitions, or --idl).
"""
import argparse
import math
import os
import struct
import sys
import time

import base58
import nacl.signing
import thriftpy2
from thriftpy2.protocol import TBinaryProtocolFactory
from thriftpy2.rpc import make_client
from thriftpy2.transport import TBufferedTransportFactory
from thriftpy2.utils import serialize

HERE = os.path.dirname(os.path.abspath(__file__))
CURRENCY_CS = 1
TT_EXECUTE = 2  # api.TransactionType


def encode_max_fee(value):
    """csdb::AmountCommission(double) as its uint16 bit pattern."""
    v = abs(value)
    expf = math.log10(v)
    expi = int(expf + 0.5) if expf >= 0 else int(expf - 0.5)
    v /= 10 ** expi
    if v >= 1.0:
        v *= 0.1
        expi += 1
    return (((expi + 18) & 0x1F) << 10) | (int(round(v * 1024)) & 0x3FF)


def variant(general, declared, text):
    """The Variant the executor matches with a declared Java type."""
    converters = {
        "java.lang.String": lambda t: general.Variant(v_string=t),
        "java.math.BigDecimal": lambda t: general.Variant(v_big_decimal=str(t)),
        "int": lambda t: general.Variant(v_int=int(t)),
        "java.lang.Integer": lambda t: general.Variant(v_int_box=int(t)),
        "long": lambda t: general.Variant(v_long=int(t)),
        "java.lang.Long": lambda t: general.Variant(v_long_box=int(t)),
        "boolean": lambda t: general.Variant(v_boolean=t.lower() in ("true", "1", "yes")),
        "java.lang.Boolean": lambda t: general.Variant(v_boolean_box=t.lower() in ("true", "1", "yes")),
        "double": lambda t: general.Variant(v_double=float(t)),
        "java.lang.Double": lambda t: general.Variant(v_double_box=float(t)),
        "byte[]": lambda t: general.Variant(v_byte_array=bytes.fromhex(t)),
    }
    if declared not in converters:
        sys.exit(f"arguments of type {declared} are not supported by this client")
    return converters[declared](text)


def plain(v):
    if v is None:
        return None
    for name in ("v_string", "v_big_decimal", "v_boolean", "v_boolean_box", "v_int", "v_int_box", "v_long",
                 "v_long_box", "v_double", "v_double_box", "v_byte", "v_short"):
        value = getattr(v, name, None)
        if value is not None:
            return value
    if v.v_byte_array is not None:
        return v.v_byte_array.hex()
    if v.v_void is not None:
        return "(void)"
    return v


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--api", default="127.0.0.1:9090", help="host:port of the node's Thrift API")
    parser.add_argument("--idl", default=os.path.join(HERE, "..", "..", "third-party", "thrift-interface-definitions"))
    parser.add_argument("--seed", help="base58 private key (64 bytes, or its 32-byte seed) that signs a call")
    parser.add_argument("--max-fee", type=float, default=1.0, help="most CS the call may cost (default 1)")
    parser.add_argument("command", choices=["methods", "get", "call"])
    parser.add_argument("contract")
    parser.add_argument("method", nargs="?")
    parser.add_argument("args", nargs="*")
    a = parser.parse_args()

    general = thriftpy2.load(os.path.join(a.idl, "general.thrift"), module_name="general_thrift", include_dirs=[a.idl])
    api = thriftpy2.load(os.path.join(a.idl, "api.thrift"), module_name="api_thrift", include_dirs=[a.idl])
    host, port = a.api.rsplit(":", 1)
    client = make_client(api.API, host=host, port=int(port), proto_factory=TBinaryProtocolFactory(),
                         trans_factory=TBufferedTransportFactory(), timeout=120000)
    contract = base58.b58decode(a.contract)

    # the methods of the contract: its byte code (SmartContractGet), listed by the node's executor
    deployed = client.SmartContractGet(contract)
    if deployed.status.code != 0:
        sys.exit(f"SmartContractGet: {deployed.status.message}")
    methods = client.ContractAllMethodsGet(deployed.smartContract.smartContractDeploy.byteCodeObjects)
    if methods.code != 0:
        sys.exit(f"ContractAllMethodsGet: {methods.message}")
    if a.command == "methods":
        for m in sorted(methods.methods, key=lambda m: m.name):
            print(f"  {m.returnType} {m.name}({', '.join(arg.type + ' ' + arg.name for arg in m.arguments)})")
        return

    candidates = [m for m in methods.methods if m.name == a.method and len(m.arguments) == len(a.args)]
    if not candidates:
        known = [f"{m.name}({', '.join(x.type for x in m.arguments)})" for m in methods.methods if m.name == a.method]
        sys.exit(f"no method {a.method} with {len(a.args)} arguments" + (f"; there is {', '.join(known)}" if known else ""))
    method = candidates[0]
    params = [variant(general, arg.type, text) for arg, text in zip(method.arguments, a.args)]

    if a.command == "get":
        # TransactionFlow with forgetNewState: run by the node's executor on the current state, nothing
        # is sent and no fee is paid; no signature is checked
        sci = api.SmartContractInvocation(method=a.method, params=params, usedContracts=[], forgetNewState=True,
                                          smartContractDeploy=None, version=1)
        source = bytes(32) if not a.seed else nacl.signing.SigningKey(base58.b58decode(a.seed)[:32]).verify_key.encode()
        t = api.Transaction(id=0, source=source, target=contract, amount=general.Amount(integral=0, fraction=0),
                            balance=general.Amount(integral=0, fraction=0), currency=CURRENCY_CS, signature=bytes(64),
                            smartContract=sci, fee=api.AmountCommission(commission=encode_max_fee(a.max_fee)),
                            timeCreation=int(time.time() * 1000), type=TT_EXECUTE)
        r = client.TransactionFlow(t)
        if r.status.code != 0:
            sys.exit(r.status.message)
        print(plain(r.smart_contract_result))
        return

    if not a.seed:
        sys.exit("call needs --seed: the call is a transaction signed by its sender")
    key = nacl.signing.SigningKey(base58.b58decode(a.seed)[:32])
    pk = key.verify_key.encode()
    inner_id = int(client.WalletTransactionsCountGet(pk).lastTransactionInnerId or 0) + 1
    max_fee = encode_max_fee(a.max_fee)
    # every non-optional field set, so that the bytes signed are those the node serializes
    sci = api.SmartContractInvocation(method=a.method, params=params, usedContracts=[], forgetNewState=False,
                                      smartContractDeploy=None, version=1)
    code = serialize(sci, TBinaryProtocolFactory())
    signed = b"".join([
        inner_id.to_bytes(6, "little"), pk, contract,
        struct.pack("<iQ", 0, 0),          # amount
        struct.pack("<H", max_fee),
        struct.pack("<B", CURRENCY_CS),
        struct.pack("<BI", 1, len(code)) + code,  # one user field: the invocation
    ])
    t = api.Transaction(id=inner_id, source=pk, target=contract, amount=general.Amount(integral=0, fraction=0),
                        balance=general.Amount(integral=0, fraction=0), currency=CURRENCY_CS,
                        signature=key.sign(signed).signature, smartContract=sci,
                        fee=api.AmountCommission(commission=max_fee), timeCreation=int(time.time() * 1000), type=TT_EXECUTE)
    r = client.TransactionFlow(t)
    print(f"status {r.status.code}: {r.status.message.strip()}")
    if r.status.code == 0:
        print(f"returned {plain(r.smart_contract_result)!r}, transaction {r.id.poolSeq}.{r.id.index}")
    sys.exit(0 if r.status.code == 0 else 1)


if __name__ == "__main__":
    main()
