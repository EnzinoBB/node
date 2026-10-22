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
  - a private 7-node network;
  - a robustness suite: crashes, power cuts, full disks, network faults, mixed binaries, every
    storage backend, conversions;
  - upgrades from the official node, tested on mainnet data;
  - release packages and the Docker image.

## Not released yet

| PR | Change |
|---|---|
| #67 | csdb, ci, docs: conversion follow-ups from the review of #65 (contract-records message, keep a copy of `db/` before converting from berkeleydb, sturdier db-convert scenario) |
| #70 | csnode: bootstrap tables of 9 to 25 confidants are accepted (the official node silently drops them, so a network of 9 or more initial confidants never starts); the same choice as the official node up to 8 |

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
