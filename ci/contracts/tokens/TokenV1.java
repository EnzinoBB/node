import com.credits.scapi.annotations.Getter;
import com.credits.scapi.v1.ExtensionTokenStandard;
import com.credits.scapi.v3.SmartContract;

import java.math.BigDecimal;
import java.util.HashMap;
import java.util.Map;

import static java.math.BigDecimal.ROUND_DOWN;
import static java.math.BigDecimal.ZERO;

// A token written the way current Credits tokens are: SmartContract v3 and ExtensionTokenStandard v1
// (BigDecimal amounts), balances in a HashMap keyed by the base58 address, CS bought through payable().
public class TokenV1 extends SmartContract implements ExtensionTokenStandard {

    private final String owner;
    private final int decimal;
    private final BigDecimal tokenCost;
    private final HashMap<String, BigDecimal> balances;
    private final HashMap<String, Map<String, BigDecimal>> allowed;
    private final String name;
    private final String symbol;
    private BigDecimal totalCoins;
    private boolean frozen;

    public TokenV1() {
        super();
        name = "Test Token V1";
        symbol = "TTV1";
        decimal = 3;
        tokenCost = new BigDecimal(1).setScale(decimal, ROUND_DOWN);
        totalCoins = new BigDecimal(10_000_000).setScale(decimal, ROUND_DOWN);
        owner = initiator;
        allowed = new HashMap<>();
        balances = new HashMap<>();
        balances.put(owner, totalCoins);
    }

    @Getter
    @Override
    public int getDecimal() {
        return decimal;
    }

    @Override
    public boolean setFrozen(boolean isFrozen) {
        if (!initiator.equals(owner)) {
            throw new RuntimeException("unable change frozen state! The wallet " + initiator + " is not owner");
        }
        frozen = isFrozen;
        return true;
    }

    @Getter
    @Override
    public String getName() {
        return name;
    }

    @Getter
    @Override
    public String getSymbol() {
        return symbol;
    }

    @Getter
    @Override
    public BigDecimal totalSupply() {
        return totalCoins;
    }

    @Getter
    @Override
    public BigDecimal balanceOf(String owner) {
        return getTokensBalance(owner);
    }

    @Getter
    @Override
    public BigDecimal allowance(String owner, String spender) {
        Map<String, BigDecimal> spenders = allowed.get(owner);
        if (spenders == null) {
            return ZERO;
        }
        BigDecimal amount = spenders.get(spender);
        return amount != null ? amount : ZERO;
    }

    @Override
    public boolean transfer(String to, BigDecimal amount) {
        contractIsNotFrozen();
        checkAmount(amount);
        if (!to.equals(initiator)) {
            BigDecimal sourceBalance = getTokensBalance(initiator);
            BigDecimal targetBalance = getTokensBalance(to);
            if (sourceBalance.compareTo(amount) < 0) {
                throw new RuntimeException("the wallet " + initiator + " doesn't have enough tokens to transfer");
            }
            balances.put(initiator, sourceBalance.subtract(amount));
            balances.put(to, targetBalance.add(amount));
        }
        return true;
    }

    @Override
    public boolean transferFrom(String from, String to, BigDecimal amount) {
        contractIsNotFrozen();
        checkAmount(amount);
        if (!from.equals(to)) {
            BigDecimal sourceBalance = getTokensBalance(from);
            BigDecimal targetBalance = getTokensBalance(to);
            if (sourceBalance.compareTo(amount) < 0) {
                throw new RuntimeException("unable transfer tokens! The balance of " + from + " less then " + amount);
            }
            Map<String, BigDecimal> spenders = allowed.get(from);
            if (spenders == null || !spenders.containsKey(initiator)) {
                throw new RuntimeException("unable transfer tokens! The value of approved tokens is null");
            }
            BigDecimal allowTokens = spenders.get(initiator);
            if (allowTokens.compareTo(amount) < 0) {
                throw new RuntimeException("unable transfer tokens! The value of approved tokens is less then " + amount);
            }
            spenders.put(initiator, allowTokens.subtract(amount));
            balances.put(from, sourceBalance.subtract(amount));
            balances.put(to, targetBalance.add(amount));
        }
        return true;
    }

    @Override
    public void approve(String spender, BigDecimal amount) {
        checkAmount(amount);
        allowed.computeIfAbsent(initiator, k -> new HashMap<>()).put(spender, amount);
    }

    @Override
    public boolean burn(BigDecimal amount) {
        contractIsNotFrozen();
        checkAmount(amount);
        BigDecimal balance = getTokensBalance(initiator);
        if (balance.compareTo(amount) < 0) {
            throw new RuntimeException("the wallet " + initiator + " doesn't have enough tokens to burn");
        }
        balances.put(initiator, balance.subtract(amount));
        totalCoins = totalCoins.subtract(amount);
        return true;
    }

    @Override
    public void register() {
        balances.putIfAbsent(initiator, ZERO.setScale(decimal, ROUND_DOWN));
    }

    @Override
    public String payable(BigDecimal amount, byte[] userData) {
        contractIsNotFrozen();
        if (amount.compareTo(ZERO) <= 0) {
            throw new RuntimeException("amount must be positive");
        }
        BigDecimal tokens = amount.divide(tokenCost, decimal, ROUND_DOWN);
        BigDecimal ownerBalance = getTokensBalance(owner);
        if (ownerBalance.compareTo(tokens) < 0) {
            throw new RuntimeException("not enough tokens left to sell");
        }
        balances.put(owner, ownerBalance.subtract(tokens));
        balances.put(initiator, getTokensBalance(initiator).add(tokens));
        return "bought " + tokens.toPlainString() + " " + symbol;
    }

    // sells tokens back to the contract for CS, through a transaction the contract emits
    public boolean sell(BigDecimal amount) {
        contractIsNotFrozen();
        checkAmount(amount);
        BigDecimal balance = getTokensBalance(initiator);
        if (balance.compareTo(amount) < 0) {
            throw new RuntimeException("not enough tokens");
        }
        balances.put(initiator, balance.subtract(amount));
        balances.put(owner, getTokensBalance(owner).add(amount));
        sendTransaction(contractAddress, initiator, amount.multiply(tokenCost).doubleValue());
        return true;
    }

    @Getter
    public String getOwner() {
        return owner;
    }

    @Getter
    public int holders() {
        return balances.size();
    }

    private void contractIsNotFrozen() {
        if (frozen) {
            throw new RuntimeException("unavailable action! The smart-contract is frozen");
        }
    }

    private void checkAmount(BigDecimal amount) {
        if (amount == null || amount.signum() < 0) {
            throw new RuntimeException("amount must not be negative");
        }
    }

    private BigDecimal getTokensBalance(String address) {
        BigDecimal balance = balances.get(address);
        return balance != null ? balance : ZERO.setScale(decimal, ROUND_DOWN);
    }
}
