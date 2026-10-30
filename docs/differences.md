# The community node compared with the official node

This page explains what changes for someone who runs the community node (`4.2.532-community.N`) in
place of the official 4.2.532 node from CREDITSCOM. It covers behaviour, not code. For the full
list of changes, pull request by pull request, see [CHANGES.md](CHANGES.md).

## What stays the same

- **Network.** The protocol version (532), the block format and the hashes are unchanged, and so
  are the consensus rules, except for the cases listed under "Consensus fixes" below, which only
  change what a node does when a round ends in conflict. A community node syncs and takes part in
  consensus alongside official nodes.
- **Configuration.** The official `config.ini` works unchanged. Without a `[storage]` section the
  node is a full node on BerkeleyDB, like the official one. It opens the same `db/` folder and
  converts nothing.
- **Upgrading.** You can upgrade a folder of the official node by replacing the binaries, and go
  back to the official binaries later. Both directions are tested in CI on mainnet data
  (`upgrade-compat`). Going back works only on a full BerkeleyDB folder: the official node reads
  only BerkeleyDB, and needs every block.
- **Keys, ports and the Thrift API.** These are unchanged. The only API change: when blocks are
  pruned, `TransactionsListGet` skips them (see "Pruned storage" below).

## New options

### Block database and pruned storage

There are new keys in the `[storage]` section of `config.ini`. See [storage.md](storage.md) for
each key and its default.

| Key | What it does |
|---|---|
| `db_backend` | Chooses the block database: `berkeleydb` (the default, the official format), `lmdb` or `rocksdb`. RocksDB is about a third of the BerkeleyDB size on mainnet. |
| `prune_keep_blocks` | Pruned storage: the node keeps only the latest blocks and removes older ones at each periodic checkpoint. Pruned storage runs on LMDB. |
| `async_write_queue_size`, `write_batch_size`, `rocksdb_block_cache_mb`, `rocksdb_memtable_mb` | Tune the database writer and RocksDB. |
| `backfill`, `backfill_batch`, `backfill_rate`, `backfill_store_per_turn` | Backward sync: a full node whose oldest block is above block 0 (started from a snapshot on lmdb or rocksdb, or switched from pruned to full) fetches the older blocks from its neighbours in the background, with the existing block requests, checks each against the chain's hashes and its signatures and stores it without applying it; the API serves each block and history as it arrives. On by default; `backfill=off` keeps only the snapshot's blocks. |

### Converting the block database

The node converts an existing database to another backend in place.

- `--convert-db` converts, then starts the node.
- `--convert-only` converts, then exits.
- `--convert-batch N` sets how many blocks go in each batch.

The conversion is verified batch by batch. If it is interrupted (by Ctrl-C, a kill or a power
cut), it resumes at the next start. Checkpoints and the transaction index stay valid, so the node
quick-starts after a conversion.

### Snapshots built and restored by the node

What an operator sees and sets is in [snapshots.md](snapshots.md).

- `--export-snapshot <dir>`, on a stopped node of any backend, pruned or full, builds a snapshot
  of its latest periodic checkpoint: the checkpoint files, the blocks before it as a hash chain
  ending at the checkpoint's block, compressed, with a SHA-256 for every file and every 512 KiB
  piece, in a `snapshot.json` signed with the node's key (the checkpoint carries the contract
  states at its block).
- `--download-snapshot [url]` downloads the snapshot published on GitHub (or the one at `url`) with
  `curl`, checks it and restores it as `--import-snapshot` does; run by hand or by the Docker
  entrypoint, never by a normal start.
- `--import-snapshot <snapshot.json>` restores such a snapshot into an empty database of the
  configured backend (LMDB or RocksDB). Every file is checked against the manifest and every
  block before it is written: its signatures against its confidants, the confirmations of the
  round before, and the hash chain to the declared block; the checkpoint must declare the
  manifest's state digest. Anything wrong leaves nothing behind. The node then quick-starts from
  that checkpoint.
- The `[snapshot]` section of `config.ini` holds the settings of p2p snapshot sharing
  (snapshots served by community nodes to their neighbours and fetched from them at a first
  start: `serve`, `serve_rate_kbps`, `serve_max_peers`, `snapshot_blocks`, `fetch`,
  `fetch_wait`, `fetch_fallback`, `min_sources`, `sources`, `sources_file`, `fetch_rate_kbps`).
  A key that is not one of these is logged and ignored.
- **Serving snapshots to the neighbours.** With `[snapshot] serve=on` the node builds the snapshot
  of each periodic checkpoint into `snapshot/<block>/` at low priority and answers the snapshot
  requests of its p2p neighbours while it is synced with the network, within `serve_rate_kbps`
  (KB/s, all peers together; half for one peer), `serve_max_peers` peers within 30 s, and half of
  that while it is a confidant or the writer. It announces it in its p2p host data at start, with
  its community version. Official nodes neither see nor answer these messages.
- **A first start fetches a snapshot from the network.** A node whose `db/` holds no database,
  with no checkpoint in `qs/`, `db_backend` `lmdb` or `rocksdb` and `fetch=auto` (the default),
  starts its p2p transport without opening the chain and looks for neighbours that serve
  snapshots (`SNAPSHOT: empty database: looking for snapshots on the network`). Once
  `min_sources` sources in different /24 (or /48) networks, or keys listed in `sources`, agree
  on a snapshot, it downloads it, imports it and quick-starts from it; meanwhile it processes no
  other message. Without agreement within `fetch_wait` seconds it syncs from block 0
  (`fetch_fallback=sync`) or keeps waiting (`wait`); the log line names `--download-snapshot` as
  the faster way. A BerkeleyDB node never fetches. The sources used are listed in
  `snapshot.sources.json`.
- **Private and genesis networks set `fetch=off`.** Every node of a network that starts from
  block 0 starts on an empty database: with `fetch=auto`, LMDB and RocksDB nodes would wait
  `fetch_wait` for snapshots nobody has.
- **The restored state is checked at start.** When `snapshot.json` is present (written by
  `--import-snapshot` or by the fetch) and `snapshot.verified` is not, the node compares the
  digest of the state its quick start loaded with the manifest's. It writes `snapshot.verified`,
  or `snapshot.mismatch` and stops with exit code 4; it refuses to start while
  `snapshot.mismatch` exists. An import interrupted by a kill leaves `db/.importing`; the next
  start removes what that import wrote before it looks at `db/`.

## Start-up behaviour

- **Database check.** Before opening the block database, the node looks at `db/`. It writes the
  backend's name in `db/.backend`, and holds `db/.lock` so that two node processes cannot use the
  same folder. It stops with exit code 3, changing nothing, in these cases:
  - `db/` holds a database of another backend than `config.ini` asks for, and no conversion was
    requested;
  - `db/` holds two databases with blocks;
  - another process holds the folder.

  The message says what to do. From a terminal, the node asks whether to convert instead.
- **Checkpoints.** The checkpoints used for a quick start (`qs/`) are checked against the stored
  chain before they load. A checkpoint that does not match the blocks on disk is not loaded. If
  it is ahead of them, which can happen after a crash between saving a checkpoint and writing its
  block, it is moved aside to `qs/<version>.bad` and the next start uses an older checkpoint,
  instead of failing the same way every time.
- **No slow start for a lost index.** The official node falls back to a slow start, replaying
  every block from 0, whenever the caches are inconsistent. The community node repairs the
  usual causes instead:
  - it rebuilds the transaction index (`caches/indexdb`) from the stored blocks after a quick
    start;
  - it rebuilds the block hash databases (`blockHashes: rebuilding from the stored blocks ...`)
    when they fail their check.
- **Bootstrap with more than 8 confidants.** The official node gives a bootstrap round table a
  weight that fits in 8 bits and silently drops tables of 9 or more confidants, so a network whose
  initial confidants (or the bootstrap nodes set by special order 2) are 9 or more never starts or
  restarts. The community node accepts tables of up to 25 confidants (`MaxTrustedNodes`). Up to 8 it
  gives exactly the official weights, so both pick the same table when a stalled network starts
  again; a dropped table is now logged.
- **Bootstrap tables are checked against the node's own state.** A bootstrap round table is
  taken only for a round next to the node's own (from its last block to its round + 2), and,
  unless it is signed with the blockchain key, only once the node's own round has lasted at
  least half of `round_elapse_time` (a node younger than `round_elapse_time`, or already in a
  bootstrap round, takes it as before). The tables honest nodes send pass in the cases a mixed
  network relies on, so it makes the same choices; a node that joined the stalled round late, or
  that is several blocks behind and has not learned the round yet, takes the next table instead.
  A bootstrap through the diag API must name a round within two of what the nodes know (their
  last block + 1, as the sending node uses when it raises the round).
- **State kept across a quick start.** Pending consensus-settings changes, special orders and
  deployed smart contracts survive a quick start. The official node can lose them, or apply
  them differently, after a quick start.

## Running

- **Wallet state digest.** Every 1000 blocks the log has a line
  `STATE DIGEST #<sequence> <hex>`. It is a digest of all wallet state, so nodes, builds and
  backends can be compared: two nodes that agree on the chain must print the same digest at the
  same sequence.
  Every checkpoint saves the digest of the state it holds, with a hash of the consensus settings,
  as `qs/<sequence>/state_digest.bin`, and logs it (`STATE DIGEST #<sequence> ... checkpoint`). A
  quick start recomputes it from the loaded caches, logs `... checkpoint loaded`, and moves a
  checkpoint whose caches do not match it aside to `qs/<sequence>.bad`.
- **Stopping on errors.** Some errors that the official node can survive in an inconsistent
  state now stop the node, and are logged with `cserror`:
  - a block that cannot be stored;
  - a block that fails to apply to the caches;
  - a BerkeleyDB environment that needs recovery.

  The caches are not saved in these cases, so the next start rebuilds them from the database.
  Run the node under a supervisor (systemd, Docker `restart:`) so that it restarts.
- **Full disk.** On RocksDB a full disk no longer blocks the writer: the node keeps the blocks in
  memory and retries.
- **Pruned storage.** A pruned node answers NoSuchBlocks for blocks it has pruned, so a peer
  asks another node for them. `TransactionsListGet` skips pruned blocks.
- **Contract states while syncing.** Blocks carry only the hash of a contract's new state. The
  official node, when it syncs, re-executes the calls to rebuild the states; for some contracts
  this is not reproducible (a game that reads its own balance sees it after the block's payout),
  the state stays out of date, and the fee of a later payout was charged to an earlier caller, so
  official nodes disagree among themselves on such callers' balances. The community node:
  - charges a payout's fee as the nodes that executed the contract do (see "Consensus fixes");
  - records, for every contract, the call and the hash of the state the chain declares, and once
    it is live asks the confidants for every state it holds out of date (at most 16 a round,
    then less and less often), taking only the declared one; the log has a line per state received
    and a summary (`contract states: N known, S stale, …`) when the node becomes live and every
    1000 rounds while some remain;
  - `[params] sync_contract_execution` (default `true`): `false` skips the re-execution while
    syncing altogether, and every state comes from the confidants.

## Security and correctness fixes

These were found by reviewing the official code. None of them changes the blocks or the
consensus rules.

- **Remote crashes and out-of-bounds reads.** Fixed when a peer sends a malformed compressed
  region, an empty packet, or a pool whose lengths point past its data. Also fixed: a data race
  in `LockFreeChanger`.
- **Smart-contract solver.** Fixed:
  - stale contract locks;
  - executor connections lost between calls;
  - a race on a contract call's inputs;
  - contract states accepted from peers without a request;
  - stage-1 messages that arrive early.
- **Consensus state after a restart.** Fixed:
  - the mining and staking flags swapped on restore;
  - the rewards of an uncertain block kept when the trusted mask differs;
  - the revert of a timed delegation;
  - uninitialised consensus settings.

## Consensus fixes

- **Two blocks of one round.** When a trusted node does not get one stage-2 in time, it fetches it
  with a request that takes exactly as long as the others wait for its stage-3. Depending on a few
  milliseconds, some trusted nodes finish the round with every signature while the others retry
  stage 3 without the late node, and the writer publishes the retried block. The two blocks differ
  only in the set of real trusted. The official node keeps its own block for good, then no longer
  matches the following rounds (seen in CI as a network stuck in bootstrap). The community node
  logs this as a fork and, if it is a trusted node that still holds the block undeferred, takes
  the writer's block when:
  - a majority of all the trusted of the round signed it, checked against the node's own list of
    the round's trusted, so a minority of dishonest signers can never make it switch;
  - the nodes outside that set are too few to form a round on their own block.

  Fees and the block reward move to the new set of signers.
- **The official "more signatures" rule works.** The official node meant to replace its block by
  one with more signatures, but the check before the replacement compared the block with itself and
  always failed. It now works. Residual risk, as in the official design: a package with more
  signatures that reaches only part of the network would move only that part.

- **Fees of contract payouts.** A transaction a contract emits is charged to the contract's
  latest successful execution in the same block, which is what a node that executed the contract
  does (it applies every new state of the block before the payouts). A syncing official node whose
  re-execution failed charges it to an earlier caller instead. This changes, by one fee, the
  balances such a node computes for those callers, and makes them those of the nodes that took
  part in the rounds.

Official nodes on the side that keeps its block behave as before; mixed networks do not fork (CI:
equal state digests with official binaries on half of the nodes, and the same share of signed
rounds for both).

## Packages and Docker

- **Release packages.** Each release has one package for full and pruned storage (until
  community.13, a full and a pruned package), in these builds:
  - `linux-x64` needs glibc 2.38 (Ubuntu 24.04, Debian 13 and later);
  - `linux-x64-glibc2.31` runs on Ubuntu 20.04, Debian 11 and later;
  - `windows-x64` (a `.zip`, Windows 10 and later; start `node.exe` from a terminal in the `csnode`
    folder) and `macos-arm64` (Apple Silicon; clear the download quarantine once with
    `xattr -dr com.apple.quarantine csnode`; the node is not signed).

  The Linux packages include the JDK of the official release; the Windows and macOS ones include
  Eclipse Temurin 11 (the official release has no JDK for them), in `jdk/`, and their
  `executor_command` points at it. Each package includes the community build of the contract
  executor.
- **Checkpoints and snapshots are the same on every system.** A Windows or macOS node loads what a
  Linux node wrote and the other way round: the binary checkpoint files record `long` as 8 bytes
  whatever the system, and the checkpoint's text files are read and written in binary mode. On
  Linux the files are byte for byte what they were.
- **`config.ini` in the packages.** It is the official one, with five changes:
  - `[event_report]` is commented out (it reports node events to a fixed address);
  - `executor_commit_max` accepts the community executor;
  - `compatible_version=true`, the default of the node binary (the official file sets `false`):
    a synced block that fails the node's own transaction check is logged
    (`skip block validation`) and stored, instead of stopping the node at it for good. See
    "Contract states while syncing";
  - `sync_contract_execution=true` (the default), with a comment;
  - next to it, `config.pruned.ini` is the same file with a `[storage]` section for pruned storage
    on LMDB: `./node --config-file config.pruned.ini`.

  The executor listens on 127.0.0.1 only.
- **Known hosts.** `known_hosts.txt` is refreshed weekly from a verified list of seed nodes.
- **Docker image.** The image is `ghcr.io/enzinobb/credits-node`. By default it runs a pruned
  node that starts from a snapshot of the chain, published every 12 hours, instead of syncing
  from block 0. The snapshot's checksum and its state digest are verified before use. A full node
  on lmdb or rocksdb starts from the snapshot too and fetches the older blocks in the background
  (`CREDITS_BACKFILL=off` to keep only the snapshot's blocks); switching from pruned to full is
  allowed. See [packaging/docker/README.md](../packaging/docker/README.md).
