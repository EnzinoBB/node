# Contract states during sync: fetched from the network, never recomputed — design

Status: draft for review, 2026-10-24. Code references are to origin/master at a6e640f2
(EnzinoBB/node, from CREDITSCOM node 4.2.532 at 4a95aaaf).

## Goal

A node that reads blocks it did not take part in (sync from peers, start-up read after a
checkpoint, snapshot import) must end with the same contract states and the same wallet balances
as the nodes that were live when those blocks were made. Today it re-executes every contract call
to rebuild the state; for contracts whose execution depends on the moment it runs (a contract that
reads its own balance, see 2.5) the re-execution fails, the state stays stale and a later fee is
charged to the wrong wallet.

## 1. Summary of the recommendation

1. **Fee attribution no longer depends on the state** (consensus-relevant). The fee of a transaction
   a contract emits is charged to the initiator of the `new_state` of the same contract that
   precedes it in the same block, not to the initiator of the last `new_state` whose state this
   node managed to obtain. This is what a live node does when it has the state, and what the
   rollback code already does (3.6).
2. **The current state of every contract is fetched from the network and verified against the hash
   the chain declares**, as soon as the node is live (end of sync, end of the start-up read) and
   whenever a state is still missing later; re-execution during sync, if kept at all, produces only
   provisional values. Peers hold only the current state of a contract (3.3), so states after old
   calls cannot be fetched: the fetch happens at the head, and item 1 is what makes the balances
   independent of it.
3. The existing `StateRequest`/`StateReply` messages are reused unchanged; official 4.2.532 nodes
   answer them (3.4). The chain-declared hash of each contract's last successful state is recorded
   in the contract cache and in the checkpoints, so a reply can be verified without the block,
   also on a pruned node.

## 2. What the code does today

### 2.1 What a block carries

A block carries, for every executed call, a `new_state` transaction with the user fields
`RefStart` (the call), `RetVal`, `Fee` and `Hash` (solver/include/solver/smartcontracts.hpp:86-99).
Smart consensus replaces the state value by its hash before the transaction goes to the block
(solver/src/smartconsensus.cpp:97-116; an execution that failed gets `Zero::hash`, line 109-111).
The state value itself travels only in the smart-consensus packet, as a "state transaction", and
only when it is at most `Consensus::MaxContractStateSizeToSync` = 8192 bytes
(solver/src/smartconsensus.cpp:718-733, solver/src/consensus.cpp:116).

### 2.2 How a live node gets the state

When the conveyer assembles the block it emits `statesCreated` with the state transactions of the
packet (csnode/src/conveyer.cpp:577-579). `SmartContracts::on_update` (solver/src/smartcontracts.cpp:2580-2588)
calls `update_contract_state(t, false)` for each, so the state is in `known_contracts` before the
block reaches `on_store_block`. A live node that did not get the packet state (state over 8192
bytes, packet missed) re-executes the call like a syncing node and, if that fails, asks the
confidants with `net_request_contract_state` (2.4).

### 2.3 The three read paths all run as `reading_db`

- Sync from peers: `PoolSynchronizer::getBlockReply` stores each block with
  `storeBlock(pool, PoolStoreType::Synced)` (csnode/src/poolsynchronizer.cpp:131-148). `BlockChain`
  applies the block to the wallet caches first (`applyBlockToCaches`, csnode/src/blockchain.cpp:1709),
  then emits `storeBlockEvent` (line 1738). `SmartContracts::on_store_block`
  (solver/src/smartcontracts.cpp:1407-1420) passes `reading_db = true` when the current round is
  more than one ahead of the block, so every synced block is processed as `reading_db`.
- Start-up read (full start, or quick start reading the blocks after the checkpoint): the storage's
  `readBlockEvent` goes to `BlockChain::onReadFromDB` (csnode/src/blockchain.cpp:395-424, wallet
  caches updated at line 421) and then to `SmartContracts::on_read_block` (connected at
  solver/src/smartcontracts.cpp:219, body at 1423-1442), always `reading_db = true`.
- Snapshot import: the snapshot holds the contracts database (csnode/src/snapshot_builder.cpp:385-408,
  `contracts.bin.lz4`) and the quick-start caches, including `smartcontracts.dat`
  (csnode/src/smartcontracts_serializer.cpp:18); the blocks after it are read as above.

In all three the wallet caches already include the block (its payouts and fees) when the contract
code sees it.

### 2.4 Rebuilding a state: `get_actual_state`

`on_next_block_impl` (solver/src/smartcontracts.cpp:1768-1860) calls `update_contract_state(tr, reading_db)`
for every `new_state` (line 1796). `update_contract_state` (2590-2770) ignores a `new_state` whose
`RefStart` is not newer than `item.ref_execute` (2618-2630), then calls `get_actual_state` (2632).

`get_actual_state` (1222-1405):

- returns the transaction as is when it carries `Value` (1228-1230; never the case for blocks made
  by the current protocol, 2.1);
- uses the cached state when `item.ref_execute == ref_start` and its hash matches (1270-1286);
- otherwise re-executes the call with `execute(exe_data, true /*validationMode*/)` and
  `explicit_last_state = item.state` (1287-1322);
- compares the hash of the result with the hash in the block (1343-1378). On a mismatch while
  `reading_db` it stores the wrong state in the cache anyway and returns it (1364-1377, "all future
  calls will be re-executed"); when live it asks the confidants (1359-1361);
- on an execution error, or when the contract is not the primary target of the call, it returns an
  empty value (1385-1387) and, when live, asks the confidants (1279-1281, 1296-1298). Since #83 it
  warns that the declared state is not available (1391-1398).

Network requests are made only when `!reading_db`.

With an empty value `update_contract_state` returns `false` without touching `item.ref_execute`,
`item.state` or the cache (2758-2769). With a non-empty value it updates the contracts database
(`dbcache_update`, 2671-2716), `item.state`, `item.ref_state`, `item.ref_execute`, `item.execute`
(2719-2753) and emits `contract_state_updated` (2756).

### 2.5 Why re-execution is not reproducible: the executor reads the current balance

The executor serves `getBalance()` from the node through apiexec `WalletBalanceGet`
(api/src/apihandler.cpp:3275-3283), which reads `blockchain_.findWalletData`, the current wallet
cache. During consensus the cache is at the previous block; during sync it already includes the
block being processed (2.3), so a contract that pays out from its balance sees the balance after
its own payout. The mainnet dice contract 9ATAmu2Y… derives the stake from that balance and the
re-execution ends with "insufficient CS attached": the state stays stale and `ref_execute` stays at
the previous call.

### 2.6 Fees of emitted transactions

A transaction emitted by a contract (source = contract) is first charged like any transaction: the
wallet cache takes its counted fee from the contract (csnode/src/walletscache.cpp:591-600, through
`loadNextBlock` 135-177). Then `on_next_block_impl` recognises it by its source
(solver/src/smartcontracts.cpp:1836-1855), loads the starter transaction of
`contract_item.ref_execute` and emits `signal_emitted_accepted(tr, starter)`, which reaches
`WalletsCache::Updater::smartSourceTransactionReleased` (csnode/include/csnode/blockchain.hpp:317,
csnode/src/walletscache.cpp:264-285): the counted fee goes back to the contract and is taken from
the starter's source. The comment at 1838-1839 states the assumption: "such transactions ALWAYS
follow the new_state, so corresponding new_state has already been processed".

When the state of that `new_state` was not available, `ref_execute` still points at an older call
and the fee is taken from that call's initiator. This is the one-fee difference on 7hWM6wvs…
(2026-10-23). Live nodes that had the state charge the right initiator; live nodes that did not
(state over 8192 bytes, packet missed, and the re-execution failed) charge the wrong one, which is
why official mainnet nodes also disagree among themselves (7 of 20 on 7hWM, 3 values on AL3dpJg3).

The rollback of a removed block looks the starter up differently: it finds the `new_state` of the
same contract earlier in the same block and uses its `RefStart` (1657-1680).

### 2.7 The existing state request and reply

- `SmartContracts::net_request_contract_state` (3317-3320) records the contract in
  `pending_state_requests` and sends `StateRequest` to the confidants of the current round.
- `Node::sendStateRequest` (csnode/src/node.cpp:1418-1429): round and contract key, signed with
  the node key, `sendDirect` to the given keys. The p2p layer routes a direct packet to any node id,
  looking the node up when it is not connected (p2p/src/host.cc:154-168), so the confidants need
  not be neighbours.
- `Node::getStateRequest` (1431-1462): checks the signature and answers with
  `blockChain_.getContractData(abs_addr)`, that is the contracts database entry: the `RefStart` of
  the call that produced the node's current state and the state value (`dbcache_update` format,
  solver/src/smartcontracts.cpp:3148-3150). Nothing else is checked: any node answers any signed
  request, with whatever state it currently has, stale or not.
- `Node::sendStateReply` / `getStateReply` (1464-1505): signed reply, handed to
  `net_update_contract_state`.
- `net_update_contract_state` (3323-3420): rejects a reply that was not requested or is older than
  1000 rounds (3337-3345), refers to a block after the current round (3348-3351) or is older than
  `item.ref_execute` (3353-3356). It verifies the state by loading the blocks from `ref.sequence`
  up to 100 further and looking for the `new_state` with that `RefStart`, comparing its `Hash`
  (3357-3398). **If the block is not found, the reply is accepted unverified** (only
  `state_found && !state_test_passed` rejects, 3395-3397). On acceptance it writes the cache and
  sets `item.state`, `item.ref_cache`, `item.ref_execute` (3404-3417).
- `StateRequest`/`StateReply` are always processed (csnode/src/node.cpp:2190-2191,
  net/src/packetvalidator.cpp:141-142). The handlers are unchanged since 4.2.532
  (`git diff 4a95aaaf..a6e640f2 -- csnode/src/node.cpp` touches none of them), so official nodes
  serve and accept them.

### 2.8 Caches, checkpoints, snapshots

- Contracts database: one record per contract, keyed by its public key
  (csdb/src/storage.cpp:1302-1320): `ref_start.sequence, ref_start.transaction, state`. There is no
  history; a node cannot serve a state other than its current one.
- Memory cache `known_contracts[abs_addr]` (`StateItem`): `payable, ref_deploy, ref_execute,
  ref_cache, ref_state, deploy, execute, state, uses`, serialized by `StateItem::to_bytes`
  (solver/src/smartcontracts.cpp:3651-3680) into `smartcontracts.dat` with the queue, blacklist and
  locks (`SmartContracts::serialize`, 3555-3594). Quick start restores it; `rehydrateContractDbCache`
  and `validateRestoredStatesAgainstChain` (csnode/src/node.cpp:212-213 and 249-250,
  solver/src/smartcontracts.cpp:3188-3300) write missing database records from it and invalidate a
  restored state whose hash differs from the chain, when the block is still there. On a pruned
  node the block is usually gone and the state counts as "unverifiable" (3230-3233). The local
  pruned node logs `2344 already present with non-empty state … 168 empty in RAM` and
  `2344 unverifiable`.
- The state digest covers wallets only (csnode/src/statedigest.cpp:114-129); contract states are
  compared in CI through the API (`SmartContractGet.objectState`, ci/testnet/contracts.py:554-565),
  which reads the contracts database (api/src/apihandler.cpp:1598, solver/src/smartcontracts.cpp:418-430).

## 3. Design

### 3.1 Fee attribution by the block (consensus-relevant)

In `on_next_block_impl`, for a transaction whose source is a known contract, the starter is the
`RefStart` of the nearest preceding `new_state` of that contract in the same block. Only when the
block has no such `new_state` does the code fall back to `contract_item.ref_execute`, with a
warning (the rollback at 1657-1680 already uses the same lookup, so the two stay symmetrical).

Effect: when the node has the state, nothing changes (`ref_execute` equals that `RefStart`). When
it does not, the fee is charged to the same wallet as on a live node that had the state. The
balances of a syncing node then no longer depend on whether any contract state was obtained.

Consensus implications: balances are not in the blocks, but a node validates transactions against
them (`IterValidator`, csnode/src/itervalidator.cpp:497 and 526, "insufficient balance") and
reports them through the API. After this change our node agrees with the live nodes that had the
state (on 7hWM the 13 of 20) and disagrees, by one fee per affected payout, with official nodes
that synced the same blocks from scratch and with live official nodes that missed the state. That
disagreement exists today among official nodes; this change picks the side of the nodes that
executed the contract. A node already synced with today's code keeps its balances until it syncs
again from block 0 or from a snapshot made by a fixed node (see 3.7).

### 3.2 What the chain declares: `declared_ref`, `declared_hash`

`StateItem` gets two fields, set in `update_contract_state` for every `new_state` whose `Hash` is
not `Zero::hash`, before and regardless of whether the state value is obtained:

- `declared_ref`: the `RefStart` of that `new_state` (the call that produced the state);
- `declared_hash`: its `Hash`.

A `new_state` with `Zero::hash` (failed execution) leaves them unchanged: the current state is still
the one of the last successful call. Both are serialized after the existing fields of
`StateItem::to_bytes`; `from_bytes` reads them when present and leaves them invalid for old
checkpoints (backward compatible, the snapshot format does not change).

`is_stale(item)` = `declared_hash` is set and (`item.state` is empty or `hash(item.state) !=
declared_hash`). A contract with an unknown `declared_hash` (old checkpoint, block pruned) is
"unverifiable", not stale.

`validateRestoredStatesAgainstChain` fills `declared_*` from the chain when the block of
`item.ref_state` is still stored (the hash it already reads at 3246-3253); the "unverifiable" count
then means only "block pruned and checkpoint without declared hash".

### 3.3 Why states after old calls cannot be fetched

Every node keeps one state per contract (2.8) and answers a request with it (2.7). A node syncing
through block N that needs the state after call C can only get a peer's current state, which is the
state after the peer's latest call C' ≥ C. The only way to serve historical states would be a
state history on every serving node, a new request carrying the ref, and serving nodes running
that code; official nodes would never serve it, and the history would grow with every call of every
contract (the dice contract alone makes a `new_state` per roll). Not proposed.

So "pause until the state arrives" (option a of the question) is not possible for a block behind
the head; at the head it is unnecessary, because a live `new_state` arrives with its packet state
(2.2) or triggers the request (2.4). Applying blocks is also synchronous in the sync path
(csnode/src/poolsynchronizer.cpp:144, under the node's message loop), so a wait there would stall the
node.

What is possible: with 3.1 the balances are right without any state; the state matters only for
executing future calls (as a confidant), for the API and for smart consensus votes, and for those
the current state is what is needed. It is fetched once the node is at the head (3.4).

### 3.4 Fetching the current states

**Trigger.** A fetch cycle starts

1. when the node becomes live: the first `on_store_block` with `reading_db == false` after a run of
   `reading_db` blocks, and `PoolSynchronizer::synchroFinished` (csnode/src/poolsynchronizer.cpp:428-433)
   — the first live block is the simpler single hook, it covers all three paths of 2.3;
2. after every live block that leaves a contract stale (today's calls at 1279, 1296, 1359 become
   "mark stale; the fetcher asks");
3. periodically while stale contracts remain (every `kRetryRounds` = 50 rounds, with the per-contract
   backoff below).

**Queue and pacing.** A `StateFetcher` (new, solver/src/statefetcher.{hpp,cpp} or inside
`SmartContracts`) keeps for each stale contract: the round of the last request, the number of
attempts. Per round it sends at most `kRequestsPerRound` = 16 requests, oldest first, skipping a
contract asked less than `min(50 × 2^attempts, 5000)` rounds ago. The mainnet node has ~2.5k
contracts; a full first pass takes about 160 rounds when everything is stale (the
`sync_contract_execution=off` case, 3.6) and a few rounds when only the non-reproducible contracts
are. `pending_state_requests` and its 1000-round expiry (3334-3345) are replaced by the fetcher's
bookkeeping.

**Peers.** The confidants of the current round, as today (3319): they are live and execute the
contracts, so they hold the state the chain declares. One request reaches all of them
(`sendMulticast`); several answer. No new message, no new peer selection. A later refinement, if the
confidants turn out not to answer (official nodes with a stale state of their own answer with it,
which the verification rejects), is to add the neighbours; not in the first version.

**Verification** (`verify_state_reply(reply_ref, state, item)` → `Accepted | Newer | Older |
HashMismatch | Unverifiable`), replacing 3357-3398:

- `reply_ref > declared_ref`: the peer executed a call this node has not reached (we are one or two
  rounds behind): ignore, the next cycle asks again once `declared_ref` has advanced;
- `reply_ref < declared_ref`: the peer's state is stale: ignore;
- `reply_ref == declared_ref` and `hash(state) == declared_hash`: accept;
- `reply_ref == declared_ref` and hash differs: the peer holds a wrong state for the right call:
  ignore, log at warning once per contract and peer;
- `declared_ref` unknown (3.2): fall back to today's check against the block (3357-3398) and
  **reject when the block cannot be loaded** (closing the hole at 3395-3397). A pruned node with an
  old checkpoint therefore cannot repair a contract whose last call is pruned until that contract is
  called again (3.7).

On acceptance: `dbcache_update(..., force = true)` (the cache may hold a wrong state from 1364-1377
with the same ref, which `force = false` would keep), `item.state`, `item.ref_cache`,
`item.ref_execute = reply_ref`, `contract_state_updated` emitted (today's net path does not emit it;
the API's deploy list and token tracking listen to it, so it should).

**Logging.** One `cslog` per accepted state ("state of X after {s.t} received from peer, verified"),
one `cswarning` per contract when the first cycle finds no verified reply, and a summary line per
cycle: `contract states: N known, S stale, R requested, A accepted this round`. The summary at the
end of sync and at every 1000th round is what tests and operators read.

### 3.5 When no peer can provide a state

The node keeps what it has (the provisional re-executed state, or nothing), the contract stays
stale, and the fetcher keeps asking with the backoff of 3.4 for as long as the node runs. This is no
worse than today, where the stale state is kept silently and never asked for. A stale contract is
executed, if this node is a confidant, with the stale state, as today; its vote then differs from
the majority and is outvoted. Official peers behave as our peers: they answer with their current
state, which passes or fails the verification.

A stale count that stays above zero for a long time is reported in the periodic summary (3.4).

### 3.6 Re-execution during sync: keep, as a provisional source, behind a switch

With 3.1, re-execution during sync no longer affects balances; its only effects are provisional
states (right for reproducible contracts, which are most of them) and executor time. Two options:

- `sync_contract_execution = on` (today's behaviour): the executor re-executes every call during
  sync; at the head only the contracts that ended stale (hash mismatch, failures) are fetched. A
  state no peer serves stays at the re-executed value.
- `sync_contract_execution = off`: `get_actual_state` returns the empty value without calling the
  executor when `reading_db`; every contract is stale at the head and fetched (one pass of ~160
  rounds on mainnet). Sync does not need the executor at all (relevant for the Docker image and the
  snapshot workflow), and no wrong state is ever written to the cache. A state no peer serves stays
  empty.

Recommendation: implement the switch, default `on` in the first release (no behaviour change for
reproducible contracts, no dependence on peers answering), measure both in the scale test (sync
time, stale count after the head), and decide the default then. The strict reading of "never
recomputed" is `off`; it is one config line once the fetcher is proven.

The wrong-hash branch at 1364-1377 is kept with `on` but now leaves the contract stale (its
`declared_hash` differs), so the fetcher repairs it.

### 3.7 The local node and the published snapshots

The checkpoints and snapshots made so far carry states re-executed by today's code with no
`declared_*` fields. After the change:

- a contract whose last successful `new_state` is in a block the node still stores gets
  `declared_*` at start-up (3.2) and is repaired by the first fetch cycle if stale;
- a contract whose last call is pruned stays unverifiable until it is called again; then the live
  `new_state` sets `declared_*` and the usual live path (packet state, or request) repairs it. The
  dice contracts are called many times a day, so they heal within hours; a contract nobody calls
  keeps its possibly stale state, which nothing reads;
- balances stay as they are: fixing them needs a sync from block 0 (full node) or a snapshot made
  by a node that synced with 3.1. The snapshot workflow starts from the previous snapshot, so a
  one-time rebuild of the snapshot chain from a fixed full sync is needed for the published
  snapshots to carry correct balances. Open question for the user (5).

An operator-triggered "majority repair" (accept a state that k of n confidants return identically,
without chain verification) would repair the unverifiable contracts at once; it accepts states on
trust and is not proposed unless the user wants it.

### 3.8 Paths covered

| Path | reading_db | 3.1 | 3.4 trigger |
| --- | --- | --- | --- |
| sync from peers (storeBlock Synced) | yes | yes | first live block / synchroFinished |
| quick start, blocks after the checkpoint | yes | yes | first live block |
| full start-up read | yes | yes | first live block |
| snapshot import | (as quick start) | yes | first live block; the snapshot's `smartcontracts.dat` has `declared_*` once made by a fixed node |
| live block without packet state | no | yes (no change in effect) | after the block, as today, through the fetcher |

## 4. Tests

### 4.1 Unit tests (tests/csnode/smartcontracts_tests.cpp, gtest)

`SmartContracts` needs a `BlockChain` and a scheduler (solver/include/solver/smartcontracts.hpp:328),
so the logic is put in static functions and tested there:

- `SmartContracts::emitting_new_state(pool, tr_idx, abs_addr)`: a synthetic block with
  `new_state(A)`, `emitted(A)`, `new_state(B)`, `emitted(B)`, `emitted(A)` → the right `RefStart`
  for each, `nullopt` when there is none before the transaction.
- `verify_state_reply`: the five outcomes of 3.4, including the unverifiable case rejecting a reply
  when the block cannot be loaded.
- `StateItem` round trip with `declared_*`; bytes written by the old `to_bytes` read back with
  invalid `declared_*`.
- `is_stale` and the fetcher's pacing/backoff with a fake round counter (no network).

### 4.2 The reproduction on a private network (fails with today's code)

`ci/testnet/contracts.py --dice` (run.sh `CONTRACT_DICE=1`, needs `CONTRACTS=1`) deploys, in API v3
with `payable()`:

```java
public class Dice extends SmartContract {
    private int plays;
    private String lastPlayer;
    public Dice() { super(); }
    public int play() {
        BigDecimal balance = getBalance(contractAddress);
        if (balance.compareTo(new BigDecimal("20")) < 0) {
            throw new RuntimeException("insufficient balance " + balance);
        }
        sendTransaction(contractAddress, initiator, 10.0, new byte[0]);
        plays += 1;
        lastPlayer = initiator;
        return plays;
    }
    @Override public String payable(BigDecimal amount, byte[] userData) { return "ok"; }
}
```

(`contractAddress`, `initiator`, `getBalance(String)`, `sendTransaction(String, String, double,
byte[])` as in `com.credits.scapi.v3.SmartContract` of the packaged sc-api-support.jar.)

Sequence, before the restart test, from two player accounts A and B (the writer accounts of
contracts.py, line 315, funded by the master): fund the contract with 25 CS (a transfer to the
contract address, which runs `payable()`), then three times: A or B in turn calls `play()`, then
the master tops the contract up by 10 CS. Each `play()` sees a balance between 20 and 30 when
executed live and pays 10; a node re-executing it sees the balance after the payout and the fees,
below 20, and throws. The player alternates so that the mis-attributed fee lands on another wallet
(with one player it lands on the same wallet and the balances agree by accident).

Scenario `contracts-dice-join` in robustness.yml: `DB_BACKEND=lmdb CONTRACTS=1 CONTRACT_DICE=1
JOIN_PRUNED_AT=500 JOIN_FULL=1 CONTRACTS_AFTER_AT=700 TARGET_SEQ=1300`. The joining node syncs
from block 0 through the three plays. Expected with today's code:

- run.sh: `STATE DIGEST` of the joining node differs from the others at #1000 (the fee of each
  payout charged to the previous player, or to the master for the first play);
- contracts.py: "Dice state per node" shows a different state hash on the joining node
  (`plays` stayed at 0, 1 or 2), "FAIL: nodes disagree on the Dice state";
- the joining node's log has three "declared by the block is not available while reading the
  blockchain" lines for the dice address.

The scenario is added first and marked `manual: true` with a comment "reproduction of the contract
state divergence, fails until #<fix>", run by name; the flag is removed when it passes.

### 4.3 After the fix

The same scenario passes: identical digests, identical Dice state on every node including the
joiner, and the joiner's log has `state of <dice> after {s.t} received from peer, verified` and a
final summary with `0 stale`. With `SYNC_CONTRACT_EXECUTION=off` (run.sh passes it to the joining
node's config) the same, plus no executor call during its sync (no "state after … has updated"
line before the first live block) and `S stale = N known` in the first summary.

`contracts-pruned-term-kill` and `contracts-power-cut` with `CONTRACT_DICE=1` cover the quick-start
read (the dice calls between a checkpoint and the kill are re-read); they are not deterministic
reproductions, only coverage.

### 4.4 Scale test

scale.yml `scenario` input (feat/snapshot-importer, or master once merged):
`CONTRACTS=1 CONTRACT_DICE=1 JOIN_PRUNED_AT=500 JOIN_FULL=1 TARGET_SEQ=1600`, 51 nodes, 25 trusted:
the joiner asks 25 confidants, the pacing and the multicast replies are exercised at mainnet-like
fan-out. Pass: as 4.3; the summary shows the fetch finished within 50 rounds of the first live block.

Optional: `node_alt_ref` = a build of 4a95aaaf on the even nodes (robustness.yml:128-162 builds an
alternate node; the submodules must match, line 138), which shows official nodes serving the
reply to our joiner. Whether 4a95aaaf builds against the current submodules has to be checked.

## 5. Open questions for the user

1. 3.1 changes balances of a syncing node relative to official nodes that synced the same blocks
   (and matches the live ones). Confirm this is the side to take.
2. Default of `sync_contract_execution` in the first release: `on` (recommended until the fetch is
   measured) or `off` (strict "never recomputed").
3. The published snapshot chain carries balances computed by today's code. Rebuild it once from a
   full sync with the fix, or let it be (balances off by a few fees on contract initiators' wallets
   until then)?
4. Is a "majority repair" of the unverifiable contracts of the local pruned node wanted (3.7), or is
   healing on the next call enough?
5. Should the fetcher ask the neighbours too when the confidants give no verified reply, in the
   first version or later?

## Decisions (2026-10-24)

1. Fee attribution: the rule of the live nodes (step 1), consensus-relevant, approved by the user.
2. `sync_contract_execution` on by default in the first release (provisional re-execution, then the fetch).
3. The published snapshot chain is not rebuilt now; a rebuild from a fixed full sync is a later decision.
4. No unverified "majority repair" for contracts whose last call is pruned.
5. Neighbours as sources: a later version; confidants first.

