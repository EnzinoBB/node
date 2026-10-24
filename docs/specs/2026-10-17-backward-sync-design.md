# Backward sync (backfill): design

Status: draft for review, 2026-10-17; updated 2026-10-19 (f526f466) and 2026-10-24 against master
b5f74ecd (4.2.532-community.11). The implementation plan is `backward-sync-plan.md`, next to this
file. Every file:line reference below was checked on b5f74ecd.

## Goal

A full node that starts from a pruned snapshot takes part in consensus at once. In the background
it downloads the older blocks from peers, newest first, down to block 0, and becomes a full node.
Today a full node must sync from block 0 for about 35 hours on a fast machine, and days on a small
one, before it can do anything.

Decisions taken with the user (unchanged):
- **No final verification of the snapshot state.** A full replay from block 0 is not run at the end.
  The downloaded history itself is verified, by hash chain and signatures.
- **All three block databases** (BerkeleyDB, LMDB, RocksDB) support backward sync.

Out of scope: serving snapshots from node to node; the full/pruned ratio guard.

## Since 2026-10-19

What changed on master and what it changes here. Marked **[changed]** in the sections below.

- **The first pruned snapshot is published** (release `pruned-snapshot`, `latest.json` +
  `credits-pruned-<seq>.tar.zst`, refreshed every 12 h by `.github/workflows/snapshot.yml`; today
  block 186,466,024, 282 MB). A snapshot holds `db/lmdb`, `caches/` (but `poolcachedb`), `qs/`,
  `roundstat.dat`, `smartcontracts.dat`, made with `checkpoint_every=100000`, `checkpoint_keep=2`,
  `prune_keep_blocks=1000`: its oldest block is 100,000 to 200,000 below its head. The mainnet test
  and the use of backward sync are no longer blocked.
- **Review fixes #57–#64** touch the code this design relies on: the RocksDB writer owns its batch
  and a full disk makes `pool_save` fail instead of hanging (section 4 writes backfilled blocks
  outside that queue); pruning holds `dbLock_` (section 4 uses the same lock); a pruned node answers
  `NoSuchBlocks` for blocks it pruned, **before** it records the request in its anti-duplicate log
  (`node.cpp:1850-1856`): a community pruned peer never leaves a dead entry (section 2); the
  block-hash databases are rebuilt when they do not match the stored range (`blockchain.cpp:266-279`,
  `blockhashes.cpp:102-131`), which section 5 must not trigger by accident; `seq_no` is 0-based on
  every backend.
- **Index rebuild after a quick start (#53, #54)** stays the fallback for a damaged index. The
  backfill indexes each block as it stores it (section 6), so that the history is usable at once,
  without a restart **[changed 2026-10-24, user's decision: no restart, everything dynamic]**.
- **Conversion (#49–#52, #63, #65, #67)** refuses a database whose first block is above 0 to
  berkeleydb (`storage_convert.cpp:328-334`). Once the backfill reached block 0 that conversion is
  allowed, so a node from a snapshot can end as a BerkeleyDB folder the official node opens.
- **#83 (contract replay is not reproducible)** concerns forward sync only: backfilled blocks are
  stored, never applied. A later slow start of a backfilled node would replay them and could differ
  from the snapshot's balances by fees on a few contract callers, as any from-scratch sync does.
- **#73/#74, #70/#71/#80, #82, #84/#86**: no effect on this design.
- **Measurements [changed]**: section 10 has the block size, the chain size and the rates measured
  on mainnet and on the published snapshot.

## Compatibility

- **No protocol change.** Blocks are requested with the existing `BlockRequest` message, so official
  4.2.532 nodes, all full, serve the history as they serve any sync today. Their replies
  (`RequestedBlock`) are handled on a separate path in our node. Their `SyncroMsg` answers
  (`AwaitAnswer`, `NoSuchBlocks`, `DuplicatedRequest`) are read by that path too.
- **No change to blocks or consensus.** Backfilled blocks are stored, never applied to the wallet
  caches. The state already comes from the snapshot. Nothing in this design is consensus-relevant.

## What the code does today (master b5f74ecd)

- **Requests and replies.** A request is an arbitrary list of sequences
  (`nodecore.hpp:42`, `PoolsRequestedSequences`). The responder, `Node::getBlockRequest`
  (`node.cpp:1828-1888`), refuses an empty list, then the anti-duplicate log (below), then answers
  `NoSuchBlocks` when the first sequence is outside `[firstStored, last]`
  (`node.cpp:1850-1856`, `blockrequest.hpp:17-22`); otherwise it records the request, sends
  `AwaitAnswer`, loads every sequence with `loadBlockForSync` (skipping those it cannot load,
  `node.cpp:1868-1877`) and sends one compressed `RequestedBlock` (`node.cpp:1939-1960`), or
  `NoSuchBlocks` if it loaded nothing. The mainnet `config.ini` asks 150 blocks per request
  (`packaging/mainnet/config.ini:28`); the responder has no limit of its own.
- **Peers.** `PoolSynchronizer::onPingReceived` (`poolsynchronizer.cpp:288-295`) keeps only peers
  ahead of our last block. The transport knows every neighbour's last sequence:
  `Transport::forEachNeighbour` (`transport.cpp:422`, callback `(key, lastSeq, round)`), at most
  `kMaxNeighbours = 16` (`neighbourhood.hpp:23`).
- **Old blocks are ignored.** `Node::getBlockReply` drops every reply when forward sync is off
  (`node.cpp:1890-1898`); `PoolSynchronizer::manageSyncBlocks` skips blocks below the last one
  (`poolsynchronizer.cpp:134-137`); `BlockChain::storeBlock` ignores them
  (`blockchain.cpp:2004-2016`). `Node::sendBlockRequest` sets the node status to
  `Synchronization` (`node.cpp:2148-2156`).
- **Saving.** `Storage::pool_save` accepts only the block after the head
  (`storage.cpp:748-751`); for RocksDB it queues the block for the writer thread
  (`storage.cpp:761-777`). Every backend's `put(hash, seq, bytes)` stores any sequence: BerkeleyDB
  in a Recno table opened without `DB_RENUMBER` (`database_berkeleydb.cpp:243`, record = seq + 1,
  `database_berkeleydb.cpp:316-331`), LMDB with an integer key (`database_lmdb.cpp:120-141`),
  RocksDB with a big-endian key (`database_rocksdb.cpp:178-197`). `first_sequence()` is the first
  key of an iterator (`storage.cpp:1353-1361`). `Storage::size()` is `count_pool`, which a quick
  start sets to the checkpoint's sequence (`storage.cpp:311`): it counts sequences, not stored
  blocks **[changed]**: the first draft expected it to grow with each backfilled block; it must not.
- **Hashing and signatures.** The hashed part of a block covers version, previous hash, sequence,
  user fields, round cost, transactions, new wallets, `numberTrusted`, `realTrusted`, the
  confidants, `numberConfirmations`, the confirmation mask and the round confirmations
  (`pool.cpp:122-153`). Outside it: `hashingLength_`, the pool signatures and the smart signatures
  (`pool.cpp:157-173`). `hashingLength_` is checked against the data size when a block is parsed
  (#5, `pool.cpp:239-251`). `Pool::hash()` is computed locally from the bytes. Signatures are
  checked with `NodeUtils::checkGroupSignature(confidants, mask, signatures, hash)`
  (`nodeutils.cpp:15`), as `finalizeBlock` does for every synced block (`blockchain.cpp:868-949`):
  confirmations of block k against the confidants of block k − 1 for k > 1, unless the block is a
  bootstrap block or the mask has one entry (`blockchain.cpp:896-904`), then the pool signatures
  against the block's own confidants for k > 0 (`blockchain.cpp:922-934`); the genesis block has
  none (`blockchain.cpp:936-938`). `recordBlock` takes the previous confidants from the deferred
  block or loads block k − 1 (`blockchain.cpp:1602-1612`). Smart signatures are checked by
  `Node::deepBlockValidation` with the solver's own validation of the block's transactions
  (`node.cpp:4536-4600`), which needs the wallet state of that time.
- **Pruning** deletes from `firstStoredSeq_` upwards under `dbLock_` (`blockchain.cpp:320-367`);
  it is scheduled only when `prune_keep_blocks > 0` (`blockchain.cpp:307-318`). `firstStoredSeq_`
  is atomic (`blockchain.hpp:614`), set at start from `first_sequence()` (`blockchain.cpp:225-230`).
- **Start-up.** A slow start refuses a storage whose first block is above 0
  (`storage.cpp:322-333`). The transaction index is rebuilt from the stored blocks after a quick
  start when it needs it (`blockchain.cpp:143-163`, `256-262`; `transactionsindex.cpp:237-294`).
  The block-hash databases are rebuilt when `consistentWith(first, last)` fails: their entry counts
  must be within `[blocks, blocks + 128]` of the stored range and three samples must map both ways
  (`blockhashes.cpp:102-131`).
- **Caches' durability.** The LMDB block database runs with `MDB_NOSYNC`
  (`database_lmdb.cpp:83-85`), synced by `Storage::flush()` at every checkpoint
  (`blockchain.cpp:1025-1028`) and at close (`blockchain.cpp:1322`). The hash and index databases
  (`cs::Lmdb`) run with `MDB_NOSYNC | MDB_WRITEMAP | MDB_MAPASYNC` (`lmdbxx/lmdb.hpp:43`) and are
  flushed at the same points.
- **The transaction index.** One LMDB table (`caches/indexdb`), key = public key (32 bytes) ‖
  sequence (8 bytes, native byte order, so keys of one wallet are not in sequence order), value
  = the previous block holding a transaction of that wallet, or `kWrongSequence` for none
  (`transactionsindex.cpp:28-35`, `448-458`). Each wallet's history is a chain newest → oldest:
  `TransactionsIterator` starts at the wallet cache's last transaction and follows
  `getPrevTransBlock` (`transactionsiterator.cpp:8-13`, `51-80`), stopping at a block it cannot
  load (#62: on a pruned node the oldest stored entry still points to a pruned block, whose own
  entry pruning removed, `transactionsindex.cpp:460-476`) or at `kWrongSequence` (what a rebuild
  from stored blocks writes at a wallet's oldest stored entry, `transactionsindex.cpp:244-246`).
  Writes happen on the transport's processor thread (`applyBlockToCaches`, `blockchain.cpp:964`);
  API threads read through their own LMDB read transactions. `getPreviousPoolSeq` stops the node
  if an entry points to itself (`blockchain.cpp:2394-2410`). Every `cs::Lmdb::insert` is one
  write transaction (`lmdbxx/lmdb.hpp:165-183`).
- **Non-empty blocks.** `TransactionsListGet` walks `previousNonEmpty_`
  (`apihandler.cpp:2760-2800`, `blockchain.cpp:439-452`), a map saved in every checkpoint
  (`blockchain_serializer.cpp:73-75`) and never pruned. Every published snapshot descends from the
  first one, whose node synced from block 0: the map of a snapshot node covers the whole chain, and
  the walk skips the blocks it cannot load (#46).
- **Other API reads of history** go to the storage by sequence or by hash: `PoolInfoGet`,
  `PoolListGet`, `PoolTransactionsGet`, `TransactionGet` (`loadBlock`, `loadTransaction`,
  `blockchain.cpp:580-640`); wallet transaction counts come from the wallet cache; contract
  getters use the deploy the smart contracts keep (#74).
- **The responders' anti-duplicate log.** Active only once special order 35 has set
  `Consensus::syncroChangeRound` (`node.cpp:4408-4413`, default `ULLONG_MAX`,
  `consensus.cpp:129`); every function of the log returns at once below that round
  (`node.cpp:1698-1826`). When active: a request whose last sequence is not above the last one
  logged for the same sender is refused with `DuplicatedRequest` (`node.cpp:1801-1826`); an entry
  turns to `Sent` when the reply goes out and expires 5 s later (`node.cpp:1739-1788`,
  `1956`); an entry left in `AwaitAnswer` (the responder loaded nothing after accepting the
  request) never expires. Whether mainnet nodes ever received order 35 is not known: the local
  pruned node never applied it (it quick-started from a snapshot taken after block 0's era; its
  logs have no `Changes will be aplied in round`), and the replies it gets do not tell. The
  design detects it per peer (section 2) instead of assuming either case.

## Design

### 1. When it runs

- Backward sync runs when the storage is full (`prune_keep_blocks=0`) and the first stored block
  is above 0. A pruned node never backfills. Switching a pruned node to full starts the backfill;
  the Docker entrypoint, which today refuses pruned to full (`entrypoint.sh:123-125`), allows it
  from the release that ships this (section 8).
- It runs only while the node is synced: forward sync is not running
  (`PoolSynchronizer::isSyncroStarted()`), the last block is within 3 of the current round, and
  the node is not in a bootstrap round. Whenever that stops holding, no new request goes out;
  replies already in flight are still verified and stored, since storing never touches the
  caches. **[changed]** A trusted node keeps backfilling: one batch of 100 blocks costs a few
  milliseconds of signature checks and one storage transaction, on the thread that already stores
  synced blocks; the robustness scenario `backfill-trusted` measures it (section 11).
- `[storage] backfill=off` disables it. On by default for full nodes.
- **Backends.** A node from a snapshot runs on LMDB, or on RocksDB after a conversion, and
  backfills there. A BerkeleyDB node backfills only if it was a pruned BerkeleyDB node from block 0
  switched to full: records below the first stored one were deleted one by one, so each put fills
  an existing hole and the lock exhaustion of the conversion cannot happen. Conversion of a
  database whose first block is above 0 to berkeleydb stays refused until the backfill reached 0.

### 2. Requests **[changed: adaptive pacing, peer detection of order 35]**

- **Anchor.** `first`, the lowest stored block, and `anchorPrev = pool_load(first).previous_hash()`.
  The next block to store is `first − 1`.
- **Batches.** Up to `backfill_batch` sequences (default 100, at most 1000) per request, ascending
  inside the batch as forward sync sends them: `[first − 100 .. first − 1]`, then
  `[first − 200 .. first − 101]`, and so on. At most 4 batches in flight in total, each with its own
  peer, covering consecutive ranges downwards.
- **Peers.** Every neighbour whose last sequence (from `forEachNeighbour`) is above the batch,
  except those marked. Forward sync's own list is not used: it holds only peers ahead of us.
  Marks: a peer whose batch failed verification, or that answered `NoSuchBlocks` or nothing
  (timeout) twice, is left out for 1 hour; a peer that answered `NoSuchBlocks` after
  `AwaitAnswer` is left out for the rest of the run (its anti-duplicate log holds an entry for us
  that never expires, and every lower request would be refused).
- **Pacing.** One batch in flight per peer. The next request to the same peer goes out:
  - at least 6 s after its reply, if that peer ever sent us `AwaitAnswer` (so its anti-duplicate
    log is active: the next request has a lower last sequence and would be refused until the
    `Sent` entry expires, 5 s after the reply);
  - otherwise at once, subject to the global rate.
  A `DuplicatedRequest` from a peer sets its pace to 6 s and delays the next request to it by
  10 s. The batch size is below the 150 blocks mainnet nodes already answer to forward sync.
- **Rate.** `backfill_rate` blocks per second over all peers (default 1000, 0 = unlimited), as a
  token bucket on the requests: about 2 MB/s of block bytes at 1000 blocks/s before the responder's
  compression (section 10). With the 6-s pacing on every peer and 10 full peers the rate is about
  170 blocks/s, whatever the limit.
- **Timeouts.** A batch not complete 30 s after its request is requested again from another peer,
  for the sequences still missing; the slow peer waits 60 s before its next batch. Blocks a
  responder skipped (it could not load them) are requested the same way.
- **Replies.** A reply counts only if it comes from a peer with a batch in flight and only for the
  sequences of that batch; everything else is dropped before it is buffered.

### 3. Verification of each block, before it is stored

For the next expected block k (the one below the lowest stored block k + 1, whose `previous_hash`
is known):

1. `pool.is_valid()` and `pool.sequence() == k`.
2. `pool.hash()`, computed locally from the received bytes, equals the `previous_hash` of block
   k + 1. This pins the whole hashed part: transactions, confidants, trusted mask, confirmations.
3. For k ≥ 1: the pool signatures verify against the block's own confidants with
   `mask = bitsToMask(numberTrusted, realTrusted)`, exactly as `tryFlushDeferredBlock` does
   (`blockchain.cpp:1277-1285`); the signatures are outside the hashed part and are the only thing
   a peer could alter without changing the hash. Block 0 has no signatures and needs none.
4. For k ≥ 2, one block late: when block k − 1 is accepted, the round confirmations of block k are
   checked against the confidants of block k − 1 (`finalizeBlock`'s rule, with its exceptions for
   bootstrap blocks and one-entry masks). They are inside the hashed part, so a failure cannot come
   from the peer: it is logged as an error with the sequence and counted, and the backfill goes on
   (a full sync from block 0 passes every mainnet block through the same check today, so none is
   expected).

A block that fails 1–3 is dropped with its whole batch, the peer is marked, and the batch is
requested from another peer. Blocks of a batch may arrive in any order and batches out of order:
they wait in a buffer of at most 2 × 4 × `backfill_batch` blocks, and are stored strictly
downwards. Smart signatures, also outside the hashed part, are not verified: checking them needs
the wallet state of that time (`deepBlockValidation`). Residual risk, accepted: section 9.

### 4. Storage **[changed: no count, synchronous on RocksDB]**

- **A new `Storage::pool_save_backfill(const Pool&)`:**
  - refuses a block already present by hash or by sequence;
  - writes it with the backend's `put(hash, seq, bytes)` directly, on every backend, including
    RocksDB: not through the writer queue, whose bookkeeping (`in_flight`, `removals_waiting`,
    `write_failing`) is about the last blocks. A failing database (full disk) makes it return
    false; the backfill then waits 60 s and retries the same block;
  - leaves `last_hash`, `count_pool` and `pools_cache` untouched;
  - is called under `dbLock_`, the lock of every other storage call.
- **Strictly downwards**: after a crash the stored range has no hole; `first_sequence()` is where
  the backfill resumes, no other watermark is kept.
- **Durability.** Every 10,000 backfilled blocks or 60 s: the index database's flush (section 6),
  the hash databases' flush (section 5), then `Storage::flush()` (LMDB `mdb_env_sync`, RocksDB
  `SyncWAL`), so that after a power cut the caches are never behind the storage by more than the
  window the OS flushed on its own, and the window itself is bounded, as the forward blocks since
  the last checkpoint are today.
- **RocksDB and LMDB alike.** Neither backend orders its writes: a block of any sequence is one
  `put` with its key (`database_lmdb.cpp:120-141`, `database_rocksdb.cpp:178-197`), so a RocksDB
  node backfills exactly as an LMDB one, from the first block it has down to 0. The only LMDB
  point is the snapshot itself (section 8).
- **BerkeleyDB.** Each put fills the record `seq + 1` just below the lowest stored one, deleted by
  an earlier pruning. The same code path as LMDB and RocksDB; the scenario `backfill-berkeleydb`
  is the proof.

### 5. Caches **[changed: bottom-edge repair, flush order]**

- `blockHashes_->update(pool)` for each backfilled block, before the storage put, so that `seqdb`
  and `hashdb` answer lookups by sequence and by hash for the whole stored range.
- `firstStoredSeq_` drops to k once the block is written. The responder then serves it.
- When block 1 is stored, `uuid_` is set from it as `recordBlock` does (`blockchain.cpp:1637`): a
  node from a snapshot has `uuid_ == 0` today (block 1 is read only at a slow start or when it is
  recorded, `blockchain.cpp:377-383`, `1636-1638`).
- **Start-up check.** The hash databases are flushed before the storage, so after a crash they can
  only be ahead at the bottom (entries below `first_sequence()`), never behind. Before
  `consistentWith` runs, `BlockChain::init` repairs the bottom edge: entries below the first stored
  block are removed, and missing entries from the first stored block upwards are inserted from the
  storage, both loops ending at the first block that needs nothing. Without this, a crash during
  the backfill could leave more than the 128 entries of slack and trigger a full rebuild of the
  hash databases, which reads every stored block.
- **Non-empty blocks.** `previousNonEmpty_` already covers the chain by lineage (above). As
  insurance, a backfilled block Q with transactions is merged under `dbLock_`: N = the lowest key
  above Q in the map; if N exists and `previousNonEmpty_[N].poolSeq != Q`, then
  `previousNonEmpty_[Q] = previousNonEmpty_[N]` and `previousNonEmpty_[N] = {Q, count}`. Merged
  entries reach disk with the next checkpoint. `lastNonEmptyBlock_` and
  `totalTransactionsCount_` are not touched (the lineage's values).
- Nothing else is touched: the wallet caches and the counters already describe the full history.

### 6. Transaction index **[changed 2026-10-24: indexed online, no restart]**

The user's rule: when the blocks are there the node uses them at once, with nothing to restart,
with or without Docker. So each backfilled block is indexed as it is stored, and a wallet's
history grows downwards while the node runs. The rebuild after a quick start (#53/#54) stays only
as the fallback for a damaged index.

**The chain of a wallet during the backfill.** For wallet A the entries newest → oldest end at
the oldest stored block holding A, whose value names either a pruned block P (the snapshot's
lineage pruned it) or `kWrongSequence` (the index was rebuilt from stored blocks). Call that
entry the **tail** of A. Blocks are backfilled strictly downwards, so when block Q arrives every
block between Q and the first stored one is already stored and indexed, and the tail is the
smallest indexed block of A above Q.

**Indexing one backfilled block Q** (addresses deduplicated as `updateFromNextBlock` does,
`transactionsindex.cpp:412-454`), before the block goes to the storage:

1. `tail = tailOf(A)`: a per-wallet record in the index database, key `'t' ‖ public key`
   (33 bytes, never equal to a 40-byte entry key), value = the lowest indexed sequence of A. The
   record is valid if the entry (A, tail) exists; otherwise, and when there is no record, the tail
   is found by walking A's chain from the wallet cache's last transaction down to the first entry
   whose value is `kWrongSequence` or below the first stored block, as `TransactionsIterator`
   does without loading blocks. The walk happens once per wallet, costs one LMDB read per entry
   and is bounded by A's transactions in the stored range; a wallet with no entry at all (its only
   transactions are below the first stored block, so its last transaction in the wallet cache
   names an unstored block) has no tail.
2. If there is a tail: set (A, tail) → Q, unless its value is Q already (a pruned lineage left it
   pointing at Q).
3. Insert (A, Q) → `kWrongSequence` if that key is absent (a crash may have left it with a lower
   value already; keep it).
4. Write the tail record (A) → Q.

Then write `__backfill_floor__` → Q in the index database, and only then store the block
(section 4). Every write is one LMDB transaction, as every write of the index is today.

**Why this is right.** Step 2 never writes a pointer to itself (Q < tail), so the node never
stops for an inconsistent index. A reader between the writes sees at worst a chain that ends one
block early (an entry pointing at Q before (A, Q) exists, or at Q before Q is stored): the same
truncated history it gets today for a pruned block. Pointers written by the forward index above
the tail are never changed. A live block with A updates the head of the chain
(`TransactionsIndex::update` takes the previous block from the wallet cache,
`transactionsindex.cpp:412-438`); the backfill updates the tail; both run on the transport's
processor thread, so they never interleave. API threads read through LMDB read transactions,
unchanged.

**Crash safety.** The index is written before the block, so a block in the storage is always
indexed unless the index lost committed writes (LMDB with `MDB_NOSYNC` after a power cut loses a
suffix of its transactions). At start, with `F = __backfill_floor__` and `first` = the storage's
first block: if `first < F`, the blocks `F − 1 .. first` are re-indexed from the storage with the
same four steps (idempotent: the tail record or the walk gives the right tail, step 3 keeps an
existing entry); if `F < first`, entries exist for blocks not stored: harmless, they are refreshed
when the blocks are fetched again. The index and hash databases are flushed before the storage at
every flush point (section 4), which makes `first < F` the rare case. A tail record left stale by
a crash (written after the entries) is corrected by step 1's validity check on the next block of
that wallet. `rebuildFrom` (the fallback) resets the whole database, tail records and floor
included; its result has no tails, which step 1 then derives by walking.

**Ranges not backfilled yet.** The API answers as a pruned node answers today: a history stops at
the lowest stored block of that wallet (#62); `TransactionsListGet` skips blocks it cannot load
(#46); `PoolInfoGet`, `PoolListGet`, `TransactionGet` report the block as not found; wallet
transaction counts are complete (wallet cache). As soon as a block is stored, every one of these
sees it.

**Cost.** One or a few LMDB writes per address per block, on the thread that stores the block:
most mainnet blocks are empty; a block with 100 transactions costs a few milliseconds.

### 7. Progress and completion

- **Visibility.** A log line every 100,000 blocks: lowest stored block, blocks per second, peers in
  use, estimated time left. One line at every peer mark, one at every pause and resume. The
  periodic status line gets `oldest stored = K`. No API change in this version.
- **At block 0:** `BACKFILL: full history stored, blocks 0..N`. Nothing else happens: the index and
  the hash databases already cover the chain, every API call already serves it, and the slow-start
  check accepts the database by itself (`storage.cpp:322-333` sees block 0). Conversion to
  berkeleydb is allowed from then on.

### 8. Docker, packaging, configuration **[changed]**

- **Configuration keys** (`[storage]`): `backfill` (`on`/`off`, default `on`; without effect on
  pruned nodes), `backfill_batch` (1..1000, default 100), `backfill_rate` (blocks per second,
  default 1000, 0 = unlimited).
- **RocksDB nodes and the snapshot [changed 2026-10-24].** Backward sync is native on RocksDB
  (section 4). What is LMDB is the published snapshot's block database. Three ways to start a
  RocksDB full node from it were weighed:
  - (a) restore the snapshot, then let the node convert its small block database to RocksDB once
    at its first start, with the existing conversion;
  - (b) publish a RocksDB snapshot next to the LMDB one;
  - (c) teach the restore to write the snapshot's blocks into the configured backend.

  (a) is chosen: it reuses the conversion that CI already tests in every direction, on a
  database of 100,000 to 200,000 blocks (seconds: the conversion to RocksDB runs at about
  38,000 blocks/s, `docs/storage.md`), and needs no second publishing pipeline (b: twice the
  release assets under the 2 GB limit, twice the checks) and no new restore code in the node (c).
  To need no command-line flag, the start-up check (`client/src/dbcheck.cpp:190-300`) converts
  without asking when `db/` holds an LMDB database, `config.ini` asks for `rocksdb` and
  `snapshot.json` lies next to `db/` (a restore writes it; the node never does), logging
  `the restored snapshot is converted to rocksdb before the start`. `--convert-db` keeps working
  as the explicit way. BerkeleyDB: a database whose first block is above 0 cannot be converted to
  it (`storage_convert.cpp:328-334`), so a BerkeleyDB full node still syncs from block 0, or is
  converted from an LMDB or RocksDB node once its backfill reached block 0. After the start a
  RocksDB node backfills like an LMDB one.
- **Entrypoint.** Pruned to full is allowed: `switching from pruned to full storage: the node
  keeps its blocks and fetches the older ones in the background (backfill)`. A full node with an
  empty data directory on lmdb or rocksdb restores the latest snapshot like a pruned one (the
  rocksdb case is then converted by the node as above) and backfills, instead of syncing from
  block 0. A full BerkeleyDB node still syncs from block 0. `CREDITS_SNAPSHOT=off` keeps the old
  behaviour.
- **Without Docker [new].** Backward sync is in the node, not in the image: a node run from the
  release package does the same. The release packages ship `snapshot-restore.sh` (today it is
  only in the image). A full node from a snapshot by hand:
  1. unpack the full package (`...-linux-x64-full.tar.gz`), `cd csnode`;
  2. `./snapshot-restore.sh . https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/latest.json`
     (needs `curl`, `zstd`, `tar`, `python3`): it checks the archive's SHA-256 and puts `db/`,
     `caches/`, `qs/`, the `.dat` files and `snapshot.json` in place;
  3. in `config.ini`, `[storage]` with `db_backend=lmdb` or `db_backend=rocksdb` and no
     `prune_keep_blocks` (the full package's default section);
  4. start the node. It quick-starts from the snapshot's checkpoint, syncs the last hours forward,
     and logs `BACKFILL: full node with blocks N..M: fetching 0..N-1 from peers in the background`;
  5. check by hand that the first `STATE DIGEST #<seq>` line of the log equals `state_digest` in
     `snapshot.json` (the entrypoint does this for Docker).
  Documented in `docs/storage.md` ("A full node from a snapshot") and the README.
- **Docs.** `docs/storage.md` (keys, "Full and pruned", "A full node from a snapshot"),
  `packaging/docker/README.md` (the storage table), `docs/differences.md`, `docs/CHANGES.md`.

### 9. Security **[changed]**

- **Forged chains.** A block is accepted only if its hash, computed from the received bytes,
  equals the previous hash of the verified block above it. The chain above is anchored by the
  snapshot's lowest block, whose integrity the snapshot's digest check covers at restore. A peer
  cannot make the node store a block the network did not produce.
- **Altered tails.** The pool signatures are outside the hash and are verified against the
  confidants inside it. The smart signatures are outside the hash and are not verified (section
  3): a peer could serve an old block with an altered or missing smart-signature tail, which the
  node would store and later serve. The forward path detects it only with the solver's state of
  that time. The user accepted this residual risk on 2026-10-24; a sample cross-check of the
  bytes from a second peer stays a later option, outside the plan.
- **Bounded memory.** Replies are accepted only for sequences of a batch in flight, from its peer;
  the buffer holds at most 8 batches; at most 4 requests are in flight. The decompressed reply is
  bounded as for forward sync (#4, #7).
- **Denial of service by peers.** A peer that answers garbage or nothing is marked and the batch
  goes to another; with no usable peer the backfill waits and logs. A peer cannot slow forward sync
  or consensus: backfill pauses when they need the node, and never touches the caches.
- **Load on the network.** `backfill_rate` caps what one node asks; the responders already serve
  150-block requests to syncing nodes. The anti-duplicate log of order 35, if active, is respected
  per peer by detection, never defeated.

### 10. Measurements **[new]**

- **Block size.** The published snapshot `credits-pruned-186460890` (local copy): the LMDB
  `blocks` table holds 161,891 blocks in 80,977 leaf + 285 branch + 2 overflow pages of 4 KB =
  333 MB, 2,056 bytes per recent block; the `seq_no` table adds 87 bytes per block. The first
  4.4 M blocks take 9.6 GB in BerkeleyDB (`docs/storage.md`), 2.2 KB per block.
- **Full database.** About 186.5 M blocks × 2.15 KB ≈ 400 GB in LMDB or BerkeleyDB; RocksDB
  compresses to about a third (9.6 GB → 3.1 GB on the first 4.4 M): about 130 GB. The download is
  the same 390 GB of block bytes, less the responder's compression.
- **Rates.** The local pruned node synced forward at about 260 blocks/s on mainnet (175.5 M to
  185.4 M in 10.5 h, 150-block requests to ~15 peers, blocks applied, `nice 19`, 2 CPUs). The
  LMDB conversion writes 6,400 blocks/s (`docs/storage.md`), so storage is not the limit at
  1000 blocks/s. Whole chain: about 2.2 days at 1000 blocks/s; about 13 days if every peer has
  order 35 active (6-s pacing, 10 peers).

### 11. Tests

**Unit tests** (`tests/csnode`): the backfill state machine with fake peers and a fake chain
(batch assignment, pacing with and without `AwaitAnswer`, timeouts, reassignment, out-of-order
replies stored strictly downwards, buffer bound, resume from `first`); block verification on
generated signed blocks (valid, altered signature, altered transaction, wrong sequence, genesis);
`pool_save_backfill` on LMDB and on the in-memory database of `storage_writer_tests.cpp`
(RocksDB when built); the hash databases' bottom-edge repair; the backward indexing of
`TransactionsIndex` on a temporary index (tails, pruned-lineage pointers, rebuilt-index pointers,
a wallet first seen late, idempotent re-indexing, the floor repair at start) and its concurrent
readers.

**Private network** (`robustness.yml`, all with matching state digests; `ci/testnet/run.sh` gets
`BACKFILL=1`, which makes the node joining from the snapshot a full node):

| Scenario | What happens | Must show |
| --- | --- | --- |
| `backfill-join` | n8 joins from a snapshot of n6 (taken after two prunings) as a full LMDB node | It reaches block 0; every block 0..T has the same hash on n8 and n1 (`compare_blocks.py` over the API); its wallet histories, read while it runs and without a restart, equal n1's; histories read in the middle of the backfill are n1's truncated at n8's lowest stored block, never wrong |
| `backfill-rocksdb` | The same, n8 configured for RocksDB: the node converts the restored snapshot at its first start | The same, and `block database: rocksdb` in its log |
| `backfill-berkeleydb` | n4 (pruned BerkeleyDB from block 0) switched to full | The same, on BerkeleyDB |
| `backfill-trusted` | `storage-switch` as today: n6, an initial confidant, pruned to full, backfills while trusted | It reaches block 0; the network's block rate does not drop; digests equal |
| `backfill-crash` | n8 killed three times during the backfill, once right after a flush and once with `__backfill_floor__` above its first stored block (test build: skip the index write of one block) | It resumes from its lowest block each time, re-indexes the gap at start; no hole (every block compares); histories equal n1's |
| `backfill-tamper` | One peer (test build) serves blocks with an altered signature tail, another with an altered transaction | Each is refused and logged, the blocks come from another peer, both peers are marked |
| `backfill-order35` | Special order 35 sets `syncroChangeRound` on every node | The joining node logs the paced peers, receives no `DuplicatedRequest` after the first, and finishes |
| `backfill-mixed` | `node_alt_ref` = a master without this code on the even nodes | The history comes from nodes without this code (the joining node runs the new binary) |

**Mainnet.** A Docker full node from the published snapshot, on this host next to the pruned
container, backfills for a day without slowing forward sync: rate, CPU, memory and disk growth
are recorded; balances and histories are compared with the monitor (`check_node.py balances`,
`history`) while it runs: balances always equal; histories equal the monitor's for wallets whose
transactions all lie in the stored range, and the node's truncated list is a suffix of the
monitor's for the others (`check_node.py history` gets a `--min-stored` option for that). One
restart is made to check the resume (no hole, floor repair). Then, optionally, the backfill is
left to finish and the database converted to berkeleydb to check the official binary opens it.

### 12. Open points for the user

Resolved since the first draft: the snapshot exists; the rate question is answered by detection
and `backfill_rate`; the Docker entrypoint change is designed (section 8); BerkeleyDB stays in,
as its own last step.

Decided by the user on 2026-10-24:
1. **No restart, everything dynamic.** Backfilled blocks are usable as soon as they are stored,
   index included (section 6); nothing waits for a restart, with or without Docker.
2. **`backfill_rate=1000`** by default.
3. **Smart-signature tails:** the residual risk is accepted; the sample cross-check is not in the
   plan (a later option).
4. **RocksDB:** backward sync is native on RocksDB; the restored LMDB snapshot is converted once
   by the node at its first start (section 8).
5. **Trusted nodes** keep backfilling; the `backfill-trusted` scenario measures it.

Remaining: none that blocks the plan. The mainnet test (plan, step 7) reports the measured rate
and whether mainnet peers have order 35 active.
