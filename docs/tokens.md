# Tokens and the token API

Credits tokens are smart contracts that implement one of the token standards of the contract API.
The chain only knows contracts and their states. The token methods of the Thrift API
(`TokensListGet`, `TokenInfoGet`, `TokenBalancesGet`, `TokenHoldersGet`, `TokenTransfersGet`,
`TokenWalletTransfersGet`, `TokenTransfersListGet`, `TokenTransferGet`, `TokenTransactionsGet`)
answer from a token cache that the node builds next to the chain. This cache affects only the
API, never consensus.

## Token standards

The executor reports a standard for every contract (`getContractMethods`, `tokenStandard`):

| Id | Interface | Amounts |
|---|---|---|
| 0 | not a token | |
| 1 | `com.credits.scapi.v0.BasicStandard` | `String` |
| 2 | `com.credits.scapi.v0.ExtensionStandard` (adds `register()`, `buyTokens(String)`) | `String` |
| 3 | `com.credits.scapi.v1.BasicTokenStandard`, and the v2 interface that extends it | `BigDecimal` |
| 4 | `com.credits.scapi.v1.ExtensionTokenStandard` (adds `register()`), and its v2 interface | `BigDecimal` |

The executor of the mainnet release (build 1518) reports a token that implements the v2
interfaces as 0, not a token. The community executor reports it as 3 or 4. For every other
contract the two executors report the same id.

A call must pass each argument in the type the method declares. A v1 token's
`transfer(String, BigDecimal)` takes the amount as `v_big_decimal`. Sent as `v_string`, the
call would fail on every node with "Cannot find a method by name and parameters specified", and
the transaction fee would still be charged. The reverse applies to v0 tokens. A wallet reads the
types from `ContractMethodsGet` (or `SmartContractGet`) before it builds the call.

`TransactionFlow` refuses such a call before it reaches the network: a method the contract does
not have, a wrong number of arguments, or an argument whose Variant cannot match the declared type
(for example `transfer(java.lang.String, java.math.BigDecimal): argument 2 is java.math.BigDecimal
(v_big_decimal), the call passes v_string`). Nothing is sent and no fee is paid. Only certain
mismatches are refused: with types other than `String`, `BigDecimal`, `boolean`, `int`, `long`,
`double` (and their wrappers) and `byte[]` the call goes on as before.

## A template and a client

- `samples/contracts/Token.java`: a token to start from (ExtensionTokenStandard v1, token standard
  4). It checks every amount (positive, no more decimals than the token has), keeps balances at the
  token's scale, sets allowances with `approve()` as ERC-20 does, and sells tokens for CS through
  `payable()`. The executor harness runs it (`template_token`).
- `samples/python/token_client.py`: lists a contract's methods, reads a getter for free, and signs
  and sends a call, converting each argument to the type the method declares:

  ```sh
  python3 samples/python/token_client.py methods <token>
  python3 samples/python/token_client.py get <token> balanceOf <wallet>
  python3 samples/python/token_client.py --seed <private key> call <token> transfer <wallet> 12.5
  ```

## The token cache

- `[api] tokens_cache=true` turns the cache on. It is off by default. The RelMonitor and
  RelWallet builds always have it on, as the official monitor and wallet nodes do. With the
  cache off, the token methods answer "This node doesn't provide such info".
- **RelMonitor and RelWallet builds.** These are the official monitor and wallet configurations
  (`-DMONITOR_NODE`, `-DWEB_WALLET_NODE`). The community CI neither builds nor tests them, and a
  private network of RelWallet nodes did not get past its first round in our tests. Use a normal
  build with `tokens_cache=true` instead: it serves the same token methods.
- **What it records.** A token is recorded at its deploy. After each successful call, the node
  records:
  - the initiator, and every argument that is a wallet address, as holders of the token;
  - `transfer` and `transferFrom` calls, with the amount as a string or a BigDecimal, as
    transfers.
  
  The node then asks the contract for the balances of those wallets (`balanceOf`), and for its
  name, symbol and total supply. A token with up to 50 holders is read in full. A balance is
  always the one the contract reports, including after methods outside the standard such as
  mint, airdrop or a sale for CS. Failed calls change nothing.
- **Starting.** When the node runs, it reads every token's balances again. This includes a quick
  start at the last stored block, which reads no block. If the checkpoint holds no tokens, for
  example because it was saved with the cache off, the node first looks for tokens among all
  deployed contracts and their transactions. The log has `API: token cache is empty, looking for
  tokens among N contracts`, then `tokens loaded!`.

## Contract calls through the API after a restart

`TransactionFlow` waits for the result of a contract call and returns its return value. A node
that quick-started from a checkpoint at its last stored block never marked its stored blocks as
read. On such a node every contract call through its API ended with "Transaction in progress"
and no result, even though the network executed the call. The node now marks the blocks as read
once it runs.

## Tests

- `ci/contracts/executor_harness.py` runs the token scenarios of `ci/contracts/token_scenarios.py`
  on one or more contract executor jars, with no node. With two jars, any answer that differs
  from the first jar's fails the run:

  ```sh
  python3 ci/contracts/executor_harness.py --jar csnode/contract-executor.jar \
      --jar other/contract-executor.jar --jdk csnode/ojdkbuild/java-11-openjdk-11.0.4
  ```

- `ci/testnet/run.sh` with `CONTRACTS=1 TOKENS=1` runs `ci/testnet/tokens.py` on the private
  network. The script deploys a v1 token and a v0 token, then makes transfers, approvals, a CS
  payment to the token and a sale back for CS. It checks the balances and the states on every
  node, and the token API. `TOKENS_VIA=5` with `RESTART_NODES=5` sends everything through the
  node of the restart test after it quick-started.
