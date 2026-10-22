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

### Choosing a backend

| Backend | Use it for | Notes |
|---|---|---|
| `berkeleydb` | A folder the official node must also be able to open | The official node's format. Its block table does not reuse the space of pruned blocks, so it is a poor fit for pruned storage. A pruned database cannot be converted to it. |
| `lmdb` | Pruned nodes (the pruned package and Docker default) | Reuses the space of removed blocks; `data.mdb` never shrinks. |
| `rocksdb` | The smallest full node | Compressed: about a third of the berkeleydb size on mainnet (9.6 GB to 3.1 GB on the first 4.4 M blocks). |

The official node reads berkeleydb only. On a folder converted to another backend it would not
find its files and would sync from block 0: convert back to berkeleydb before running it there.

### Full and pruned

Going from full to pruned keeps the database: set `prune_keep_blocks` and the old blocks are
removed at the next checkpoints. Going from pruned to full is not possible, the removed blocks are
gone: start over from an empty `db/`.

The pruned package writes this section:

```ini
[storage]
checkpoint_every=100000
checkpoint_keep=2
prune_keep_blocks=1000
block_db=lmdb
```

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
