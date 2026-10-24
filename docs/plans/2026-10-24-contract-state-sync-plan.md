# Contract states during sync: implementation plan

Companion of `contract-state-sync-design.md` (same directory). Code references to origin/master
a6e640f2. Each step is one PR; the order matters: step 0 must fail before step 1, and steps 1-3 are
verified with the scenario of step 0. Steps marked **consensus-relevant** change balances or
contract states a node reports and need the user's go-ahead before merge.

Sizes: S = under a day, M = one to two days, L = more.

## Step 0 — Reproduce the divergence on a private network (S/M, CI only)

Files: `ci/testnet/contracts.py`, `ci/testnet/run.sh`, `.github/workflows/robustness.yml`.

1. `contracts.py`: add the `Dice` contract of design 4.2 (API v3, `payable()`), `--dice` flag:
   - deploy from the master, like `Ledger` (`deploy()` around line 257);
   - fund the contract with 25 CS from the master: a plain transfer to the contract address. Check
     first that a transfer to a contract with `payable()` is accepted by node 1's API on the test
     network (it runs `payable()` through smart consensus, `is_payable_target`); if it is not, fund
     the contract in the deploy transaction's amount instead (verify the node accepts a deploy with
     an amount);
   - three rounds before `--ready-file`: `play()` from player A, then B, then A (two of the writer
     accounts of line 315, funded by the master with a few CS for fees), each followed by a 10 CS
     top-up from the master; each `play()` must return 1, 2, 3 on node 1;
   - add `("Dice", dice)` to the final comparison (line 554) and a `read_all(dice, "plays", …)`
     if a getter is added;
   - `write_sources()` writes `Dice.java` for the compile check.
2. `run.sh`: `CONTRACT_DICE` (default 0, needs `CONTRACTS=1`) passes `--dice`; document it in the
   header next to `CONTRACTS`.
3. `robustness.yml`: scenario `contracts-dice-join`, `env: DB_BACKEND=lmdb CONTRACTS=1
   CONTRACT_DICE=1 JOIN_PRUNED_AT=500 JOIN_FULL=1 CONTRACTS_AFTER_AT=700 TARGET_SEQ=1300`,
   `contracts: true`, `manual: true`, comment: reproduction, fails until step 3.
4. Run it by name. Pass criterion of this step is a **failure** with the three signs of design 4.2:
   digest mismatch of n8 at #1000, "FAIL: nodes disagree on the Dice state", and the "declared by
   the block is not available while reading the blockchain" lines in n8's log. Keep the run URL and
   the three log excerpts in the PR description.

If the re-execution does not fail (the executor's `getBalance` not reading the node's cache, or the
fees making the balance land differently), adjust the thresholds (20 / 10 / 25) until the live play
passes and the re-execution throws; the log of n8 shows the executor error text.

## Step 1 — Fee attribution by the `new_state` in the block (S, **consensus-relevant**)

Files: `solver/src/smartcontracts.cpp` (`on_next_block_impl` 1836-1855), `solver/include/solver/smartcontracts.hpp`,
`tests/csnode/smartcontracts_tests.cpp`.

1. Static `std::optional<SmartContractRef> SmartContracts::emitting_new_state(const csdb::Pool&,
   size_t tr_idx, const csdb::Address& abs_addr)`: scan `tr_idx - 1 … 0` for `is_new_state(t) &&
   absolute_address(t.target()) == abs_addr`, return its `RefStart`. (The address comparison needs
   the blockchain for optimized addresses: pass a resolver, or make it a member and test the member
   through a block whose addresses are public keys; the existing `absolute_address` handles both.)
2. In `on_next_block_impl`: starter = `get_transaction(*emitting_new_state(...), abs_addr)`; if
   none in the block, `cswarning` and fall back to `contract_item.ref_execute` as today.
3. `on_remove_block` 1657-1680 already does the same; make it call the helper so the two cannot
   drift.
4. Unit test: synthetic block `new_state(A), emitted(A), new_state(B), emitted(B), emitted(A),
   plain transfer`; expected refs; `nullopt` for an emitted transaction before any `new_state`.
5. Pass: `contracts-dice-join` now has identical `STATE DIGEST`s on all nodes (n8 included); the
   Dice state still differs (expected, noted in the PR); all other robustness scenarios unchanged.

## Step 2 — Record what the chain declares (M)

Files: `solver/include/solver/smartcontracts.hpp` (`StateItem`), `solver/src/smartcontracts.cpp`
(`update_contract_state` 2590-2770, `StateItem::to_bytes/from_bytes` 3651-3700,
`validateRestoredStatesAgainstChain` 3215-3300, `toString` 3805-3815), `tests/csnode/smartcontracts_tests.cpp`.

1. `StateItem`: `SmartContractRef declared_ref; cs::Hash declared_hash{}` (zero = unknown).
2. `update_contract_state`: right after `ref_start` is known and before the duplicate/outdated
   checks (2618-2630), if `t.user_field(new_state::Hash)` is valid, 32 bytes and not
   `Zero::hash`, set `declared_ref = ref_start`, `declared_hash = hash` when `ref_start >
   declared_ref`. Also when the state ends empty.
3. `to_bytes` appends `declared_ref` and `declared_hash`; `from_bytes` reads them only if bytes
   remain (`is.isAvailable`), else leaves them unset. No format version bump needed (append-only,
   the quick-start hash check covers the file).
4. `bool is_stale(const StateItem&)` as in design 3.2; `size_t count_stale()` for the summary.
5. `validateRestoredStatesAgainstChain`: when it loads the `new_state` of `ref_state` (3246-3253)
   and `declared_*` is unset, set them from it; count "declared from chain" in its log line.
6. Unit tests: round trip; old bytes (produced by a copy of the old `to_bytes` in the test) read
   back with unset `declared_*`; `is_stale` for empty state, matching hash, differing hash,
   unknown hash.
7. Pass: unit tests; `contracts-pruned-term-kill` and `snapshot-join` unchanged (checkpoints with
   the new bytes load, old checkpoints from the previous build load: the `upgrade-compat.yml`
   workflow covers a checkpoint made by the previous release).

## Step 3 — Fetch and verify at the head (M/L)

Files: new `solver/include/solver/statefetcher.hpp`, `solver/src/statefetcher.cpp` (or inside
`SmartContracts`); `solver/src/smartcontracts.cpp` (`get_actual_state` 1279/1296/1359,
`on_store_block` 1407-1420, `net_request_contract_state` 3317, `net_update_contract_state`
3323-3420, `clear` 3548); `csnode/src/node.cpp` (`getStateReply` 1479-1505 unchanged in format);
`tests/csnode/smartcontracts_tests.cpp` (+ `statefetcher_tests.cpp`).

1. `verify_state_reply(reply_ref, state, const StateItem&, block_loader)` → enum of design 3.4;
   the unverifiable branch keeps today's block scan (3357-3398) but returns `Unverifiable` (reject)
   when no block with the `new_state` can be loaded.
2. `StateFetcher`: `mark_stale(addr)`, `on_round(round, confidants, send)` (pacing 16/round,
   backoff `min(50 × 2^attempts, 5000)` rounds), `on_reply(addr)` (clears the entry on acceptance),
   `summary()`. Pure: the send is a callback, tested with a fake.
3. `SmartContracts`:
   - `on_store_block`: when the block is the first with `reading_db == false` after `reading_db`
     ones (a `was_reading_db` flag), mark every `is_stale` contract and log the summary;
   - every live block: `fetcher.on_round(...)` (from `on_next_block_impl`'s live path or
     `test_exe_queue`'s caller);
   - the three `net_request_contract_state` calls in `get_actual_state` → `fetcher.mark_stale`
     (the request itself goes out with the pacing); the `on_remove_block` call at 1612 likewise;
   - `net_update_contract_state`: pending check → `fetcher.is_pending(addr)`; verification →
     `verify_state_reply`; acceptance → `dbcache_update(force = true)`, `item.*`,
     `contract_state_updated(t_state)` built from the reply (the signal currently is only emitted
     at 2756);
   - `clear()` resets the fetcher.
4. Logging as design 3.4 (accepted state at `cslog`, no verified reply at `cswarning` once per
   contract, summary at the first live block and every 1000 rounds while stale > 0).
5. Tests: `verify_state_reply` five cases; fetcher pacing and backoff; the existing
   `pending_state_requests` behaviour (reply not requested is ignored) preserved through the
   fetcher.
6. Pass: `contracts-dice-join` passes (identical digests and Dice state on all nodes, n8's log has
   the "received from peer, verified" line for the dice address and a final summary with `0
   stale`); remove `manual: true` from the scenario. `contracts-pruned-join`, `contracts-crash`,
   `contracts-power-cut`, `contracts-net-loss`, `snapshot-join` unchanged.

## Step 4 — `sync_contract_execution` switch (S)

Files: `client/include/config.hpp`, `client/src/config.cpp` (where `alwaysExecuteContracts` lives),
`solver/src/smartcontracts.cpp` (`get_actual_state` 1287), `packaging/*/config.ini` template,
`ci/testnet/run.sh` (`SYNC_CONTRACT_EXECUTION`, written to every node's config), docs.

1. `[Sys] sync_contract_execution = true|false` (default true in this step).
2. `get_actual_state`: when `reading_db && !sync_contract_execution`, skip the executor branch
   (1287-1382) and return the empty value with a `csdebug` instead of the warning at 1391-1398
   (the state is expected to be missing).
3. Scenario `contracts-dice-join-nosync-exec`: the same as `contracts-dice-join` with
   `SYNC_CONTRACT_EXECUTION=0`; pass as step 3 plus: n8's log has no "state after … has updated"
   line before its first live block, and the first summary shows `stale = known`.
4. Scale test (design 4.4) twice, on and off: sync time of the joiner, rounds from the first live
   block to `0 stale`. Report both to the user for the default decision.

## Step 5 — Documentation and release notes (S)

- `docs/CHANGES.md` entries for steps 1-4 (consensus-relevant marked as such).
- Operator note in the README/docs: what the summary line means, that a stale count that stays
  above zero means no confidant serves a verified state, the switch of step 4.
- `docs/specs/`: move the design there as `2026-10-24-contract-state-sync-design.md` with the
  decisions of the open questions filled in.

## Step 6 — Local node and snapshots (user decision, outside the PRs)

After step 3 is released: restart credits-pruned-c3 on the new build, read the start-up
`validateRestoredStatesAgainstChain` line (declared from chain / unverifiable counts) and the first
summary (stale, accepted). Expected: the contracts called within the stored block window are
verified or repaired; the rest heal on their next call. If the user wants correct balances in the
published snapshots, a one-time full sync with step 1 and a new first snapshot from it (the
snapshot workflow then continues from there).

## Consensus-relevant summary

| Step | Changes what | Agrees with | Disagrees with |
| --- | --- | --- | --- |
| 1 | fee attribution of emitted transactions when the state is unavailable | live nodes that had the state (the majority on 7hWM) | official nodes that synced from scratch; live official nodes that missed the state |
| 3 | contract states of a synced node (replaced by verified current states) | every node holding the chain-declared state | nodes holding a stale state of their own (they are wrong by the chain's hash) |
| 4 (off) | no states during sync | — | — (states missing until the head, then as step 3) |
