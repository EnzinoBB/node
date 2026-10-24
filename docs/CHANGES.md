# Changes in the community node

Every pull request merged into this fork since it branched from
[CREDITSCOM/node](https://github.com/CREDITSCOM/node) at `4a95aaaf` (node 4.2.532), grouped by
the first community release that contains it. For what these changes mean when running a node, see
[differences.md](differences.md). Each release is published as
`community-v4.2.532-community.N`, with full and pruned packages and the Docker image
`ghcr.io/enzinobb/credits-node`.

Add a line here with every merged pull request. When a release is cut, move the "Not released yet"
lines under the new release.

## In short

- **Storage.** You can choose the block database: BerkeleyDB (the official one), LMDB or RocksDB.
  An existing database converts to another backend in place, and an interrupted conversion
  resumes. See [storage.md](storage.md).
- **Pruned nodes.** A pruned node keeps only the latest blocks. It starts from a snapshot, and
  rebuilds its transaction index and block hashes without a slow start.
- **Safety fixes.** Remote crashes and out-of-bounds reads in network and block parsing are fixed,
  and so are races in the smart-contract solver. Quick-start checkpoints are checked against the
  chain, and governance state now survives a quick start.
- **Wallet state digest.** A digest of all wallet state, logged every 1000 blocks, lets different
  nodes and builds compare their state.
- **Continuous integration.**
  - unit tests;
  - private networks of 5 to 61 nodes, with smart contracts read and written;
  - a robustness suite: crashes, power cuts, full disks, network faults, mixed binaries, every
    storage backend, conversions;
  - upgrades from the official node, tested on mainnet data;
  - release packages and the Docker image.

## Not released yet

| PR | Change |
|---|---|
| #87 | csnode: every checkpoint saves the digest of the state it holds (wallets, their count, consensus settings) as `state_digest.bin` and logs it; a quick start recomputes it and moves aside a checkpoint whose caches do not match |
| #88 | client: `[snapshot]` section of `config.ini` for snapshots served by community nodes (no behaviour yet) |
| #90 | csnode: `BlockVerifier` for blocks that come from outside the consensus (hash, signatures against the block's confidants, round confirmations), for the snapshot import and backward sync |
| #89 | csnode, client: snapshot format 2 built by the node from its own checkpoint and blocks on any backend (lz4, per-chunk SHA-256, manifest signed with the node key); `--export-snapshot <dir>` |

## community.11 — 2026-10-24

| PR | Change |
|---|---|
| #82 | csnode: a node still syncing logs a missing round table of the network's current round as a warning (the block comes with the sync); an error only when it is not behind |
| #83 | packaging, solver: `compatible_version=true` in the mainnet `config.ini` (the binary's default; the official file's `false` stops a syncing node for good at a block whose transaction its replayed balances reject); a warning when a contract state declared by a block is not available while replaying |
| #84 | ci(snapshot): `digest_offline.sh` reads the start-up digest from the run's console output (on a node with long-lived logs, log rotation made it wait for its whole timeout and fail) |

A mainnet node syncing with the packaged `config.ini` could stop for good at a valid block: replaying
some contracts is not reproducible (official behaviour), and its balances then reject the block's
transaction. The packaged config now uses the binary's default (#83), and the node warns when a
replayed contract state is lost. #82 and #84 are logging and snapshot-tooling fixes. No consensus
change.

## community.10 — 2026-10-23

| PR | Change |
|---|---|
| #67 | csdb, ci, docs: conversion follow-ups from the review of #65 (contract-records message, keep a copy of `db/` before converting from berkeleydb, sturdier db-convert scenario) |
| #70 | csnode: bootstrap tables of 9 to 25 confidants are accepted (the official node silently drops them, so a network of 9 or more initial confidants never starts); the same choice as the official node up to 8 |
| #71 | csnode: a bootstrap round table is taken only for a round next to the node's own, and, unless signed with the blockchain key, only once the node's own round has stalled; the block-removal request it set (never effective) is gone |
| #68 | ci(testnet): resource sampling (host and per-process memory and CPU, block rate) in every private-network run; a `nodes` input for testnet and robustness |
| #69 | ci: smart contracts in the robustness suite (crashes, a power cut, packet loss, a pruned node killed and joining, a database conversion) |
| #72 | ci: contract reads on every node and a contract load phase; networks of up to 61 nodes with 5 to 25 initial confidants, stake-based election and nodes below the minimum stake; a scale workflow |
| #73 | api: the executor version check asks every second up to `executor_check_version_delay` instead of once after it (the node starts as soon as its executor answers); with `executor_multi_instance` a stopping node no longer kills the other nodes' executors |
| #74 | api: contract getters and the deployer's contract list take the deploy the smart contracts keep (a pruned node no longer has its block, and answered getters with an empty success); a getter with nothing to execute, or no executor, now fails |
| #75 | ci(testnet): the contract load window starts after funding its writers, block rates from the first block (found in the slowdown analysis of the scale runs) |
| #76 | ci(testnet): round_elapse_time 20 s by default (10 s pre-empted slow rounds with a bootstrap); a run that ends stuck in a bootstrap round fails; bootstrap-livelock exercises an official stage-1 hash race (intermittent; run by name) |
| #79 | csnode, ci: a node that keeps its own block while the round goes on with another block of the same sequence logs a warning (a fork from a stage-3 retry race, official behaviour); a test network run with such a fork fails |
| #80 | csnode: a trusted node whose block of the last round differs from the writer's only in the real trusted set takes the writer's block when a majority of the round's trusted signed it (verified against its own confidants) and the other set cannot form a round; fees and reward move to the new set; the official "more signatures" replacement, whose consistency check could never pass, now works (the rebuilt block carries the reward and the bootstrap flag). Consensus-relevant |

#70, #71 and #80 change what a node does in consensus edge cases (bootstrap with more than 8
confidants, bootstrap tables checked against the node's own state, two blocks of one round); see
[differences.md](differences.md). #73 and #74 fix the contract executor start and contract reads on
pruned nodes. The rest is CI: smart contracts in the robustness suite, networks of up to 61 nodes,
stake-based election and nodes below the minimum stake, a scale workflow, fork detection.

## community.9 — 2026-10-22

| PR | Change |
|---|---|
| #57 | csdb: the RocksDB block writer owns its batch, and a full disk neither hangs nor stops the node |
| #58 | csnode: pruning holds `dbLock_` while it loads and removes blocks |
| #59 | csnode: a saved deferred block leaves the deferred slot; a block that fails to apply stops the node (`Node::stopOnError`) |
| #60 | csnode: checkpoints ahead of the blocks are set aside (quarantined in `qs/<v>.bad`); replaced hashes leave hashdb |
| #61 | csnode: a pruned node answers NoSuchBlocks for blocks it pruned |
| #62 | csnode, csdb: pruned history walk, legacy index watermark, small databases detected, 0-based `seq_no`, a failed LMDB open stays closed |
| #63 | csdb: the conversion estimate counts RocksDB compression; BerkeleyDB writes fail cleanly, and recovery errors stop the node |
| #64 | packaging: atomic snapshot restore (`.restore-incomplete`), fresh digest lines, logs read in time order, no snapshot of a running node |
| #65 | csdb, client: once every block is converted, a conversion finishes without an old database that does not open; the BerkeleyDB source is checkpointed after each compaction; the db-convert scenario covers deterministic kill points |
| #66 | docs: this file, [differences.md](differences.md), the README and the conversion disk space brought up to date |

PRs #57–#64 fix the findings of an independent review of the whole fork. #65 fixes the
db-convert failure that a robustness run found on master afterwards.

## community.8 — 2026-10-19

| PR | Change |
|---|---|
| #55 | ci: the monitor history check walks the oldest pages; no bytecode committed |
| #56 | csnode: the block hashes databases are rebuilt when they fail; no snapshot is taken from a damaged LMDB environment |

## community.7 — 2026-10-19

| PR | Change |
|---|---|
| #54 | csnode: every node rebuilds its transaction index after a quick start, instead of falling back to a slow start |

## community.6 — 2026-10-19

| PR | Change |
|---|---|
| #49 | csdb: detect the block database format and convert it to another backend, resumably |
| #50 | client: check the block database at start-up and convert it with `--convert-db` / `--convert-only` |
| #51 | docker, packaging: convert the block database with `CREDITS_CONVERT_DB=yes` |
| #52 | ci, docs: block database conversion on the private network and on mainnet data; storage documentation |
| #53 | csnode: a pruned node rebuilds its transaction index from its blocks, instead of falling back to a slow start |

## community.5 — 2026-10-17

| PR | Change |
|---|---|
| #44 | csnode: take over the official node's transaction index watermark |
| #45 | csnode: ignore unsolicited one-block replies to the disabled necessary-block request |
| #46 | api: `TransactionsListGet` skips pruned or unreadable blocks |
| #47 | csdb: sync the BerkeleyDB log before a checkpoint is saved |
| #48 | ci(robustness): every block database, full and pruned, in one network |

## community.4 — 2026-10-16

| PR | Change |
|---|---|
| #41 | Docker image and pruned-node snapshots |
| #42 | release: packages for Ubuntu 20.04 and later (glibc 2.31) |
| #43 | ci: upgrade an official node folder to the community node and back |

## community.3 — 2026-10-15

| PR | Change |
|---|---|
| #20 | api, csnode: serve contract deploys restored by a quick start |
| #21 | ci(testnet): smart contracts with the mainnet executor across the restart test |
| #37 | RocksDB block database, three fork fixes, robustness suite |
| #38 | ci(testnet): smart contracts with the mainnet executor and mixed executors |
| #39 | ci: mixed node binaries, network faults, slow nodes and random datagrams on the private network |
| #40 | ci(testnet): wait for a transfer still in progress before its sender sends again |

## community.2 — 2026-10-13

| PR | Change |
|---|---|
| #35 | ci(known-hosts): a working weekly run on Actions, with a verified seed list |
| #36 | packaging: update known hosts |

## community.1 — 2026-10-13

The first community release: everything from the fork point up to the release packages.

**Continuous integration and test network**

| PR | Change |
|---|---|
| #1 | CI: build and test with GitHub Actions; fix two stale csnode tests |
| #3 | tests: fix the `Process.HighLoadUsage` hang (a lost wake-up in the test) |
| #15 | ci: private 7-node test network with state-digest comparison |
| #16 | ci(testnet): disk-full scenario and manual run parameters |
| #19 | ci(testnet): governance orders, delegations and quick-start replay checks |
| #23 | ci(testnet): pruned-storage scenarios |
| #34 | ci: community release packages (full and pruned) and a weekly verified known_hosts list |

**Crashes and memory safety**

| PR | Change |
|---|---|
| #2 | lib: `LockFreeChanger` reads its `shared_ptr` atomically (a data race and crash) |
| #4 | csnode: validate `CompressedRegion` before decompressing it (a remote crash during sync) |
| #5 | csdb: reject pools whose hashing length points past their data (an out-of-bounds read) |
| #6 | net: reject empty packets before reading their flags (a remote crash) |
| #7 | csdb: bound `Pool::reserve()` by the bytes left to parse (a huge allocation) |
| #24 | lib: read class and type names for logs without nameof's fixed offsets |

**Consensus, governance and quick start**

| PR | Change |
|---|---|
| #8 | csnode: fix the swapped mining and staking flags on restore; warn about dropped rewards |
| #9 | csnode: don't keep an uncertain block's rewards when the trusted mask differs |
| #10 | csnode: check quick-start checkpoints against the chain before loading them |
| #11 | csnode: make the revert of a timed delegation mirror its apply |
| #12 | csnode: give BlockChain's consensus-settings members defined defaults |
| #14 | Wallet state digest (ECMH over ristretto255), logged every 1000 blocks |
| #17 | csnode: keep pending consensus-settings changes across a quick start |
| #18 | csnode: replay the other special orders after a quick start |

**Pruned storage and LMDB**

| PR | Change |
|---|---|
| #22 | csnode, csdb: pruned storage |
| #25 | csdb: compaction of pruned storage can no longer terminate the node |
| #26 | csdb: LMDB block database for pruned storage |
| #27 | csdb, csnode: prune a whole step in one LMDB transaction |

**Smart contracts**

| PR | Change |
|---|---|
| #28 | solver: unlock only stale contract locks, without reading past the end |
| #29 | api: replace the executor client on any transport error |
| #30 | solver: notice a lost executor connection before dispatching a call |
| #31 | solver: resolve a contract call's inputs under the lock before the worker runs it |
| #32 | solver: accept a contract state from a peer only in answer to a request |
| #33 | csnode: keep smart stage-1 messages that arrive before their consensus starts |

PR #13 (flush the deferred block before applying the next one) was closed without merging. Its
concern was later addressed by #59.
