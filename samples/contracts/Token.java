import com.credits.scapi.annotations.Getter;
import com.credits.scapi.v1.ExtensionTokenStandard;
import com.credits.scapi.v3.SmartContract;

import java.math.BigDecimal;
import java.util.HashMap;
import java.util.Map;

import static java.math.BigDecimal.ZERO;

/**
 * A token template for the Credits network: SmartContract v3 and ExtensionTokenStandard v1, so that
 * wallets and the node's token API (token standard 4) recognise it.
 *
 * What it does that older templates did not:
 * - every amount is checked: positive, and with no more decimals than the token has (a call with
 *   1.0001 on a 3-decimal token fails instead of creating dust nobody can move);
 * - every balance and allowance is stored with the token's scale, so 5 and 5.000 are the same value;
 * - approve() sets the allowance (ERC-20 semantics), it does not add to it;
 * - transfer() to oneself and transferFrom() with from == to do nothing but still check the amount.
 *
 * Calls pass amounts as BigDecimal (Variant v_big_decimal) and addresses as base58 strings
 * (v_string): the node refuses a call whose arguments do not match (docs/tokens.md).
 * Change NAME, SYMBOL, DECIMALS, SUPPLY and PRICE before deploying; "CS" and "CS Credits" are refused.
 */
public class Token extends SmartContract implements ExtensionTokenStandard {

    private static final String NAME = "My Token";
    private static final String SYMBOL = "MYT";
    private static final int DECIMALS = 3;
    private static final long SUPPLY = 10_000_000L;
    // CS paid for one token through payable() (a CS transfer to the contract); 0 = not for sale
    private static final String PRICE = "1";

    private final String owner;
    private final HashMap<String, BigDecimal> balances = new HashMap<>();
    private final HashMap<String, Map<String, BigDecimal>> allowed = new HashMap<>();
    private BigDecimal totalCoins;
    private boolean frozen;

    public Token() {
        super();
        owner = initiator;
        totalCoins = scaled(new BigDecimal(SUPPLY));
        balances.put(owner, totalCoins);
    }

    @Getter
    @Override
    public String getName() {
        return NAME;
    }

    @Getter
    @Override
    public String getSymbol() {
        return SYMBOL;
    }

    @Getter
    @Override
    public int getDecimal() {
        return DECIMALS;
    }

    @Getter
    @Override
    public BigDecimal totalSupply() {
        return totalCoins;
    }

    @Getter
    @Override
    public BigDecimal balanceOf(String owner) {
        return balance(owner);
    }

    @Getter
    @Override
    public BigDecimal allowance(String owner, String spender) {
        Map<String, BigDecimal> spenders = allowed.get(owner);
        BigDecimal amount = spenders == null ? null : spenders.get(spender);
        return amount == null ? scaled(ZERO) : amount;
    }

    @Getter
    public String getOwner() {
        return owner;
    }

    @Override
    public boolean setFrozen(boolean isFrozen) {
        onlyOwner();
        frozen = isFrozen;
        return true;
    }

    @Override
    public boolean transfer(String to, BigDecimal amount) {
        notFrozen();
        BigDecimal value = checked(amount);
        if (!to.equals(initiator)) {
            move(initiator, to, value);
        }
        return true;
    }

    @Override
    public boolean transferFrom(String from, String to, BigDecimal amount) {
        notFrozen();
        BigDecimal value = checked(amount);
        BigDecimal allowance = allowance(from, initiator);
        if (allowance.compareTo(value) < 0) {
            throw new RuntimeException("allowance of " + initiator + " is " + allowance.toPlainString() + ", less than " + value.toPlainString());
        }
        if (!from.equals(to)) {
            move(from, to, value);
        }
        allowed.get(from).put(initiator, allowance.subtract(value));
        return true;
    }

    @Override
    public void approve(String spender, BigDecimal amount) {
        BigDecimal value = amount.signum() == 0 ? scaled(ZERO) : checked(amount);
        allowed.computeIfAbsent(initiator, k -> new HashMap<>()).put(spender, value);
    }

    @Override
    public boolean burn(BigDecimal amount) {
        notFrozen();
        BigDecimal value = checked(amount);
        BigDecimal balance = balance(initiator);
        if (balance.compareTo(value) < 0) {
            throw new RuntimeException("balance of " + initiator + " is " + balance.toPlainString() + ", less than " + value.toPlainString());
        }
        balances.put(initiator, balance.subtract(value));
        totalCoins = totalCoins.subtract(value);
        return true;
    }

    @Override
    public void register() {
        balances.putIfAbsent(initiator, scaled(ZERO));
    }

    // a CS transfer to the contract buys tokens from the owner at PRICE
    @Override
    public String payable(BigDecimal amount, byte[] userData) {
        notFrozen();
        BigDecimal price = new BigDecimal(PRICE);
        if (price.signum() <= 0) {
            throw new RuntimeException("the token is not for sale");
        }
        BigDecimal tokens = amount.divide(price, DECIMALS, BigDecimal.ROUND_DOWN);
        if (tokens.signum() <= 0) {
            throw new RuntimeException("the amount buys no token");
        }
        move(owner, initiator, tokens);
        return "bought " + tokens.toPlainString() + " " + SYMBOL;
    }

    private void move(String from, String to, BigDecimal value) {
        BigDecimal source = balance(from);
        if (source.compareTo(value) < 0) {
            throw new RuntimeException("balance of " + from + " is " + source.toPlainString() + ", less than " + value.toPlainString());
        }
        balances.put(from, source.subtract(value));
        balances.put(to, balance(to).add(value));
    }

    // a positive amount with at most DECIMALS decimals, at the token's scale
    private BigDecimal checked(BigDecimal amount) {
        if (amount == null || amount.signum() <= 0) {
            throw new RuntimeException("the amount must be positive");
        }
        if (amount.stripTrailingZeros().scale() > DECIMALS) {
            throw new RuntimeException("the amount has more than " + DECIMALS + " decimals");
        }
        return scaled(amount);
    }

    private BigDecimal balance(String address) {
        BigDecimal balance = balances.get(address);
        return balance == null ? scaled(ZERO) : balance;
    }

    private static BigDecimal scaled(BigDecimal value) {
        return value.setScale(DECIMALS, BigDecimal.ROUND_UNNECESSARY);
    }

    private void onlyOwner() {
        if (!initiator.equals(owner)) {
            throw new RuntimeException("only the owner " + owner + " can do this");
        }
    }

    private void notFrozen() {
        if (frozen) {
            throw new RuntimeException("the token is frozen");
        }
    }
}
