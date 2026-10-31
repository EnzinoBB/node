import com.credits.scapi.v0.ExtensionStandard;
import com.credits.scapi.v0.SmartContract;

import java.math.BigDecimal;
import java.util.HashMap;
import java.util.Map;

import static java.math.BigDecimal.ROUND_DOWN;
import static java.math.BigDecimal.ZERO;

// A first generation token: SmartContract v0 and ExtensionStandard v0, amounts passed as strings.
public class TokenV0 extends SmartContract implements ExtensionStandard {

    private final String owner;
    private final int decimal = 2;
    private final HashMap<String, BigDecimal> balances = new HashMap<>();
    private final HashMap<String, Map<String, BigDecimal>> allowed = new HashMap<>();
    private BigDecimal totalCoins;
    private boolean frozen;

    public TokenV0() {
        super();
        owner = initiator;
        totalCoins = new BigDecimal(1_000_000).setScale(decimal, ROUND_DOWN);
        balances.put(owner, totalCoins);
    }

    @Override
    public String getName() {
        return "Test Token V0";
    }

    @Override
    public String getSymbol() {
        return "TTV0";
    }

    @Override
    public int getDecimal() {
        return decimal;
    }

    @Override
    public boolean setFrozen(boolean isFrozen) {
        if (!initiator.equals(owner)) {
            throw new RuntimeException("not the owner");
        }
        frozen = isFrozen;
        return true;
    }

    @Override
    public String totalSupply() {
        return totalCoins.toString();
    }

    @Override
    public String balanceOf(String owner) {
        return get(owner).toString();
    }

    @Override
    public String allowance(String owner, String spender) {
        Map<String, BigDecimal> spenders = allowed.get(owner);
        BigDecimal amount = spenders == null ? null : spenders.get(spender);
        return (amount == null ? ZERO : amount).toString();
    }

    @Override
    public boolean transfer(String to, String amount) {
        notFrozen();
        BigDecimal value = parse(amount);
        BigDecimal from = get(initiator);
        if (from.compareTo(value) < 0) {
            throw new RuntimeException("not enough tokens");
        }
        balances.put(initiator, from.subtract(value));
        balances.put(to, get(to).add(value));
        return true;
    }

    @Override
    public boolean transferFrom(String from, String to, String amount) {
        notFrozen();
        BigDecimal value = parse(amount);
        Map<String, BigDecimal> spenders = allowed.get(from);
        BigDecimal allow = spenders == null ? null : spenders.get(initiator);
        if (allow == null || allow.compareTo(value) < 0) {
            throw new RuntimeException("not approved");
        }
        BigDecimal source = get(from);
        if (source.compareTo(value) < 0) {
            throw new RuntimeException("not enough tokens");
        }
        spenders.put(initiator, allow.subtract(value));
        balances.put(from, source.subtract(value));
        balances.put(to, get(to).add(value));
        return true;
    }

    @Override
    public void approve(String spender, String amount) {
        allowed.computeIfAbsent(initiator, k -> new HashMap<>()).put(spender, parse(amount));
    }

    @Override
    public boolean burn(String amount) {
        BigDecimal value = parse(amount);
        BigDecimal balance = get(initiator);
        if (balance.compareTo(value) < 0) {
            throw new RuntimeException("not enough tokens");
        }
        balances.put(initiator, balance.subtract(value));
        totalCoins = totalCoins.subtract(value);
        return true;
    }

    @Override
    public void register() {
        balances.putIfAbsent(initiator, ZERO.setScale(decimal, ROUND_DOWN));
    }

    @Override
    public boolean buyTokens(String amount) {
        return false;
    }

    // the payable() of the first API version: amount and user data as strings
    public String payable(String amount, String currency) {
        BigDecimal value = parse(amount);
        BigDecimal ownerBalance = get(owner);
        balances.put(owner, ownerBalance.subtract(value));
        balances.put(initiator, get(initiator).add(value));
        return "bought " + value;
    }

    private void notFrozen() {
        if (frozen) {
            throw new RuntimeException("frozen");
        }
    }

    private BigDecimal parse(String amount) {
        BigDecimal value = new BigDecimal(amount.replace(',', '.')).setScale(decimal, ROUND_DOWN);
        if (value.signum() < 0) {
            throw new RuntimeException("negative amount");
        }
        return value;
    }

    private BigDecimal get(String address) {
        BigDecimal balance = balances.get(address);
        return balance != null ? balance : ZERO.setScale(decimal, ROUND_DOWN);
    }
}
