# The community node compared with the official node

This page explains what changes for someone who runs the community node (`4.2.532-community.N`) in
place of the official 4.2.532 node from CREDITSCOM. It covers behaviour, not code. For the full
list of changes, pull request by pull request, see [CHANGES.md](CHANGES.md).

## What stays the same

- **Network.** The protocol version (532), the block format, the hashes and the consensus rules
  are unchanged. A community node syncs and takes part in consensus alongside official nodes.
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

### Converting the block database

The node converts an existing database to another backend in place.

- `--convert-db` converts, then starts the node.
- `--convert-only` converts, then exits.
- `--convert-batch N` sets how many blocks go in each batch.

The conversion is verified batch by batch. If it is interrupted (by Ctrl-C, a kill or a power
cut), it resumes at the next start. Checkpoints and the transaction index stay valid, so the node
quick-starts after a conversion.

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
  bootstrap round, takes it as before). The tables honest nodes send always pass, so a mixed
  network makes the same choices.
- **State kept across a quick start.** Pending consensus-settings changes, special orders and
  deployed smart contracts survive a quick start. The official node can lose them, or apply
  them differently, after a quick start.

## Running

- **Wallet state digest.** Every 1000 blocks the log has a line
  `STATE DIGEST #<sequence> <hex>`. It is a digest of all wallet state, so nodes, builds and
  backends can be compared: two nodes that agree on the chain must print the same digest at the
  same sequence.
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

## Packages and Docker

- **Release packages.** Each release has full and pruned packages, in two builds:
  - `linux-x64` needs glibc 2.38 (Ubuntu 24.04, Debian 13 and later);
  - `linux-x64-glibc2.31` runs on Ubuntu 20.04, Debian 11 and later.

  Each package includes the JDK of the official release and the community build of the contract
  executor.
- **`config.ini` in the packages.** It is the official one, with three changes:
  - `[event_report]` is commented out (it reports node events to a fixed address);
  - `executor_commit_max` accepts the community executor;
  - the pruned package adds a `[storage]` section.

  The executor listens on 127.0.0.1 only.
- **Known hosts.** `known_hosts.txt` is refreshed weekly from a verified list of seed nodes.
- **Docker image.** The image is `ghcr.io/enzinobb/credits-node`. By default it runs a pruned
  node that starts from a snapshot of the chain, published every 12 hours, instead of syncing
  from block 0. The snapshot's checksum and its state digest are verified before use. See
  [packaging/docker/README.md](../packaging/docker/README.md).
