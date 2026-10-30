"""Token scenarios for executor_harness.py.

Each scenario gets the executor, the recorder and the Contract class. It drives contracts the way
the node does and states what a token user expects with rec.expect(): an expectation that does not
hold is a finding about the platform (the executor, or the way the node and the API use it).
"""
import os

from executor_harness import b58, key, plain, v_bool, v_bytes, v_dec, v_str

HERE = os.path.dirname(os.path.abspath(__file__))

OWNER = key("owner")
ALICE = key("alice")
BOB = key("bob")
CAROL = key("carol")


def source(name):
    with open(os.path.join(HERE, "tokens", name + ".java")) as f:
        return f.read()


BASIC_V0 = """import com.credits.scapi.v0.BasicStandard;
import com.credits.scapi.v0.SmartContract;

public class BasicV0 extends SmartContract implements BasicStandard {
    public BasicV0() { super(); }
    public String getName() { return "Basic V0"; }
    public String getSymbol() { return "BV0"; }
    public int getDecimal() { return 0; }
    public boolean setFrozen(boolean frozen) { return false; }
    public String totalSupply() { return "1"; }
    public String balanceOf(String owner) { return "0"; }
    public String allowance(String owner, String spender) { return "0"; }
    public boolean transfer(String to, String amount) { return true; }
    public boolean transferFrom(String from, String to, String amount) { return true; }
    public void approve(String spender, String amount) { }
    public boolean burn(String amount) { return true; }
}
"""

BASIC_V1 = """import com.credits.scapi.v1.BasicTokenStandard;
import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class BasicV1 extends SmartContract implements BasicTokenStandard {
    public BasicV1() { super(); }
    public String getName() { return "Basic V1"; }
    public String getSymbol() { return "BV1"; }
    public int getDecimal() { return 0; }
    public boolean setFrozen(boolean frozen) { return false; }
    public BigDecimal totalSupply() { return BigDecimal.ONE; }
    public BigDecimal balanceOf(String owner) { return BigDecimal.ZERO; }
    public BigDecimal allowance(String owner, String spender) { return BigDecimal.ZERO; }
    public boolean transfer(String to, BigDecimal amount) { return true; }
    public boolean transferFrom(String from, String to, BigDecimal amount) { return true; }
    public void approve(String spender, BigDecimal amount) { }
    public boolean burn(BigDecimal amount) { return true; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# the v2 token standard of sc-api-support: balances exposed as an ObservableMap
BASIC_V2 = """import com.credits.scapi.v2.BasicTokenStandard;
import com.credits.scapi.v2.MapChangeListener;
import com.credits.scapi.v2.ObservableMap;
import com.credits.scapi.v2.WalletAddress;
import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;
import java.util.HashMap;

public class BasicV2 extends SmartContract implements BasicTokenStandard {
    public static class Balances extends HashMap<WalletAddress, BigDecimal> implements ObservableMap<WalletAddress, BigDecimal> {
        public void addListener(MapChangeListener<? super WalletAddress, ? super BigDecimal> l) { }
        public void removeListener(MapChangeListener<? super WalletAddress, ? super BigDecimal> l) { }
    }
    private final Balances balances = new Balances();
    public BasicV2() { super(); }
    public ObservableMap<WalletAddress, BigDecimal> getTokenBalances() { return balances; }
    public String getName() { return "Basic V2"; }
    public String getSymbol() { return "BV2"; }
    public int getDecimal() { return 0; }
    public boolean setFrozen(boolean frozen) { return false; }
    public BigDecimal totalSupply() { return BigDecimal.ONE; }
    public BigDecimal balanceOf(String owner) { return BigDecimal.ZERO; }
    public BigDecimal allowance(String owner, String spender) { return BigDecimal.ZERO; }
    public boolean transfer(String to, BigDecimal amount) { return true; }
    public boolean transferFrom(String from, String to, BigDecimal amount) { return true; }
    public void approve(String spender, BigDecimal amount) { }
    public boolean burn(BigDecimal amount) { return true; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# a token whose standard comes from a base class, as code reuse makes common elsewhere
INHERITED = """import com.credits.scapi.v1.BasicTokenStandard;
import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class Inherited extends TokenBase {
    public Inherited() { super(); }
    public String getName() { return "Inherited"; }
    public String getSymbol() { return "INH"; }
}

abstract class TokenBase extends SmartContract implements BasicTokenStandard {
    public int getDecimal() { return 0; }
    public boolean setFrozen(boolean frozen) { return false; }
    public BigDecimal totalSupply() { return BigDecimal.ONE; }
    public BigDecimal balanceOf(String owner) { return BigDecimal.ZERO; }
    public BigDecimal allowance(String owner, String spender) { return BigDecimal.ZERO; }
    public boolean transfer(String to, BigDecimal amount) { return true; }
    public boolean transferFrom(String from, String to, BigDecimal amount) { return true; }
    public void approve(String spender, BigDecimal amount) { }
    public boolean burn(BigDecimal amount) { return true; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# the reserved name check of the deploy, for every token interface
def reserved(cls, iface, smart, sig_amount):
    imports = {"v0": "import com.credits.scapi.v0.BasicStandard;",
               "v1": "import com.credits.scapi.v1.BasicTokenStandard;",
               "v2": "import com.credits.scapi.v2.BasicTokenStandard;\nimport com.credits.scapi.v2.ObservableMap;\nimport com.credits.scapi.v2.WalletAddress;"}[iface]
    name = {"v0": "BasicStandard", "v1": "BasicTokenStandard", "v2": "BasicTokenStandard"}[iface]
    t = sig_amount
    ret = "String" if t == "String" else "BigDecimal"
    zero = '"0"' if t == "String" else "BigDecimal.ZERO"
    extra = ""
    if iface == "v2":
        extra = "public ObservableMap<WalletAddress, BigDecimal> getTokenBalances() { return null; }\n"
    if iface != "v0":
        extra += "public String payable(BigDecimal amount, byte[] userData) { return \"ok\"; }\n"
    return f"""{imports}
import com.credits.scapi.{smart}.SmartContract;
import java.math.BigDecimal;

public class {cls} extends SmartContract implements {name} {{
    public {cls}() {{ super(); }}
    public String getName() {{ return "CS Credits"; }}
    public String getSymbol() {{ return "CS"; }}
    public int getDecimal() {{ return 18; }}
    public boolean setFrozen(boolean frozen) {{ return false; }}
    public {ret} totalSupply() {{ return {zero}; }}
    public {ret} balanceOf(String owner) {{ return {zero}; }}
    public {ret} allowance(String owner, String spender) {{ return {zero}; }}
    public boolean transfer(String to, {t} amount) {{ return true; }}
    public boolean transferFrom(String from, String to, {t} amount) {{ return true; }}
    public void approve(String spender, {t} amount) {{ }}
    public boolean burn({t} amount) {{ return true; }}
    {extra}
}}
"""


# a method that changes the state and then fails, as a transfer that checks too late does
PARTIAL = """import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;
import java.util.HashMap;

public class Partial extends SmartContract {
    private final HashMap<String, Integer> balances = new HashMap<>();
    private int calls;
    public Partial() { super(); balances.put(initiator, 100); }
    public int debitThenFail(int amount) {
        calls += 1;
        balances.put(initiator, balances.getOrDefault(initiator, 0) - amount);
        sendTransaction(contractAddress, initiator, 1.0);
        throw new RuntimeException("checked too late");
    }
    public int balance(String who) { return balances.getOrDefault(who, 0); }
    public int calls() { return calls; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# a getter that writes: the API runs getters with stateCanModify = false
GETTER_WRITES = """import com.credits.scapi.annotations.Getter;
import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class GetterWrites extends SmartContract {
    private int reads;
    public GetterWrites() { super(); }
    @Getter
    public int read() { reads += 1; return reads; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# two top-level classes: the executor takes "any" class without '$' in its name as the contract
TWO_CLASSES = """import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class Main extends SmartContract {
    private int value = 7;
    public Main() { super(); }
    public int value() { return value; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}

class Helper {
    public int value() { return -1; }
}
"""

# what a token can learn from the platform: seed, block time, CS balances, emitted transactions
PLATFORM = """import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class Platform extends SmartContract {
    private String last = "";
    public Platform() { super(); }
    public String seed() { byte[] s = getSeed(); return s == null ? "null" : Integer.toString(s.length); }
    public long time() { return getBlockchainTimeMills(); }
    public String balance(String who) { return getBalance(who).toPlainString(); }
    public String whoami() { last = initiator + "|" + contractAddress; return last; }
    public boolean pay(String to, double amount) { sendTransaction(contractAddress, to, amount); return true; }
    public boolean payFrom(String from, String to, double amount) { sendTransaction(from, to, amount); return true; }
    public String payable(BigDecimal amount, byte[] userData) { return amount.toPlainString() + "/" + userData.length; }
}
"""

# contract-to-contract: a shop that pays with the tokens it holds
SHOP = """import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class Shop extends SmartContract {
    private final String token;
    public Shop() { super(); token = "%s"; }
    public Object pay(String to, BigDecimal amount) { return invokeExternalContract(token, "transfer", to, amount); }
    public Object myBalance() { return invokeExternalContract(token, "balanceOf", contractAddress); }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# a token whose getName never returns: the deploy calls getName outside the time limit
SLOW_NAME = """import com.credits.scapi.v0.BasicStandard;
import com.credits.scapi.v0.SmartContract;

public class SlowName extends SmartContract implements BasicStandard {
    public SlowName() { super(); }
    public String getName() { long x = 0; while (true) { x++; if (x < 0) return "never"; } }
    public String getSymbol() { return "SLOW"; }
    public int getDecimal() { return 0; }
    public boolean setFrozen(boolean frozen) { return false; }
    public String totalSupply() { return "1"; }
    public String balanceOf(String owner) { return "0"; }
    public String allowance(String owner, String spender) { return "0"; }
    public boolean transfer(String to, String amount) { return true; }
    public boolean transferFrom(String from, String to, String amount) { return true; }
    public void approve(String spender, String amount) { }
    public boolean burn(String amount) { return true; }
}
"""


# a state whose serialization never ends: the executor serializes the new state outside the
# contract's time limit
SERIAL_LOOP = """import com.credits.scapi.v3.SmartContract;
import java.io.IOException;
import java.io.ObjectOutputStream;
import java.io.Serializable;
import java.math.BigDecimal;

public class SerialLoop extends SmartContract {
    public static class Payload implements Serializable {
        private boolean armed;
        private void writeObject(ObjectOutputStream out) throws IOException {
            long x = 0;
            while (armed) { x++; if (x < 0) break; }
            out.defaultWriteObject();
        }
    }
    private final Payload payload = new Payload();
    public SerialLoop() { super(); }
    public boolean arm() { payload.armed = true; return true; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""

# a double amount the CS Amount cannot hold exactly
PRECISION = """import com.credits.scapi.v3.SmartContract;
import java.math.BigDecimal;

public class Precision extends SmartContract {
    public Precision() { super(); }
    public boolean pay(String to, BigDecimal amount) { sendTransaction(contractAddress, to, amount.doubleValue()); return true; }
    public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
"""


# ---------------------------------------------------------------------------------------------
def token_standards(ex, rec, Contract):
    """The token standard getContractMethods reports for each kind of token"""
    expected = [("BasicV0", BASIC_V0, 1), ("TokenV0", source("TokenV0"), 2), ("BasicV1", BASIC_V1, 3),
                ("TokenV1", source("TokenV1"), 4), ("BasicV2", BASIC_V2, None), ("Inherited", INHERITED, None)]
    for name, src, ts in expected:
        c = Contract(ex, name, src, OWNER, rec)
        # a token standard from a base class needs a second top-level class, which the executor's
        # compiler refuses ("Class name not found"): only one top-level class per contract
        if not rec.expect(c.compile(), f"{name} compiles", severity="info" if name == "Inherited" else "bug"):
            continue
        r = c.get_methods()
        if ts is not None:
            rec.expect(r.tokenStandard == ts, f"{name} reports token standard {ts} (got {r.tokenStandard})")
        else:
            rec.expect(r.tokenStandard != 0, f"{name} is reported as a token (got {r.tokenStandard}): the API ignores "
                                             f"tokens reported as 0")


def token_v1_lifecycle(ex, rec, Contract):
    """A v1 token through deploy, getters, transfer, approve/transferFrom, burn, freeze"""
    t = Contract(ex, "TokenV1", source("TokenV1"), OWNER, rec)
    d = t.deploy()
    if not rec.expect(d and d["ok"], "TokenV1 deploys"):
        return
    get = lambda m, *p: t.get(m, *p)["ret"]
    rec.expect(get("getName") == "Test Token V1", "getName as a getter")
    rec.expect(get("balanceOf", v_str(b58(OWNER))) == "10000000.000", "the owner holds the supply")

    # a wallet that builds the call from the declared types sends a BigDecimal
    r = t.call(OWNER, "transfer", v_str(b58(ALICE)), v_dec("1500.5"))
    rec.expect(r["ok"] and r["ret"] is True, "transfer(String, BigDecimal) with a v_big_decimal amount")
    rec.expect(get("balanceOf", v_str(b58(ALICE))) == "1500.500", "Alice's balance after the transfer")

    # the API's token cache recognises a transfer only when both arguments are strings
    r = t.call(OWNER, "transfer", v_str(b58(ALICE)), v_str("10"), label="transfer(alice, \"10\") amount as a string")
    rec.expect(r["ok"], "a v1 transfer with the amount as a string (what the API's token cache expects) is accepted",
               severity="interop")

    # scale: BigDecimal keeps the scale it is given, balances may end up as 1500.5 vs 1500.500
    r = t.call(ALICE, "transfer", v_str(b58(BOB)), v_dec("0.0001"))
    rec.expect(r["ok"], "a transfer below the token's decimals is accepted by this token", severity="info")
    rec.expect(get("balanceOf", v_str(b58(BOB))) == "0.0001",
               "the token keeps the amount's own scale (0.0001 with 3 decimals): the contract must round",
               severity="info")

    r = t.call(ALICE, "transfer", v_str(b58(BOB)), v_dec("999999"))
    rec.expect(not r["ok"], "a transfer above the balance fails")
    rec.expect("enough tokens" in r["error"] or "enough tokens" in str(r.get("ret")), "its error reaches the caller")

    r = t.call(ALICE, "transfer", v_str(b58(BOB)), v_dec("-5"))
    rec.expect(not r["ok"], "a negative transfer fails (the contract checks it)")

    r = t.call(OWNER, "approve", v_str(b58(BOB)), v_dec("100"))
    rec.expect(r["ok"], "approve")
    rec.expect(get("allowance", v_str(b58(OWNER)), v_str(b58(BOB))) == "100", "allowance after approve")
    r = t.call(BOB, "transferFrom", v_str(b58(OWNER)), v_str(b58(CAROL)), v_dec("60"))
    rec.expect(r["ok"], "transferFrom within the allowance")
    r = t.call(BOB, "transferFrom", v_str(b58(OWNER)), v_str(b58(CAROL)), v_dec("60"))
    rec.expect(not r["ok"], "transferFrom above the allowance fails")
    rec.expect(get("balanceOf", v_str(b58(CAROL))) == "60.000", "Carol's balance")

    r = t.call(ALICE, "setFrozen", v_bool(True))
    rec.expect(not r["ok"], "only the owner freezes")
    r = t.call(OWNER, "setFrozen", v_bool(True))
    rec.expect(r["ok"], "the owner freezes")
    r = t.call(ALICE, "transfer", v_str(b58(BOB)), v_dec("1"))
    rec.expect(not r["ok"], "no transfer while frozen")
    t.call(OWNER, "setFrozen", v_bool(False))

    r = t.call(OWNER, "burn", v_dec("1000"))
    rec.expect(r["ok"] and get("totalSupply") == "9999000.000", "burn lowers the supply")
    r = t.call(CAROL, "register")
    rec.expect(r["ok"], "register")

    # the API refreshes every holder's balance with one executeByteCodeMultiple
    holders = [OWNER, ALICE, BOB, CAROL] + [key(f"h{i}") for i in range(50)]
    many = t.get_many("balanceOf", [[v_str(b58(h))] for h in holders], label="balanceOf x54")
    rec.expect(all(m["ok"] for m in many), "balanceOf for 54 holders in one executeByteCodeMultiple")

    # the node calls payable(BigDecimal, byte[]) for a CS transfer to a payable contract
    r = t.call(ALICE, "payable", v_dec("25.5"), v_bytes(b""), label="payable(25.5, []) as the node sends it")
    rec.expect(r["ok"] and get("balanceOf", v_str(b58(ALICE))) == "1525.9999", "payable buys tokens",
               severity="info")

    r = t.call(ALICE, "sell", v_dec("0.1"))
    rec.expect(r["ok"] and len(r["emitted"]) == 1, "sell emits one CS transaction")
    if r["ok"] and r["emitted"]:
        rec.expect(r["emitted"][0][2:] == [0, 100000000000000000] or tuple(r["emitted"][0][2:]) == (0, 100000000000000000),
                   f"0.1 CS emitted exactly (got {r['emitted'][0][2:]}): sendTransaction takes a double",
                   severity="info")


def token_v0_lifecycle(ex, rec, Contract):
    """A v0 token (amounts as strings), including the legacy payable(String, String)"""
    t = Contract(ex, "TokenV0", source("TokenV0"), OWNER, rec)
    d = t.deploy()
    if not rec.expect(d and d["ok"], "TokenV0 deploys"):
        return
    r = t.call(OWNER, "transfer", v_str(b58(ALICE)), v_str("12.34"))
    rec.expect(r["ok"], "transfer(String, String)")
    r = t.call(OWNER, "transfer", v_str(b58(ALICE)), v_dec("1"), label="transfer(alice, 1) amount as a BigDecimal")
    rec.expect(r["ok"], "a v0 transfer with a v_big_decimal amount is accepted", severity="interop")
    rec.expect(t.get("balanceOf", v_str(b58(ALICE)))["ret"] == "12.34", "balance as a string")
    r = t.call(ALICE, "payable", v_str("3"), v_str(""), label="payable(\"3\", \"\") legacy convention")
    rec.expect(r["ok"], "legacy payable(String, String)")


def failed_call_state(ex, rec, Contract):
    """What the executor returns for a call that changes the state and then throws"""
    p = Contract(ex, "Partial", PARTIAL, OWNER, rec)
    if not rec.expect((p.deploy() or {}).get("ok"), "Partial deploys"):
        return
    before = p.state
    r = p.call(OWNER, "debitThenFail", general_int(10))
    rec.expect(not r["ok"], "the call fails")
    rec.expect(not r["states"], f"a failed call returns no contract state (got {r['states']}); the node must not "
                                "keep it", severity="info")
    rec.expect(not r["emitted"], f"a failed call returns no emitted transaction (got {r['emitted']})", severity="info")
    rec.expect(p.get("balance", v_str(b58(OWNER)))["ret"] == 100 and p.state == before,
               "the state the node keeps is unchanged")


def getter_writes(ex, rec, Contract):
    """A getter that changes the state, run as the API runs getters"""
    g = Contract(ex, "GetterWrites", GETTER_WRITES, OWNER, rec)
    if not rec.expect((g.deploy() or {}).get("ok"), "GetterWrites deploys"):
        return
    r1 = g.get("read")
    r2 = g.get("read")
    rec.expect(r1["ok"] and r2["ok"] and r1["ret"] == r2["ret"] == 1,
               "a getter that writes runs on a copy: every read starts from the stored state", severity="info")


def reserved_names(ex, rec, Contract):
    """The deploy refuses the name "CS Credits" and the symbol "CS" for every token standard"""
    for cls, iface, smart, t in (("ReservedV0", "v0", "v0", "String"), ("ReservedV1", "v1", "v3", "BigDecimal"),
                                 ("ReservedV2", "v2", "v3", "BigDecimal")):
        c = Contract(ex, cls, reserved(cls, iface, smart, t), OWNER, rec)
        d = c.deploy()
        rec.expect(d is not None and not d["ok"], f"{cls} ({iface} standard) named CS is refused at deploy",
                   severity="bug" if iface != "v2" else "gap")


def root_class(ex, rec, Contract):
    """A source with two top-level classes: which one is the contract"""
    m = Contract(ex, "Main", TWO_CLASSES, OWNER, rec)
    d = m.deploy()
    rec.expect(d and d["ok"], "a contract with a helper class deploys", severity="info")
    if d and d["ok"]:
        rec.expect(m.get("value")["ret"] == 7, "the public class is the contract", severity="bug")


def platform_calls(ex, rec, Contract):
    """Seed, block time, CS balance, initiator, emitted transactions"""
    p = Contract(ex, "Platform", PLATFORM, OWNER, rec)
    if not rec.expect((p.deploy() or {}).get("ok"), "Platform deploys"):
        return
    ex.stub.balances[ALICE] = (12, 500000000000000000)
    rec.expect(p.call(ALICE, "seed")["ret"] == "32", "getSeed in a call")
    rec.expect(p.call(ALICE, "time")["ok"], "getBlockchainTimeMills in a call")
    rec.expect(p.call(ALICE, "balance", v_str(b58(ALICE)))["ret"] == "12.5", "getBalance", severity="info")
    r = p.call(ALICE, "whoami")
    rec.expect(r["ret"] == b58(ALICE) + "|" + b58(p.address), "initiator and contractAddress")
    r = p.call(ALICE, "payFrom", v_str(b58(ALICE)), v_str(b58(BOB)), general_double(1.0))
    rec.expect(r["ok"], "the executor accepts an emitted transaction from another wallet (the node drops it)",
               severity="info")
    r = p.call(ALICE, "payable", v_dec("1.5"), v_bytes(b"hi"))
    rec.expect(r["ret"] == "1.5/2", "payable with user data")


def contract_calls(ex, rec, Contract):
    """A contract that moves tokens through invokeExternalContract (refused by the node today)"""
    token = Contract(ex, "TokenV1c", source("TokenV1").replace("class TokenV1 ", "class TokenV1c ")
                     .replace("public TokenV1()", "public TokenV1c()"), OWNER, rec)
    if not rec.expect((token.deploy() or {}).get("ok"), "token deploys"):
        return
    shop = Contract(ex, "Shop", SHOP % b58(token.address), OWNER, rec)
    if not rec.expect((shop.deploy() or {}).get("ok"), "Shop deploys"):
        return
    token.call(OWNER, "transfer", v_str(b58(shop.address)), v_dec("100"))
    token.publish()
    shop.publish()
    r = shop.call(ALICE, "pay", v_str(b58(BOB)), v_dec("10"))
    rec.expect(r["ok"], f"a contract transfers the tokens it holds (error: {r['error']})", severity="gap")
    if r["ok"]:
        rec.expect(b58(token.address) in r["states"], "the token's new state is returned with the caller's")


def slow_token_name(ex, rec, Contract):
    """A token whose getName() never returns: the reserved-name check runs it without a time limit"""
    s = Contract(ex, "SlowName", SLOW_NAME, OWNER, rec)
    d = s.deploy(timeout=40000)
    rec.expect(d is not None, "the deploy of a token whose getName() loops answers within 40 s", severity="security")
    probe = Contract(ex, "BasicV0probe", BASIC_V0.replace("BasicV0", "BasicV0probe"), OWNER, rec)
    try:
        ok = probe.compile()
    except Exception:
        ok = False
    rec.expect(ok, "the executor still answers afterwards", severity="security")


def serialization_hang(ex, rec, Contract):
    """A state whose writeObject() loops once armed: serialization runs outside the time limit"""
    s = Contract(ex, "SerialLoop", SERIAL_LOOP, OWNER, rec)
    if not rec.expect((s.deploy() or {}).get("ok"), "SerialLoop deploys"):
        return
    c = ex.client(40000)
    try:
        from executor_harness import EXECUTION_TIME, EXECUTOR_VERSION, executor
        c.executeByteCode(ex.next_access_id(), ALICE, s._binary(s.state), [executor.MethodHeader(methodName="arm", params=[])],
                          EXECUTION_TIME, EXECUTOR_VERSION)
        answered = True
    except Exception:
        answered = False
    finally:
        c.close()
    rec.expect(answered, "a call whose new state never finishes serializing answers within 40 s", severity="security")


def emitted_precision(ex, rec, Contract):
    """CS amounts a contract emits go through a double"""
    p = Contract(ex, "Precision", PRECISION, OWNER, rec)
    if not rec.expect((p.deploy() or {}).get("ok"), "Precision deploys"):
        return
    for amount, want in (("0.1", (0, 100000000000000000)), ("1234567.123456789012345678", (1234567, 123456789012345678)),
                         ("0.000000000000000001", (0, 1))):
        r = p.call(ALICE, "pay", v_str(b58(BOB)), v_dec(amount))
        got = tuple(r["emitted"][0][2:]) if r["ok"] and r["emitted"] else None
        rec.expect(got == want, f"emitting {amount} CS gives {want} (got {got}): sendTransaction takes a double",
                   severity="info")


def general_int(i):
    from executor_harness import v_int
    return v_int(i)


def general_double(x):
    from executor_harness import general
    return general.Variant(v_double=x)


SCENARIOS = [token_standards, token_v1_lifecycle, token_v0_lifecycle, failed_call_state, getter_writes,
             reserved_names, root_class, platform_calls, emitted_precision, contract_calls, slow_token_name,
             serialization_hang]
