# Block database conversion: design

Status: draft for review, 2026-10-18.

## Goal

An operator with a full node on BerkeleyDB can move it to LMDB or RocksDB, keeping it a full node on less disk, without downloading the chain again and without free space for a second copy of the database.

Two pieces:

1. **Detection.** When the node starts and finds a block database, it recognises its format. If the format differs from `[storage] db_backend`, it never overwrites or ignores the existing database: it offers to convert it.
2. **Conversion.** The node converts the block database in place, one batch of blocks at a time. Each batch is copied, verified and then removed from the old database, so the disk holds the old database plus one batch at most. An interrupted conversion resumes where it stopped.

Every direction between `berkeleydb`, `lmdb` and `rocksdb` is supported. Blocks, block hashes and consensus rules do not change, so a converted node stays fully compatible with the official network.

Out of scope: converting between full and pruned (already handled by `prune_keep_blocks`), and converting the official node's checkpoints (they lack a head record and are not used anyway).

## What is converted

Only the block database depends on the backend:

| Data | Location | Converted |
|---|---|---|
| Blocks (key = block hash, sequence, serialized pool) | BerkeleyDB `db/blockchain.db` + `db/sequence.db`; LMDB `db/lmdb/`; RocksDB `db/` (column families) | yes |
| Contract data (`updateContractData` / `getContractData`) | BerkeleyDB `db/contracts.db`; inside the LMDB / RocksDB environment | yes |
| Checkpoints `qs/` | LMDB, independent of `db_backend` | no, kept as is |
| Transaction index `caches/indexdb` | LMDB, independent of `db_backend` | no, kept as is |

Because checkpoints and the transaction index stay valid, the node quick-starts after the conversion exactly as it would have before.

## 1. Detection

**Marker.** Whenever the node opens a block database it writes `db/.backend`, one line: `berkeleydb`, `lmdb` or `rocksdb`. The official node ignores the file.

**Fingerprint**, for folders without the marker (official nodes, earlier community releases):

| Files | Format |
|---|---|
| `db/blockchain.db` and `db/sequence.db` | berkeleydb |
| `db/lmdb/data.mdb` | lmdb |
| `db/CURRENT` and `db/MANIFEST-*` | rocksdb |

**Decision at start-up**, before `Storage::open`:

| Found | Configured | Action |
|---|---|---|
| nothing | any | new database, as today |
| X (with or without marker) | X | open, write the marker if missing |
| X | Y ≠ X | ask (below); never open Y on top of X |
| two formats with data | any | stop: "found both X and Y in db/, remove the one you do not want" |
| `db/.convert` present | any | resume the conversion it describes (section 3) |

**Asking.**

- stdin is a terminal: the node prints the found and configured formats, the database size, the estimated peak disk use and the free space, and asks `Convert the block database from X to Y now? [y/N]`. `N` (or no answer) stops the node with exit code 3 and nothing changed.
- no terminal (Docker, systemd, `nohup`): the node stops with exit code 3 and a message: `the block database is X, config.ini asks for Y: run the node with --convert-db to convert it, or set db_backend=X` (in Docker: `CREDITS_CONVERT_DB=yes`).
- `--convert-db` on the command line: convert without asking.
- `--convert-only`: convert, then exit 0 without starting the node.

## 2. Conversion

**Order: newest block first from berkeleydb, oldest block first from the others.** BerkeleyDB gives disk space back only from the end of its files (compaction with `DB_FREE_SPACE`, already used by pruning in bounded steps). The newest blocks are at the end of `blockchain.db`, so with a berkeleydb source the node walks from the last stored block down to the first, and each compaction shortens the file. LMDB and RocksDB sources do not shrink during the conversion anyway (see **Disk**), so from them the node walks from the first stored block up to the last. That order matters for a berkeleydb target: BerkeleyDB keeps blocks in a Recno table where record N is sequence + 1, and a first put far above record 1 creates every lower record in one operation. On mainnet (4,008,827 blocks, rocksdb to berkeleydb, newest first) that put failed with `BDB2055 Lock table is out of available lock entries` and `Db::put: Cannot allocate memory`; oldest first, the table grows record by record.

**Pruned database to berkeleydb: refused.** Its first block is far above record 1 in either order, so the conversion stops before anything is written: `a pruned block database cannot be converted to berkeleydb; use lmdb or rocksdb` (exit 3).

**Batch.** 100,000 blocks by default (`--convert-batch N`). For each batch of sequences `lo`..`hi`, read and written in the conversion's order:

1. read every block of the batch from the old database (`get(seq)`), parse it, take its hash;
2. write them to the new database (`put(hash, seq, value)`), then `flush()` (the BerkeleyDB and RocksDB flush already syncs to disk, LMDB syncs on commit);
3. verify: read each block back from the new database by sequence and by hash, compare the bytes, recompute the pool hash and check that block `seq` names block `seq-1`'s hash as previous (across batch boundaries too, through the hash recorded in the state file);
4. record the batch in `db/.convert` (written to a temporary file, synced, renamed);
5. remove the batch from the old database (`remove_batch`), then compact it (BerkeleyDB) or let the backend reclaim space (LMDB reuses pages, RocksDB compacts in the background).

A batch is removed from the old database only after step 4. A failure in steps 1–3 stops the conversion with the old database untouched for that batch.

**Contract data** is copied first, all at once (it is small), with the same write–flush–verify sequence, and removed from the old database only when every block is converted. This needs one new method, `Database::forEachContractData(callback)`, in each backend.

**State file** `db/.convert`:

```
from=berkeleydb
to=lmdb
first=0
last=186002345
order=desc
done_down_to=185902346
next_prev_hash=<hex: previous_hash of block 185902346, the hash block 185902345 must have>
contracts=copied
phase=blocks
```

From lmdb or rocksdb:

```
from=rocksdb
to=berkeleydb
first=0
last=186002345
order=asc
done_up_to=99999
done_up_to_hash=<hex: hash of block 99999, which block 100000 must name as previous>
contracts=copied
phase=blocks
```

A state file without `order=` was written before the order was recorded and is read as `desc`. Each order has its own boundary key; a file with the other order's key is unreadable.

**Resume.** On start-up with `db/.convert` present, the node opens both databases and continues in the recorded order, from `done_down_to - 1` downwards or from `done_up_to + 1` upwards. If it stopped between steps 2 and 4, the batch is in both databases: it is written again (idempotent) and verified. If it stopped during step 5, the batch is partly removed from the old database: removal is repeated for the keys still present.

**End.**

1. Check the whole new database: first and last sequence equal the recorded `first` and `last`, every sequence in between present, hash chain intact.
2. Remove the contract data and the now-empty files of the old backend.
3. Write `db/.backend`, delete `db/.convert`.
4. Log the old size, the new size and the time taken.

Then the node starts normally, or exits with `--convert-only`.

**Disk.** With a berkeleydb source, peak use is the old database plus one batch in the new format plus the compaction work space, about 1 GB on mainnet. With an lmdb or rocksdb source the old files do not shrink during the conversion (LMDB never shrinks `data.mdb`, RocksDB deletes are tombstones compacted later), so the peak is about the old plus the whole new database, and the node checks for that much free space. Before starting, the node estimates this and refuses (exit 3) when the free space is lower, naming both numbers.

**Pruned nodes.** The same procedure over the stored range `first_sequence()`..`last`, to lmdb or rocksdb; the converted node keeps its pruning settings. To berkeleydb a pruned database is refused (see **Order**).

## 3. Docker

`entrypoint.sh` today refuses to start when `.storage` names another backend than `config.ini`. With `CREDITS_CONVERT_DB=yes` it passes `--convert-db` to the node instead, and records the new backend in `.storage` once the node has started. Without the variable the refusal stays, with the new hint in its message.

## 4. Testing

- **csdb unit tests.** Every pair of backends: a database of a few thousand blocks built from the private-network genesis, converted, compared block by block; conversion killed after each step of a batch (fault injection hook), then resumed; a corrupted read in the old database stops the conversion with nothing removed.
- **Private network** (`robustness.yml`), new scenario `db-convert`:
  - node 4 (berkeleydb, full) is stopped at sequence 1500, converted to lmdb with `--convert-only`, restarted;
  - node 6 (berkeleydb, full) is converted to rocksdb and killed in the middle of the conversion, then started again with `--convert-db`: it must resume and finish;
  - pass: every node reaches the target sequence with the same state digests, both converted nodes quick-start from their existing checkpoint.
- **Mainnet data** (`upgrade-compat.yml`): the official folder with the first million blocks is converted to lmdb and to rocksdb; sizes and times are reported; the converted node syncs on and its balances and histories are compared with the public monitor as today.
- **Is it worth it** (same mainnet job): the conversion rate (blocks per second, separately for copy+verify and for removal+compaction) is measured on the sample and extrapolated to the full chain (~186 M blocks), next to the two alternatives on the same runner:
  - a fresh sync from the network with the configured backend: the job measures it on the same block range, and the local pruned node gives a long-run reference (about 25–30 M blocks per day, about 7 days for the whole chain);
  - for completeness, the community node's slow start from a local database (state rebuilt without downloading).

  The report gives the estimated hours for the whole chain for each. The conversion is worth shipping only if it is clearly faster than the network sync (the expectation is hours against days); if it is not, the feature stops at detection and the hint to resync.
- **Docker** (`test-entrypoint.sh`): mismatch without the variable refuses with the new hint; with `CREDITS_CONVERT_DB=yes` the node is called with `--convert-db`.

## Risks

- **BerkeleyDB compaction speed** on a 90+ GB file: measured in the mainnet test before release; if it is too slow, compaction runs every few batches instead of after each one, at the cost of a higher peak.
- **Power loss during step 5**: covered by repeating the removal on resume; a block missing from both databases cannot happen, because removal follows the synced record of step 4.
- **Running the official node on a converted folder**: it would not find its BerkeleyDB files and would sync from block 0. The README says so; converting back (to berkeleydb) restores it.
