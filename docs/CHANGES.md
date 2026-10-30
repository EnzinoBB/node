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
- **Snapshots.** A node builds, verifies and restores signed snapshots itself, downloads the one
  published on GitHub on request, and shares snapshots with its p2p neighbours, so that a new node
  starts in minutes from several independent sources. See [snapshots.md](snapshots.md).
- **Windows and macOS.** Packages for Windows (x64) and macOS (Apple Silicon), with a mainnet check
  that the three systems agree on the state.
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
| #119 | ci: the release checks the Windows and macOS packages on mainnet before publishing them (a node of each system restores the published snapshot, syncs to the head, logs the same state digests as Linux and stops cleanly), and the same check runs weekly on the latest published release |
| #120 | ci: a private network of 7 nodes (with the restart of one at block 300) runs on macOS and on Windows every night and on demand (`testnet-platforms.yml`): the nodes reach the same state digests as on Linux and a stopped node quick-starts again; Linux-only scenarios (network faults, slow nodes, full disks) stay on Linux |
| #121 | p2p: the node runs its fork of the p2p library ([EnzinoBB/p2p](https://github.com/EnzinoBB/p2p), from the official commit `8fb3c516`): a registration announcing more than 64 bytes or a later packet header announcing more than 256 MiB closes the connection before any allocation (one error line a minute per address and limit), and packet data is read in 1 MiB chunks so memory follows the bytes received; peers' host data (version, snapshot capability) is refreshed from their answers to our pings, so a node restarted with `serve=on` is found without leaving the routing tables first; a received datagram is passed with its own length; `~Host` stops the routing table (threads joined, UDP socket closed, routing-table notifications waited for) after its io thread and clears the `Network` reference, which `~Transport` no longer does. ci: robustness `net-huge-frame`; `snapshot-p2p-wait` back in the suite; mixed runs build the official node with its own p2p commit |
| #122 | csdb: backward sync, step 1: `Storage::pool_save_backfill` stores a block directly below a stored one on every backend (BerkeleyDB, LMDB, RocksDB), without touching the head; not called yet, no change for a running node |
| #123 | csnode: backward sync, step 2: `BlockChain::storeBackfilledBlock` stores a verified block below the first one (index, block hashes, non-empty block map, storage) without applying it to the caches, and the transaction index links it under each wallet's lowest indexed block, so that histories grow downwards while the node runs; start-up repairs after a crash, only on a node that backfilled; not called yet, no change for a running node |
| #124 | lmdbxx: two races in the cache databases (transaction index, block hashes) between the processor thread and API threads: integer keys and values are converted to text in a buffer of each call instead of one shared buffer (a lookup could name another block, a write store another number; Linux and Windows), and the map grows only while no transaction is open (a reader could be left on the old mapping and crash) |
| #125 | csnode: backward sync, step 3: a full node whose storage starts above block 0 (from a snapshot, or switched from pruned to full) fetches the older blocks from its neighbours with BlockRequest while it is synced, checks each against the hash chain and its signatures, and stores them downwards without applying them; usable by the API at once. **By default a full node started from a snapshot fetches the whole history to disk** (mainnet: about two days at 1000 blocks/s, about 400 GB with lmdb/berkeleydb, 130 GB with rocksdb); `[storage] backfill=off` avoids it. Up to 8 × `backfill_batch` blocks wait in memory; at most `backfill_store_per_turn` (50) blocks are stored per turn of the consensus thread; paced per peer under special order 35; once block 1 is stored the node enforces the chain identifier (uuid) on its neighbours; a pruned node fetches only down to the bottom of its pruned window. Keys `backfill`, `backfill_batch`, `backfill_rate`, `backfill_store_per_turn` (docs/storage.md) |
| #126 | api: after a quick start at the last stored block, contract calls through the API returned "Transaction in progress" and never their result; the node now marks its stored blocks as read when it runs |
| #127 | ci: token tests: `ci/contracts/executor_harness.py` compares contract executors on token contracts without a node, and `TOKENS=1` runs `ci/testnet/tokens.py` (v1 and v0 tokens, balances and states on every node, the token API; `TOKENS_VIA` through another node) on the private network |
| #128 | api: the token cache (`[api] tokens_cache=true`, any build) follows v1/v2 tokens (BigDecimal amounts, standards 3 and 4) and only successful calls. It reads balances from the contract for the wallets a call involves, refreshes them in the background at start, and is rebuilt from the chain when the checkpoint holds none. `approve()` without arguments no longer reads out of bounds. `TokensListGet` filters before paging, and the token lists sort without exceptions or invalid orderings. `TokenTransfersListGet` reports the signer as the sender. See [tokens.md](tokens.md) |
| #129 | ci: backward sync scenarios: a full node joining from a pruned snapshot fetches blocks 0..699 and must hold every block and the wallet histories of n1 (also midway: the newest part of them), across SIGKILLs (one right after a flush, one with a block stored but not indexed), a power cut of the whole network, two peers serving altered blocks, special order 35, and official nodes serving the history (`backfill-*` rows of `robustness.yml`); test-build hooks `testnet-tamper-replies`, `testnet-backfill-crash-unindexed` |
| #130 | packaging: in Docker a full node on lmdb or rocksdb starts from the snapshot (download, then p2p) and fetches the older blocks in the background; a full berkeleydb node still syncs from block 0; switching from pruned to full is allowed; `CREDITS_BACKFILL=off`; docs: "A full node from a snapshot" without Docker (docs/storage.md), Docker README, package README, release notes |
| #131 | packaging: a guided set-up in every package (`setup.sh`, `setup.bat`): pruned or full, database, how the first start gets the chain, snapshot sharing, and the contract executor (community or official: it shows the build in the folder, can download either, checked, and sets `executor_commit_min/max` so that the node starts it; an upgrade by copying only the node binary keeps the old executor) |
| #132 | api: `TransactionFlow` refuses a contract call that no method of the contract matches (unknown method, wrong argument count, or a Variant that cannot match the declared type, such as a token amount as `v_string` for a `BigDecimal`), which would fail on every node with its fee paid; ci: `testnet.yml` inputs `tokens`/`tokens_via`, the executor harness compares only the answers |
| #133 | ci: backward sync on BerkeleyDB: a pruned BerkeleyDB node switched to full fetches the blocks it pruned into its deleted Recno records (`backfill-berkeleydb`, `STORAGE_SWITCH_NODE`); unit test on `DatabaseBerkeleyDB` |
| #134 | api: token API leftovers: a failed transfer no longer takes a place in the pages of `TokenTransfersGet`/`TokenWalletTransfersGet`; `TokenTransferGet` answers only for a transfer of that token (it listed any transaction and added entries to the operations map); token counters above 2^31 report the largest i32 instead of wrapping; the token cache is saved, loaded and hashed under its lock (the background refresh could change it during a checkpoint) |
| #135 | samples: a token template (`samples/contracts/Token.java`: checked amounts and decimals, balances at the token's scale, ERC-20 `approve()`, sale through `payable()`), run by the executor harness, and a Python client that calls a contract with the argument types it declares (`samples/python/token_client.py`) |
| #138 | ci: macOS jobs run on `macos-15` (GitHub retires the macOS 14 images on 2026-11-02 and fails them during brownouts until then), with binaries built for macOS 14 and later, and time out after their measured duration plus a margin (build 60, platform sync 40, private network 60 minutes instead of 240, 90, 150) |

## community.14 — 2026-10-27

| PR | Change |
|---|---|
| #113 | packaging: one package per build for full and pruned storage (`config.ini` full, `config.pruned.ini` pruned on LMDB), half the release size; the workflows that take the latest package accept both layouts |
| #114 | the node builds and passes its tests on Windows (MSVC, x64) and macOS (Apple Silicon): POSIX-only code behind `lib/system/platform.hpp`, the database lock now also on Windows, a new `build-platforms.yml` workflow; the release workflow also makes a Windows `.zip` and a macOS package (with Eclipse Temurin 11), kept as artifacts until a mainnet state-digest check on each system |
| #115 | Windows and macOS nodes load the checkpoints and snapshots of Linux and sync mainnet with the same state digests: the binary checkpoint header records `long` as 8 bytes on every system (`cs::ArchiveOutput`, read back by `cs::ArchiveInput`), checkpoint text files are read and written in binary mode, macOS Boost is built as C++17; `Transport` releases the routing table with the p2p host, fixing a use-after-free at exit (a crash on macOS); a platform consensus check in CI |
| #116 | snapshot: `node --survey-snapshots [seconds]` asks the neighbours for the snapshots they serve without opening the chain and prints what they declared; the weekly known-hosts job turns it into `snapshots.md` and `snapshot_sources.txt` (shipped in the packages when present) |
| #117 | client: `--download-snapshot`, `--import-snapshot` and `--export-snapshot` start the logger with the configured levels; they printed every debug and trace record of the import to the console (about 684,000 lines for one `--download-snapshot`, the Docker first start included) |
| — | release: the version announced to peers (`MINOR_NODE_VERSION`) is 14; the packages and the Docker image carry a `VERSION` file with the release's version, which the node puts in the snapshots it builds |

Windows and macOS (#113–#115). The release has a package for Windows (x64, a `.zip`) and for macOS
(Apple Silicon) next to the Linux ones, each with its own JDK (Eclipse Temurin 11); the Linux
package is one for full and pruned storage now (`config.ini`, `config.pruned.ini`), half the size
of the two it replaces. A Windows or macOS node loads the checkpoints and snapshots a Linux node
wrote and the other way round, and syncs mainnet with the same state digests: three nodes, one on
each system, restored the published snapshot and logged identical digests for 91 blocks, at 26,000
to 51,000 blocks per minute from the snapshot to the head. On Linux the checkpoint and snapshot
files are byte for byte what they were. A use-after-free at exit (the routing table outliving the
p2p host) is fixed; it crashed the macOS node at every stop and was undefined behaviour on the
other systems.

Snapshots (#116, #117). `node --survey-snapshots` shows which neighbours serve which snapshot,
and a weekly job publishes it with a list of serving nodes. `--download-snapshot` and the other
snapshot commands no longer print every debug record of the import to the console.

## community.13 — 2026-10-26

| PR | Change |
|---|---|
| #99 | config: the `[snapshot]` section takes the keys of p2p snapshot sharing (`serve`, `serve_rate_kbps`, `serve_max_peers`, `snapshot_blocks` from 1, `fetch`, `fetch_wait`, `fetch_fallback`, `min_sources` 2, `sources` as node keys, `sources_file`, `fetch_rate_kbps`); the HTTP service keys are gone and an unknown key is logged and ignored; nothing reads them yet |
| #100 | net: four p2p messages for snapshot manifests and pieces (types 40-43) with bounded decoders; this build answers requests as not serving and sends none |
| #101 | net: `host_data` announces snapshot serving (bit 24, `[snapshot] serve=on` at start) and the community version (bits 32-47, `MINOR_NODE_VERSION`, now 13); `known_peers.json` at a clean stop lists the known peers with what they announce |
| #102 | snapshot: a node with `[snapshot] serve=on` builds the snapshot of each periodic checkpoint and serves it to its p2p neighbours, within `serve_rate_kbps` (KB/s), `serve_max_peers` and per-peer limits, at half budget while trusted; snapshots use 512 KiB chunks and no contracts file |
| #103 | ci: the official 4.2.532 node takes part in the test networks (`node_alt_ref=official`, `scale.yml` `official=1` or tag suffix `-o`, and the second nightly `testnet.yml` run), built with only the two hooks a test network needs (`ci/official/testnet-hooks.patch`); official nodes are checked by block hash, and scenarios needing a feature only this fork has are refused or skipped. First runs: mixed consensus works (13 nodes, official and community confidants); an official node that syncs a balance-reading contract ends on another state, and one killed while saving a checkpoint does not restart |
| #104 | snapshot: the p2p fetcher (not started yet): finds the neighbours that serve snapshots, agrees on one (a configured `sources=` key first, then the most independent sources by /24 or /48, the highest block only as a tie-break), downloads its pieces from several sources with every piece and file checked against the signed manifest, an adaptive piece timeout for slow links, limits per network prefix against made-up keys, and the fallback to syncing from block 0 after `fetch_wait` (or a bounded total time) |
| #105 | snapshot: a node that starts on an empty LMDB or RocksDB database with `[snapshot] fetch=auto` fetches a snapshot from its p2p neighbours before opening the chain (bootstrap mode: only the snapshot replies are processed), imports it and quick-starts from it, or syncs from block 0 after `fetch_wait`; the restored state is checked against `snapshot.json` at start (`snapshot.verified`, or `snapshot.mismatch` and exit code 4); an interrupted import is cleaned up at the next start; `snapshot.sources.json` lists the sources; CI networks set `fetch=off`, and two robustness rows join over p2p |
| #106 | ci, docs: p2p snapshot failure and attack scenarios (a source that starts serving late, sources trusted by key or listed in a file, the joiner killed during its fetch and its import, a forged state digest and corrupt pieces from test-build hooks, a join while contracts run); docs/snapshots.md for operators |
| #107 | client: `--download-snapshot [url]` downloads a published format-2 snapshot with `curl` (no shell, http(s) only, resumable), checks every file against the signed manifest and restores it; scenarios `snapshot-download` and `snapshot-download-corrupt` |
| #108 | ci(snapshot): the 12-hour snapshot workflow also publishes format 2 (the node's `--export-snapshot` of its latest periodic checkpoint: `snapshot.json` and its files as `s<seq>-<name>`, the 3 newest sequences), checked file by file before upload; format 1 stays for one release; `publish=false` for a test run |
| #109 | docker: `CREDITS_SNAPSHOT=github|p2p|off` (default github; the old `auto` still works): a pruned node's first start runs `node --download-snapshot` explicitly, then starts whatever the result (the node then tries its neighbours, then block 0); `[snapshot] serve=on` by default (`CREDITS_SERVE_SNAPSHOT`), `CREDITS_SNAPSHOT_MIN_SOURCES`; the restore script and the digest wait of the entrypoint are gone (the node verifies the snapshot itself); an old `latest.json` address points to the format-2 `snapshot.json` next to it |
| #110 | ci: stress scenarios for the confidants that serve snapshots (four nodes joining at once, a kill during the fetch and the import, a 2 Mbit/s link or 5 % loss on a serving confidant and a joiner, contracts running): the block rate and the round times while serving are compared with the same window before the joins (`STRESS_WINDOW`, at most 10 % / 20 % worse); `JOIN_EXTRA`, `NET_FAULT=rate` |

Snapshots shared between nodes (#99–#110): a community node with `serve=on` builds a snapshot of
each periodic checkpoint and serves it to its p2p neighbours; a new node with an empty LMDB or
RocksDB database fetches one from several independent neighbours that agree on it, checks every
piece against the signed manifest, imports it and joins, and falls back to syncing from block 0
without agreement (`fetch_wait`, 10 minutes). `--download-snapshot` restores the snapshot published
on GitHub, now also in format 2 (#107, #108); the Docker image calls it on a pruned node's first
start and serves snapshots by default (#109). Serving does not slow the consensus down measurably:
the stress runs (#110) kept the block rate within 3–9 % and the round time within 3 % with four
nodes joining at once, a killed join, packet loss and a slow confidant. New p2p messages are
ignored by official nodes; the CI now runs official 4.2.532 nodes in its test networks (#103).
Until community nodes serving snapshots exist on mainnet, a new LMDB or RocksDB node started with
the default `fetch=auto` waits `fetch_wait` before syncing from block 0; the packaged and Docker
first starts download the GitHub snapshot instead.

## community.12 — 2026-10-24

| PR | Change |
|---|---|
| #87 | csnode: every checkpoint saves the digest of the state it holds (wallets, their count, consensus settings) as `state_digest.bin` and logs it; a quick start recomputes it and moves aside a checkpoint whose caches do not match |
| #88 | client: `[snapshot]` section of `config.ini` for snapshots served by community nodes (no behaviour yet) |
| #90 | csnode: `BlockVerifier` for blocks that come from outside the consensus (hash, signatures against the block's confidants, round confirmations), for the snapshot import and backward sync |
| #89 | csnode, client: snapshot format 2 built by the node from its own checkpoint and blocks on any backend (lz4, per-chunk SHA-256, manifest signed with the node key); `--export-snapshot <dir>` |
| #92 | csnode, client: snapshot importer and `--import-snapshot`: a format-2 snapshot is checked file by file and block by block (signatures, round confirmations, hash chain to the declared head, checkpoint digest) and written into a new LMDB or RocksDB database; the CI snapshot test uses export and import; digest_offline waits for the node to stop |
| #93 | ci(testnet): a Dice contract that reads its own balance, played by two players (also twice in one block); scenario `contracts-dice-join`, where a node syncing through the plays must end with the live nodes' balances and contract state |
| #94 | solver: an emitted transaction's fee is charged to the contract's latest successful execution in its block, as the live nodes charge it; a syncing node whose re-execution of a contract failed charged it to an earlier caller. Consensus-relevant: balances of such callers change by a fee compared with an official node that syncs the same blocks |
| #95 | solver: each contract records the call and hash of the state the chain declares; once live, a node asks the confidants for the states it holds stale (paced, with backoff) and takes only the declared one; a state whose block is not stored is no longer taken unverified |
| #96 | solver, client: `[params] sync_contract_execution` (default true): false skips the re-execution of contract calls while syncing, every state is fetched from the confidants; set explicitly in the packaged `config.ini` |

Contract states while syncing (#93–#96): a syncing node no longer disagrees with the network on the
balances of callers of contracts whose re-execution is not reproducible (such as dice games), and
gets the states of such contracts from the confidants, verified against the hash the chain declares.
#94 is consensus-relevant: it charges contract payouts as the nodes that executed the contract do.
Snapshots (#87–#92): checkpoints carry the digest of their state, and the node builds and restores
signed snapshots itself (`--export-snapshot`, `--import-snapshot`) on any backend; serving them
between nodes comes next.

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
