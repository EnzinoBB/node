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
- **Index rebuild after a quick start (#53, #54)** is what section 6 uses at block 0.
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
- **Non-empty blocks.** `TransactionsListGet` walks `previousNonEmpty_`
  (`apihandler.cpp:2760-2800`, `blockchain.cpp:439-452`), a map saved in every checkpoint
  (`blockchain_serializer.cpp:73-75`) and never pruned. Every published snapshot descends from the
  first one, whose node synced from block 0: the map of a snapshot node covers the whole chain, and
  the walk skips the blocks it cannot load (#46).
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
the wallet state of that time (`deepBlockValidation`). Residual risk and an optional sample
cross-check: section 9.

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
- **Durability.** `Storage::flush()` (LMDB `mdb_env_sync`, RocksDB `SyncWAL`) every 10,000
  backfilled blocks or 60 s, then the hash databases' flush (section 5), so that a power cut loses
  at most that window, as it may lose the forward blocks since the last checkpoint today.
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
- Nothing else is touched: the wallet caches, the transaction index, `previousNonEmpty_`,
  `lastNonEmptyBlock_` and the counters already describe the full history.

### 6. Transaction index

The index links each wallet's transactions by a pointer to its previous block. During the backfill
it is left as the snapshot brought it, and live blocks keep updating it as on any node:

- an API history follows the pointers down to the oldest block whose own entry exists, so it stops
  at the snapshot's range: truncated but correct, as on a pruned node (#62);
- at block 0 the node writes a key of its own in the index database (`__rebuild_requested__`),
  distinct from the incomplete flag that would keep the node from the Trusted role. The next start
  quick-starts and rebuilds the whole index from the stored blocks, through the path #54 added
  (`blockchain.cpp:143-153`, `256-262`), then clears the key.

Rejected: completing the index backwards with a persisted map of pending pointers (the first
draft). It kept a second, incremental way of building the index next to the forward one, with its
own crash cases, for a result the rebuild after a quick start already gives (about 15 minutes on
the whole chain by #54's measurement on the first 4.4 M blocks, likely more; the mainnet test
measures it).

`TransactionsListGet` needs no rebuild: `previousNonEmpty_` already covers the chain (above), and
the blocks it skipped as pruned are now loadable.

### 7. Progress and completion

- **Visibility.** A log line every 100,000 blocks: lowest stored block, blocks per second, peers in
  use, estimated time left. One line at every peer mark, one at every pause and resume. No API
  change in this version.
- **At block 0:** `BACKFILL: full history stored, blocks 0..N; the transaction index is rebuilt at
  the next start`. The node keeps running; the slow-start check accepts the database again by
  itself (`storage.cpp:322-333` sees block 0). Whether the node should restart on its own is an
  open point (section 12).

### 8. Docker, packaging, configuration **[changed]**

- **Configuration keys** (`[storage]`): `backfill` (`on`/`off`, default `on`; without effect on
  pruned nodes), `backfill_batch` (1..1000, default 100), `backfill_rate` (blocks per second,
  default 1000, 0 = unlimited).
- **Entrypoint.** Pruned to full is allowed: `switching from pruned to full storage: the node
  keeps its blocks and fetches the older ones in the background (backfill)`. A full LMDB node with
  an empty data directory restores the latest snapshot like a pruned one and backfills, instead
  of syncing from block 0. A full RocksDB node restores the snapshot and starts with
  `--convert-db` once, so that the node converts the small LMDB database to RocksDB (seconds) and
  backfills there. A full BerkeleyDB node still syncs from block 0: a pruned database cannot be
  converted to berkeleydb. `CREDITS_SNAPSHOT=off` keeps the old behaviour.
- **Docs.** `docs/storage.md` (keys, "Full and pruned"), `packaging/docker/README.md` (the
  storage table), `docs/differences.md`, `docs/CHANGES.md`.

### 9. Security **[changed]**

- **Forged chains.** A block is accepted only if its hash, computed from the received bytes,
  equals the previous hash of the verified block above it. The chain above is anchored by the
  snapshot's lowest block, whose integrity the snapshot's digest check covers at restore. A peer
  cannot make the node store a block the network did not produce.
- **Altered tails.** The pool signatures are outside the hash and are verified against the
  confidants inside it. The smart signatures are outside the hash and are not verified (section
  3): a peer could serve an old block with an altered or missing smart-signature tail, which the
  node would store and later serve. The forward path detects it only with the solver's state of
  that time. Mitigation offered as an optional step: request 1 batch in 50 from a second peer as
  well and compare the bytes; a difference marks both peers and takes the batch from a third.
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
(RocksDB when built); the hash databases' bottom-edge repair; the index rebuild key.

**Private network** (`robustness.yml`, all with matching state digests; `ci/testnet/run.sh` gets
`BACKFILL=1`, which makes the node joining from the snapshot a full node):

| Scenario | What happens | Must show |
| --- | --- | --- |
| `backfill-join` | n8 joins from a snapshot of n6 (taken after two prunings) as a full LMDB node | It reaches block 0; every block 0..T has the same hash on n8 and n1 (`compare_blocks.py` over the API); after a restart its wallet histories equal n1's |
| `backfill-rocksdb` | The same, n8 converts the snapshot to RocksDB at its first start | The same |
| `backfill-berkeleydb` | n4 (pruned BerkeleyDB from block 0) switched to full | The same, on BerkeleyDB |
| `backfill-trusted` | `storage-switch` as today: n6, an initial confidant, pruned to full, backfills while trusted | It reaches block 0; the network's block rate does not drop; digests equal |
| `backfill-crash` | n8 killed three times during the backfill | It resumes from its lowest block each time; no hole (every block compares) |
| `backfill-tamper` | One peer (test build) serves blocks with an altered signature tail, another with an altered transaction | Each is refused and logged, the blocks come from another peer, both peers are marked |
| `backfill-order35` | Special order 35 sets `syncroChangeRound` on every node | The joining node logs the paced peers, receives no `DuplicatedRequest` after the first, and finishes |
| `backfill-mixed` | `node_alt_ref` = a master without this code on the even nodes | The history comes from nodes without this code (the joining node runs the new binary) |

**Mainnet.** A Docker full node from the published snapshot, on this host next to the pruned
container, backfills for a day without slowing forward sync: rate, CPU, memory and disk growth
are recorded; balances and histories are compared with the monitor (`check_node.py balances`,
`history`) before and after the restart that rebuilds the index; the index rebuild time is
measured. Then, optionally, the backfill is left to finish and the database converted to
berkeleydb to check the official binary opens it.

### 12. Open points for the user

Resolved since the first draft: the snapshot exists; the rate question is answered by detection
and `backfill_rate`; the Docker entrypoint change is designed (section 8); BerkeleyDB stays in,
as its own last step.

Remaining:
1. **Restart at block 0.** Should the node restart itself when the backfill ends (Docker restarts
   the container, systemd needs `Restart=`), or only log that a restart rebuilds the index? The
   design logs and waits for the operator; the index stays truncated until then.
2. **Default rate.** `backfill_rate=1000` (about 2 MB/s, 2 days) or a gentler default such as 300
   (7 days)? The mainnet test may show the responders set the limit anyway.
3. **Smart-signature tails.** Accept the residual risk, or add the sample cross-check of section 9
   (second peer for 1 batch in 50)?
4. **Full RocksDB nodes in Docker.** Restore the LMDB snapshot and convert at the first start (the
   design), or keep RocksDB full nodes syncing from block 0?
5. **Trusted nodes.** Keep backfilling while trusted (the design), or pause in the rounds where the
   node is a confidant? The `backfill-trusted` scenario decides whether a pause is needed.
