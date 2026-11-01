# Block storage: configuration and conversion

What the node supports for its block database, and how to move a database from one backend to
another. Everything here is set in the `[storage]` section of `config.ini` or on the command line.
For Docker, see also `packaging/docker/README.md`.

## Where the data lives

| Path (under `--db-path`, default `db/`) | What | Depends on `db_backend` |
|---|---|---|
| `blockchain.db`, `sequence.db`, `contracts.db` | blocks and contract data, berkeleydb | yes |
| `lmdb/` | blocks and contract data, lmdb | yes |
| `CURRENT`, `MANIFEST-*`, `*.sst`, ... | blocks and contract data, rocksdb | yes |
| `.backend` | one line naming the backend of the database in this folder | written by the node |
| `.convert` | progress of a conversion, present only while one is under way | written by the node |
| `.lock` | held by the running node | written by the node |

Checkpoints (`qs/`) and the transaction index (`caches/indexdb`) are always LMDB, whatever
`db_backend` says: they stay valid across a conversion, so the node quick-starts after it.

## `[storage]` settings

Without a `[storage]` section every setting keeps its default: a full node on berkeleydb, as the
official node.

| Key | Default | Meaning |
|---|---|---|
| `db_backend` | `berkeleydb` | `berkeleydb`, `lmdb` or `rocksdb` (`rocks` is read as `rocksdb`). An unknown value logs a warning and uses `berkeleydb`. |
| `block_db` | | Older name: `block_db=lmdb` means `db_backend=lmdb`. Ignored when `db_backend` is set. |
| `prune_keep_blocks` | `0` | `0` keeps every block (full node). Above `0`, pruned storage: blocks older than the previous periodic checkpoint minus this many are removed. Raised to `200` if lower; needs `checkpoint_every` > 0 (otherwise pruning is off), and raises `checkpoint_keep` to at least `2`. |
| `checkpoint_every` | `500000` | Blocks between periodic checkpoints in `qs/`. `0` turns them off; values from 1 to 999 are raised to `1000`. |
| `checkpoint_keep` | `5` | Periodic checkpoints kept (`qs/0` is always kept on top). |
| `checkpoint_every_minutes` | `0` | Also save a checkpoint when this many minutes have passed since the last one (`0` = off). |
| `async_write_queue_size` | `5000` | Blocks waiting for the database writer. |
| `write_batch_size` | `100` | Blocks written to the database in one write. |
| `rocksdb_block_cache_mb` | `1024` | RocksDB block cache in MiB (`0` = RocksDB's default). rocksdb only. |
| `rocksdb_memtable_mb` | `256` | RocksDB write buffer in MiB (`0` = RocksDB's default). rocksdb only. |
| `backfill` | `on` | Backward sync: a node whose oldest stored block is above its floor fetches the older blocks from its neighbours (see [Backward sync](#backward-sync-backfill)). `off` never does. |
| `backfill_batch` | `100` | Blocks per request, `1` to `1000`. |
| `backfill_rate` | `1000` | Blocks requested per second over all peers (`0` = no limit). |
| `backfill_store_per_turn` | `50` | Blocks checked and stored per turn of the thread that also runs consensus, `1` to `1000`; the rest waits for the next turn, 100 ms later. A turn over 200 ms is logged (`BACKFILL: slow turn of ...`) with the time spent verifying, storing and checking confirmations. |
| `backfill_turn_budget_ms` | `50` | While the node is a confidant, a turn also ends once it has taken this long (after at least one block), `0` (no limit) to `1000`: consensus keeps most of the thread; on mainnet about 250 blocks/s. |
| `backfill_idle_turn_budget_ms` | `90` | The same while the node is not a confidant, with little consensus work: on mainnet about 450 blocks/s. |

The periodic flush of the backfilled blocks (every 10,000 blocks or a minute; up to several seconds on mainnet) runs on a thread of its own, not on the thread that runs consensus; one flush at a time, and while the node is a confidant it waits until the node is out of the trusted set (10 minutes at most). The log line says `on the flush thread`.

### Choosing a backend

| Backend | Use it for | Notes |
|---|---|---|
| `berkeleydb` | A folder the official node must also be able to open | The official node's format. Its block table does not reuse the space of pruned blocks, so it is a poor fit for pruned storage. A pruned database cannot be converted to it. |
| `lmdb` | Pruned nodes (`config.pruned.ini` of the package, and the Docker default) | Reuses the space of removed blocks; `data.mdb` never shrinks. |
| `rocksdb` | The smallest full node | Compressed: about a third of the berkeleydb size on mainnet (9.6 GB to 3.1 GB on the first 4.4 M blocks). |

The official node reads berkeleydb only. On a folder converted to another backend it would not
find its files and would sync from block 0: convert back to berkeleydb before running it there.

### Full and pruned

Going from full to pruned keeps the database: set `prune_keep_blocks` and the old blocks are
removed at the next checkpoints. Going from pruned to full keeps it too: set `prune_keep_blocks=0`
and the node fetches the removed blocks from its neighbours in the background (backward sync,
below), in Docker as well.

The package's `config.pruned.ini` has this section:

```ini
[storage]
checkpoint_every=100000
checkpoint_keep=2
prune_keep_blocks=1000
block_db=lmdb
```

### Backward sync (backfill)

A full node whose oldest stored block is above block 0 (it started from a snapshot, or was switched
from pruned to full) fetches the blocks below it from its neighbours, with the same block requests
as a forward sync, newest first, down to block 0. Each block must hash to the previous hash of the
block above it and carry valid signatures of its own confidants; it is stored, never applied to
the wallets (the state comes from the snapshot), and the API serves it at once: block queries,
transaction lookups and wallet histories grow downwards while the node runs, with nothing to
restart.

- **By default a full node started from a snapshot fetches the whole history to disk.** On mainnet
  that is about 186 million blocks: about two days at the default `backfill_rate=1000` (longer if
  the peers pace the requests), and the disk of a full node, about 400 GB with lmdb or berkeleydb,
  about 130 GB with rocksdb. Set `backfill=off` to keep only the blocks of the snapshot.
- Requests go out only while the node is in sync (not syncing forward, not in a bootstrap round,
  not fetching a snapshot); a trusted node keeps fetching, at most `backfill_store_per_turn` blocks
  per turn of the thread that also runs consensus.
- Up to 4 requests are in flight and up to 8 × `backfill_batch` blocks wait in memory to be stored
  in order. A peer that sends a block that does not verify, or does not answer, is left out for an
  hour.
- A pruned node fetches only down to the bottom of the window it keeps (`checkpoint_every +
  prune_keep_blocks` below its head at start), which a snapshot usually holds already.
- Once block 1 is stored, the node knows the chain's identifier and, as a node synced from block
  0, no longer accepts neighbours that announce another chain.
- The log shows `BACKFILL: full node with blocks N..M: fetching 0..N-1`, a line every 100,000
  blocks, every peer left out, and at the end `BACKFILL: full history stored`; the status line
  gets `, oldest stored = K`. A restart resumes below the oldest stored block.

### A full node from a snapshot

Without Docker, in the folder of the full package (`config.ini`):

1. in `config.ini`, `[storage]` with `db_backend=lmdb` or `db_backend=rocksdb` and no
   `prune_keep_blocks` (a snapshot cannot start a berkeleydb database: it starts above block 0);
2. `./node --download-snapshot` (node stopped, empty `db/`): it downloads the published snapshot,
   checks every file against its signed manifest and restores it into the configured backend (see
   [snapshots.md](snapshots.md));
3. start the node. It checks the snapshot's state digest at its first start (and stops if it
   differs), syncs the last blocks forward, takes part in the network and logs
   `BACKFILL: full node with blocks N..M: fetching 0..N-1 from peers in the background`.

With an empty `db/` and `[snapshot] fetch=auto` (the default), the node also takes a snapshot from
its p2p neighbours by itself at its first start. In Docker: `CREDITS_STORAGE=full` with
`CREDITS_DB_BACKEND=lmdb` or `rocksdb` (packaging/docker/README.md).

## Start-up check

Before opening the block database the node looks at `db/`. It recognises a database by
`db/.backend`, or by its files when the marker is missing (folders of the official node and of
earlier community releases): `blockchain.db` and `sequence.db` for berkeleydb, `lmdb/data.mdb` for
lmdb, `CURRENT` and `MANIFEST-*` for rocksdb. An empty database does not count.

| Found in `db/` | Result |
|---|---|
| nothing (first start) | The node creates the configured database, as before. |
| the configured backend | The node opens it, as before, and writes `db/.backend` if missing. |
| another backend | No conversion unless asked (below); the node never opens the configured backend on top of it. |
| two backends with data | Stops (exit 3): `found both X and Y in db/, remove the one you do not want`. |
| `db/.convert` | Resumes the interrupted conversion, if it converts to the configured backend; otherwise stops and names the `db_backend` that finishes it. |

So replacing the official binary with this node, with the official `config.ini` (no
`[storage]`), opens the same berkeleydb database and converts nothing.

When the database is of another backend than `db_backend`:

| How the node runs | Result |
|---|---|
| From a terminal | Shows both backends, the database size, the estimated peak disk use and the free space, and asks `Convert the block database from X to Y now? [y/N]`. Anything but `y` stops it (exit 3) with nothing changed. |
| Without a terminal (systemd, nohup, Docker) | Stops (exit 3): `the block database is X, config.ini asks for Y: run the node with --convert-db to convert it, or set db_backend=X`. |
| With `--convert-db` or `--convert-only` | Converts without asking. |

## Conversion

Every direction between `berkeleydb`, `lmdb` and `rocksdb`, full or pruned, except a pruned
database to berkeleydb. Blocks, hashes and consensus rules do not change: a converted node stays
compatible with the network.

### Command line

| Option | Meaning |
|---|---|
| `--convert-db` | Convert to `db_backend` without asking, then start the node. |
| `--convert-only` | Convert, then exit `0` without starting. With nothing to convert it says so and exits `0`. |
| `--convert-batch N` | Blocks per batch, `1` to `1000000`, default `100000`. A batch is held in memory. |

Steps:

1. stop the node;
2. set `db_backend` in `config.ini` to the new backend (keep the other settings);
3. run `./node --convert-only` (or start the node with `--convert-db`).

### How it works

The conversion runs in place, one batch at a time: read the batch from the old database, write it
to the new one, flush, read it back and verify it (bytes by sequence and by hash, the pool hash,
and the previous-hash chain across batches), record the progress in `db/.convert`, and only then
remove the batch from the old database. Contract data is copied first and removed from the old
database last. At the end the whole new database is checked, the old files are removed and
`db/.backend` is written.

The order depends on the source: from berkeleydb the newest block goes first, so that each
compaction shortens the old file; from lmdb and rocksdb the oldest goes first, because a
berkeleydb target has to grow from its first record.

### Disk space

| Source | Peak use |
|---|---|
| berkeleydb | the database plus one batch and the compaction space: about 1 GB beyond the database on mainnet |
| lmdb | about the old plus the whole new database: its files do not shrink until the conversion ends |
| rocksdb | the old database plus the new one, which is up to 4 times larger in lmdb or berkeleydb (RocksDB compresses its blocks: 3.3 GB became 11.6 GB in berkeleydb on mainnet) |

The node estimates the peak before starting and stops (exit 3) when the free space is lower,
naming both numbers.

### Time

Measured on a GitHub runner with the first 4.4 M mainnet blocks, extrapolated linearly to the
whole chain (about 186 M blocks; later blocks are larger, so take them as indications):

| Step | Blocks per second | Whole chain |
|---|---|---|
| sync from the network | 1,486 | about 35 h |
| convert to lmdb | 6,435 | about 8 h |
| convert to rocksdb | 38,807 | about 1.3 h |
| convert to berkeleydb | 10,194 | about 5 h |

### Interruptions

Ctrl-C or SIGTERM during a conversion stops it after the current batch: `conversion paused, it
resumes at the next start` (exit 0). A crash or a power cut leaves `db/.convert` consistent as
well. The next start resumes from the recorded batch, with or without `--convert-db`, as long as
`db_backend` still names the target.

Once every block is converted the old database is no longer needed. If a crash left a BerkeleyDB
environment that recovery cannot replay (`DB_RUNRECOVERY`), the next start says `the berkeleydb
database cannot be opened (...): every block is already in db/.convert-new, finishing without it`,
checks the new database and deletes the old files. Before that point an old database that does not
open stops the conversion with `blocks N..M are not converted yet` (exit 3), and `db/.convert` and
`db/.convert-new` are kept.

That last case comes from a crash during a BerkeleyDB compaction (or any other damage to the old
files), and the node cannot repair it by itself: the blocks not converted yet are only in the old
files. Before converting a berkeleydb database, stop the node and keep a copy of the whole `db/`
folder, taken right before the conversion (take a new one if the node ran in between). The copy
takes as much space as `db/`; another disk is fine. If the conversion then stops with `cannot be
opened (...) blocks N..M are not converted yet`, replace `db/` with the copy (the whole folder, so
that `db/.convert` and `db/.convert-new` go too) and convert again from the start. Without a copy,
start over from an empty `db/`, `caches/` and `qs/` (in Docker `CREDITS_RESET=1`: a pruned node
converting to lmdb then restores a snapshot, any other syncs from block 0).

No conversion starts while another node process holds `db/.lock`: `another node process is using
db: stop it first` (exit 3).

### Exit codes

| Code | When |
|---|---|
| `0` | `--convert-only` finished or had nothing to do; a conversion paused by Ctrl-C or SIGTERM. |
| `3` | Stopped before starting, nothing changed: backend mismatch without a terminal, a refused prompt, two backends in `db/`, too little free space, another node using the folder, a `--convert-batch` out of range, a pruned database to berkeleydb, an unreadable `db/.convert`. Or a conversion that failed a check: the batch being converted is still in the old database. |

## Docker

| `.env` | Meaning |
|---|---|
| `CREDITS_STORAGE` | `pruned` (default) or `full`. |
| `CREDITS_DB_BACKEND` | `lmdb` (default for pruned), `berkeleydb` (default for full) or `rocksdb`. |
| `CREDITS_CONVERT_DB=yes` | Pass `--convert-db` on every start, so that a change of `db_backend` converts the database (and an interrupted conversion resumes). Remove it afterwards. |
| `CREDITS_RESET=1` | Move the data aside to `~/.credits/old-<date>` and start over. |

`.env` is read on the first start only, to write `config.ini`; after that, edit `db_backend` in
`~/.credits/config.ini`. Without `CREDITS_CONVERT_DB=yes` the container refuses to start on a
backend change and prints the hint.

## Design

`docs/specs/2026-10-18-block-db-conversion-design.md`.
