# Backward sync: plan update for steps 1-2

For `2026-10-24-backward-sync-plan.md` (branch `docs/backward-sync`, 2effc1ef), checked against
master af50a7cd (after #99-#120: snapshots over p2p, bootstrap mode, `--download-snapshot`, survey,
Windows and macOS builds) before steps 1 and 2 were implemented (`feat/backfill-storage`,
`feat/backfill-blockchain`). The amendments of the p2p plan (`docs/snapshot-serving`,
`2026-10-25-snapshot-p2p-plan.md`, "Backward sync steps, interleaved") stand: BS3 after the bootstrap
mode with `bootstrapping` in its `Context`; BS-F, the computed floor of pruned nodes; BS4 without the
RocksDB conversion at the first start. File:line references below are on af50a7cd.

## Step 1 (`Storage::pool_save_backfill`)

- Line references unchanged: `pool_save` `storage.cpp:730-791` (writer queue 762-778),
  `Storage::flush()` 685, `first_sequence()` 1361-1369, `count_pool` at a quick start 311.
- **A lookup by sequence needs a buffer.** `DatabaseLmdb::get(seq, nullptr)` answers false whether or
  not the block is stored (`database_lmdb.cpp:170-177`). The plan's `d->db->get(uint32_t(seq))`
  would let a stored sequence through on LMDB. Both lookups by sequence pass a buffer.
- **The adjacency check replaces "below the first block".** `pool_save_backfill` refuses a block
  whose successor `seq + 1` is not stored, instead of comparing with `first_sequence()`: that one
  opens an iterator and seeks to the first record, which on a BerkeleyDB table pruned from block 0
  walks every deleted record, at every call. Together with "not stored by hash or by sequence" this
  also refuses a block above the head (its successor is not stored) and a gap.
- **Tests.** The blocks 50..60 cannot be written with `pool_save` (it needs the chain from block 0):
  they are put with the backend's `put` and the storage is opened as a quick start at 61, which is
  how a pruned storage opens. `csdb::backend::open` opens every backend in the test build (the build
  is always `both`), so the cases run on BerkeleyDB, LMDB and RocksDB, not only LMDB; BerkeleyDB
  passes on a fresh Recno file and on records deleted by `pool_prune_batch` (an early answer to step
  6). The `MemoryDb` of `storage_writer_tests.cpp:22-114` had an empty iterator
  (`storage_writer_tests.cpp:89-100`), with which a storage cannot open on stored blocks and
  `first_sequence()` is always "empty": the shared `tests/csnode/memorydb.hpp` iterates a copy of
  the blocks. `write_queue_search` is private: the asynchronous case checks the database itself
  while the writer is held.

## Step 2 (`BlockChain`, `TransactionsIndex`, `BlockHashes`)

- Line references, `blockchain.cpp` (CRLF): `init` 162-321, `firstStoredSeq_` set at 245-250,
  deferred index rebuild 290-297, block hashes check 298-312, `pruneStep` 354-399,
  `updateNonEmptyBlocks` 475-488, `applyBlockToCaches` 989-1046 (index update 1000),
  `saveDeferredBlock` 1341-1353, uuid in `recordBlock` 1726-1729, `getPreviousPoolSeq` 2484-2504.
  `transactionsindex.cpp`: reserved keys 20-26, `rebuildFrom` 237-293, `trimToFloor` 318-410,
  `updateFromNextBlock` 412-454, `onPruneBlock` 460-472, `hasIndexEntries` 506-529.
- **Lock order.** The wallet cache's lock (`cacheMutex_`) is taken before `dbLock_`, never after
  (`blockchain.cpp:2042-2045`). The index walk reads the wallet cache (`getLastTransaction`), so
  `storeBackfilledBlock` indexes the block outside `dbLock_` and takes `dbLock_` for the block
  hashes, the non-empty map, the storage and `firstStoredSeq_`. The forward index is written
  outside `dbLock_` as well (`applyBlockToCaches`).
- **Anchor.** `storeBackfilledBlock` keeps the anchor (`first`, previous hash) of the block it
  stored last, so the check costs no load per block; it is recomputed when `firstStoredSeq_`
  changes (pruning).
- **No key-length changes needed.** `trimToFloor` skips keys that are not 40 bytes (line 340) and
  `hasIndexEntries` counts only 40-byte keys (line 519); `looksEmpty` reads only the watermark. Tail
  records (33 bytes) and `__backfill_floor__` are ignored by both already; a test covers
  `trimToFloor`.
- **Values are decimal text.** `cs::Lmdb` writes integer keys and values as decimal strings
  (`lmdb.hpp:345-368`): the tail records follow the same convention, and an entry is read in one
  transaction as a string (empty = absent).
- **Tails.** A tail below the block being indexed means the block was indexed before (a crash): no
  pointer is written then, the tail record is kept. The walk also stops at an entry pointing up or
  to itself, so a damaged index cannot make it loop.
- **Non-empty map merge.** The design's rule ("N = the lowest key above Q") misses two cases: N's
  previous block above Q (the oldest block of the map, which has no entry of its own: Q goes under
  it), and no key above Q (Q goes under `lastNonEmptyBlock_` if that is above Q).
- **Start-up repairs only where a backfill ran.** Both repairs run only when the index has a
  `__backfill_floor__` key. On a pruned node that never backfilled, `repairBottom` would otherwise
  also remove the hash entries a crash left below the first block during a pruning step, which the
  128-entry slack of `consistentWith` tolerates today: a change of behaviour the plan did not
  intend. The floor repair is skipped when the index is rebuilt at this start (`rebuildFrom` resets
  it). Both run before the block hashes check, as planned.
- **`repairBottom`** also restores a missing hash-to-sequence entry where the sequence-to-hash one
  is present, so the start-up check finds both ways.
- **Index tests without a node.** `TransactionsIndex::setChainView` gives the wallet cache's last
  transaction block and the first stored block; the tests build the forward index with
  `rebuildFrom` and a pruned lineage with `onPruneBlock`.

## Impact on later steps

- **Step 3.** `node.cpp`/`transport.cpp` references of the plan predate #99-#120 and are re-checked
  with step 3. `BackwardSync` runs next to `SnapshotFetcher` on the processor thread; its `Context`
  gets `bootstrapping` (p2p plan).
- **BS-F and pruning.** `pruneStep` removes from `firstStoredSeq_` up to `pruneUpTo_`: on a pruned
  node a backfill below `pruneUpTo_` would be pruned again. The floor of BS-F is
  `C − (checkpoint_every + prune_keep_blocks)`, the formula of `pruneUpTo_`
  (`schedulePruning`): the backfill must stop at `max(floor, pruneUpTo_)`. Pruning removes the
  index entries of the blocks it removes; the tail records it leaves are rejected (no entry), so
  backfill and pruning can alternate.
- **Step 4.** As the p2p plan says, the conversion to RocksDB at the first start is dropped: the
  snapshot importer writes the configured backend and refuses BerkeleyDB (`snapshot_importer.cpp:118`).
- **Step 6.** The step-1 tests already store blocks below the first one on BerkeleyDB, in a fresh
  table and in records pruning deleted; the scenario stays.
- **Checkpoint bytes.** A merge into `previousNonEmpty_` changes the bytes of the next checkpoint.
  The snapshot agreement compares the head block and the digests only
  (`snapshot_agreement.cpp:82-98`), and the fetcher groups sources by identical files, so at worst
  fewer sources serve that one file; on a node from a snapshot the merge is expected to find every
  block linked already.

## Outside the plan

- `cs::Lmdb::cast` formats integer keys and values into one `static` buffer per type
  (`lmdb.hpp:351`, not on macOS). The processor thread writes integer values (index entries, and
  now the backfill floor and tails) while API threads look up integer keys
  (`BlockHashes::find(seq)` through `getHashBySequence`): two threads can format into the buffer at
  once, which can name a wrong key or write a wrong value. This exists on master for the forward
  index; the backfill writes more often. A one-word fix (`static thread_local`) belongs in its own
  pull request before step 3.
