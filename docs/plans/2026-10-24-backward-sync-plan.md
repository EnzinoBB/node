# Backward sync (backfill): implementation plan

For `backward-sync-design.md` (same folder), against master b5f74ecd (4.2.532-community.11).
Each step is one pull request, in this order; every step leaves master releasable with the
feature off or absent. File:line references are on b5f74ecd. "Not consensus-relevant" holds for
every step: no block, hash, signature rule or round behaviour changes; backfilled blocks are never
applied to the caches.

Conventions: log prefix `BACKFILL: `; new files under `csnode/include/csnode/` and `csnode/src/`;
unit tests in `tests/csnode/` (GoogleTest, `tests/csnode/CMakeLists.txt` globs `*.cpp`); plain
log texts, no exclamation marks; `WithDelimiters` for sequences.

## Step 1. `csdb`: store a block below the first one (small)

**Files.** `csdb/include/csdb/storage.hpp`, `csdb/src/storage.cpp`,
`tests/csnode/storage_backfill_tests.cpp` (new).

**Change.** `bool Storage::pool_save_backfill(const Pool& pool)`, next to `pool_save`
(`storage.cpp:730-791`):

```cpp
// Backward sync: stores a block below the first stored one. The chain's bookkeeping (last hash,
// count, pool cache, write queue) is about the head and is left alone; the caller stores strictly
// downwards and keeps the first stored sequence itself.
bool Storage::pool_save_backfill(const Pool& pool);
```

Rules: `NotOpen` and `InvalidParameter` as `pool_save`; refuse a block whose hash
(`d->db->get(hash)`) or sequence (`d->db->get(uint32_t(seq))`) is already stored
(`InvalidParameter`, "already stored"); write with `d->db->put(hash.to_binary(),
static_cast<uint32_t>(pool.sequence()), pool.to_binary())` on every backend (not through
`write_queue`, `storage.cpp:761-777`: that queue and `in_flight` describe the head); on failure
`DatabaseError` with the backend's message, return false, nothing counted. Do not touch
`last_hash`, `count_pool` (a quick start sets it to the head's sequence, `storage.cpp:311`; it is
not the number of stored blocks) or `pools_cache`. `Storage::flush()` (`storage.cpp:685`) stays
the way to make the writes durable.

`to_binary()` of a block parsed from the network returns the received bytes
(`Pool::update_binary_representation`), so the stored bytes are the peer's bytes once verified.

**Tests.** On `DatabaseLmdb` in a temporary folder and on the `MemoryDb` of
`tests/csnode/storage_writer_tests.cpp:22-114` (move it to a shared header
`tests/csnode/memorydb.hpp`): a storage opened on blocks 50..60 (built with
`csdb::Pool(prev, seq, storage)` + `compose()` as `pool_tests.cpp` does) stores 49, then 48;
`first_sequence()` follows; `size()` and `last_hash()` unchanged; `pool_load(49)` returns the
block; storing 49 again fails with `InvalidParameter`; `pool_load(60)` still works; on the
async (RocksDB-style) `MemoryDb` storage the block is in the database at once, not queued
(`write_queue_search` false). With RocksDB built (`CSDB_USE_ROCKSDB`): the same on a temporary
RocksDB.

## Step 2. `csnode`: BlockChain support, online indexing (medium)

**Files.** `csnode/include/csnode/blockchain.hpp`, `csnode/src/blockchain.cpp`,
`csnode/include/csnode/blockhashes.hpp`, `csnode/src/blockhashes.cpp`,
`csnode/include/csnode/transactionsindex.hpp`, `csnode/src/transactionsindex.cpp`,
`tests/csnode/blockhashes_tests.cpp`, `tests/csnode/transactionsindex_backfill_tests.cpp` (new).

**BlockChain.**

```cpp
// Backward sync (docs/specs/.../backward-sync-design.md): the block below the first stored one is
// expected to have this hash. kWrongSequence / empty when the storage is full from block 0.
struct BackfillAnchor { cs::Sequence first; csdb::PoolHash prevHash; };
BackfillAnchor getBackfillAnchor() const;          // under dbLock_: pool_load(first).previous_hash()
// stores a verified block first - 1: transaction index, hash databases, non-empty map, storage,
// firstStoredSeq_, in that order; false if not stored (the index and the hashes then hold the
// block ahead of the storage, which the start-up repair and the next attempt both tolerate)
bool storeBackfilledBlock(const csdb::Pool& pool);
// index flush, hash databases' flush, then Storage::flush(), in that order
bool flushBackfill();
```

`storeBackfilledBlock` (under `dbLock_`, as `pruneStep`, `blockchain.cpp:330`): check
`pool.sequence() + 1 == firstStoredSeq_` and `pool.hash() == anchor.prevHash` once more (cheap,
guards a caller bug); `trxIndex_->indexBackfilledBlock(pool)` (below); `blockHashes_->update(pool)`
(`blockhashes.cpp:35-54`); `mergeNonEmptyBlock(pool)` (design, section 5: `previousNonEmpty_`
lower_bound merge, a private helper next to `updateNonEmptyBlocks`, `blockchain.cpp:439-452`);
then `storage_.pool_save_backfill(pool)`; on failure return false (nothing to undo: both caches
tolerate an entry ahead of the storage); on success `firstStoredSeq_ = pool.sequence()`; if
`pool.sequence() == 1`, `uuid_ = uuidFromBlock(pool)` (as `blockchain.cpp:1637`); log at debug.
Wrap the storage call in the same try/catch as `saveDeferredBlock` (`blockchain.cpp:1263-1275`):
an engine that needs recovery stops the node.

Start-up (`BlockChain::init`, after `firstStoredSeq_` is known, `blockchain.cpp:225-230`, and
before the `consistentWith` check at `blockchain.cpp:266-279`): when `firstStoredSeq_ > 0`,
`trxIndex_->repairBackfillFloor(first, load)` and `blockHashes_->repairBottom(first, last, load)`
(below), then the existing checks. Both are bounded by the blocks a crash left unindexed and log
what they did.

**TransactionsIndex: backward indexing (the design's section 6).**

```cpp
// Backward sync: indexes a block below every indexed one, linking it under each of its wallets'
// lowest indexed block (the tail). Idempotent: a block may be indexed again after a crash.
void indexBackfilledBlock(const csdb::Pool& pool);
// the lowest block the backfill indexed (key __backfill_floor__), kWrongSequence if none
Sequence backfillFloor() const;
// at start: re-indexes the stored blocks floor - 1 .. first that a crash left unindexed
// (first < floor); returns how many
size_t repairBackfillFloor(Sequence first, const std::function<csdb::Pool(Sequence)>& load);
```

Keys, all in the one table of `caches/indexdb` (`transactionsindex.cpp:28-35`): entries stay
`pubkey(32) ‖ seq(8)`; a tail record is `'t' ‖ pubkey(32)` (33 bytes) with an 8-byte sequence
value; the floor is the string key `__backfill_floor__` next to `__last_indexed__`,
`__incomplete__`, `__walked_complete__` (`transactionsindex.cpp:20-22`). `hasIndexEntries()` and
`looksEmpty()` must not count tail records as entries (check the key length).

`indexBackfilledBlock(pool)`, for each address of the block once (source and target, mapped to
the public key as `updateFromNextBlock` does, `transactionsindex.cpp:412-438`):

```
tail = tailOf(key):
    rec = value of 't'‖key if present
    if rec present and entry (key, rec) exists: tail = rec
    else: tail = walkTail(key)   // below; kWrongSequence when the wallet has no entry
if tail != kWrongSequence and value of (key, tail) != seq:  set (key, tail) -> seq
if (key, seq) absent:                                      insert (key, seq) -> kWrongSequence
if tail != seq:                                            insert 't'‖key -> seq
after all addresses:                                       insert __backfill_floor__ -> seq
```

`walkTail(key)`: `cur = bc_.getLastTransaction(address).pool_seq()` (the wallet cache, as
`TransactionsIterator` starts, `transactionsiterator.cpp:8-13`); if invalid or no entry
(key, cur): `kWrongSequence`; loop: `prev = getPrevTransBlock(key, cur)`; if `prev ==
kWrongSequence` or `prev < bc_.getFirstStoredSequence()` or no entry (key, prev): return `cur`;
else `cur = prev`. One LMDB read per step, no block loads. Count the steps in a debug line.

`repairBackfillFloor(first, load)`: `F = backfillFloor()`; if `F == kWrongSequence || first >= F`
return 0; for `seq = F - 1 .. first`: `indexBackfilledBlock(load(seq))` (skip an invalid pool);
log `trxIndex: re-indexed N backfilled blocks F-1..first left unindexed by a crash`.

`rebuildFrom` (`transactionsindex.cpp:237-294`) already resets the database (tail records and
floor go with it); after it, `tailOf` derives tails by walking. `onPruneBlock`
(`transactionsindex.cpp:460-476`) stays as it is: tail records of a node that prunes become
stale and `tailOf` rejects them (no entry at `rec`). `trimToFloor` (`--set-bc-top`) is not
affected: it deletes entries above a floor; tail records (33-byte keys) must be skipped by its
scan (check the key length there, `transactionsindex.cpp:326-360`).

Thread model: `indexBackfilledBlock` runs on the transport's processor thread inside
`storeBackfilledBlock`, as `update` runs inside `applyBlockToCaches` (`blockchain.cpp:964`); API
readers use LMDB read transactions and may see a chain one block short for a moment (the design,
section 6). No lock is added.

**BlockHashes.**

```cpp
// mdb_env_sync of both databases (cs::Lmdb::flush)
void flush();
// Backward sync stores blocks downwards and writes these databases before the storage: after a
// crash they may hold entries below the first stored block, or miss some just above it. Removes
// the former, inserts the latter from the stored blocks; both walks stop at the first entry that
// needs nothing. Returns how many entries changed.
size_t repairBottom(cs::Sequence first, cs::Sequence last, const std::function<csdb::Pool(cs::Sequence)>& load);
```

The fallback for a damaged index is unchanged: the rebuild after a quick start
(`blockchain.cpp:143-153`, `256-262`). Nothing sets the incomplete flag (`isReady()`,
`transactionsindex.cpp:85-89`, gates the Trusted role).

**Tests.** `repairBottom`: databases holding 10..20 with storage 12..20 → entries 10, 11 removed;
databases holding 14..20 with storage 12..20 → 12, 13 inserted; nothing to do → 0; mixed.

Index (`transactionsindex_backfill_tests.cpp`, on a `TransactionsIndex` over a temporary folder
and a minimal `BlockChain` whose wallet cache the test fills through `storeBlock`-free means: the
existing tests construct `BlockChain(genesis, start)` without a database, `blockchain_tests.cpp:
42-46`; `getLastTransaction` and `getFirstStoredSequence` are what the index needs, so either
construct blocks through `recordBlock` on a temporary LMDB storage, or give `TransactionsIndex`
two injectable callbacks for them in the test build; the implementer picks the lighter one):

1. Pruned lineage: wallet A in blocks 100, 150 (both "pruned": entries removed by `onPruneBlock`,
   first stored = 180) and 200, 250 (indexed forward). Backfill 179..100 in order: after 150,
   (A,200) → 150 unchanged, (A,150) → none, tail(A) = 150; after 100, (A,150) → 100, (A,100) →
   none; `getPrevTransBlock` chain from 250 reads 200, 150, 100, none.
2. Rebuilt index: the same wallet, index built with `rebuildFrom(180, 250)` ((A,200) → none);
   after 150: (A,200) → 150.
3. A wallet first seen late: B only in block 120, last transaction in the wallet cache = 120:
   after 120, (B,120) → none and tail(B) = 120; a history walk starts at 120 and finds it.
4. Idempotence: index block 150 twice, and index 100 then 150 again (a crash scenario): the chain
   is the same as in case 1 and no entry points to itself.
5. Stale tail record: tail(A) = 150 but the entry (A,150) was removed (as pruning does): indexing
   130 walks and finds the real tail.
6. `repairBackfillFloor`: floor = 150, storage first = 120 with blocks 120..149 holding A and C:
   returns 30 and the chains are complete.
7. Readers: an API-style walk (`getPrevTransBlock` in a loop from another thread) during 10,000
   backfilled blocks never reads an entry pointing to itself and never throws; lengths are
   monotonic non-decreasing per wallet between walks.
8. `hasIndexEntries`/`looksEmpty` ignore tail records; `trimToFloor` leaves them alone.

`storeBackfilledBlock` on a `BlockChain` opened on a temporary LMDB folder with blocks 50..60 and
a checkpoint is heavier than the existing `blockchain_tests.cpp`; cover it in step 3's node-level
test and in CI instead.

## Step 3. The backfill itself: `cs::BackwardSync` and the Node wiring (large)

**Files.** `csnode/include/csnode/backwardsync.hpp`, `csnode/src/backwardsync.cpp` (new);
`csnode/include/csnode/node.hpp`, `csnode/src/node.cpp`; `client/config/config.hpp`,
`client/config/config.cpp`; `tests/csnode/backwardsync_tests.cpp` (new).

### 3a. Configuration

`StorageData` (`config.hpp:62-72`): `bool backfill = true; size_t backfillBatch = 100; size_t
backfillRate = 1000;` read in `Config::readStorageData` (`config.cpp:952-999`) as `backfill`
(`on`/`off`/`true`/`false`), `backfill_batch` (clamped to 1..1000 with a warning),
`backfill_rate` (0 = unlimited). Include them in the `StorageData` comparison
(`config.cpp:1142`-area) so a reload notices them.

### 3b. The class

Single-threaded by construction: every call comes from the transport's processor thread
(`transport.cpp:252-283`, which also runs `CallsQueue`), like `PoolSynchronizer`. No locks of
its own; the storage calls it makes take `dbLock_` inside `BlockChain`.

```cpp
namespace cs {
class BackwardSync {
public:
    struct Peer { PublicKey key; Sequence lastSeq; };
    struct Context {                 // what the node knows at this tick
        std::vector<Peer> peers;     // Transport::forEachNeighbour
        bool forwardSyncRunning;     // PoolSynchronizer::isSyncroStarted()
        bool bootstrapRound;         // Node::isBootstrapRound_
        RoundNumber round;           // Conveyer::currentRoundNumber()
        Sequence lastSeq;            // BlockChain::getLastSeq()
    };
    using SendRequest = std::function<void(const PublicKey&, const PoolsRequestedSequences&)>;
    using Clock = std::function<uint64_t()>;   // ms, injectable for tests

    BackwardSync(BlockChain& chain, SendRequest send, Clock clock = defaultClock);
    void tick(const Context& ctx);                               // every 500 ms
    void onBlocks(const PublicKey& sender, PoolsBlock&& blocks);  // a RequestedBlock reply
    void onSyncroMessage(const PublicKey& sender, SyncroMessage msg);
    bool hasRequestTo(const PublicKey& peer) const;               // Node::getBlockReply routing
    bool isActive() const;                                        // first > 0 and enabled
    void stop();
    struct Stats { Sequence first; double blocksPerSecond; size_t peersInUse; size_t stored; size_t rejected; ... };
    Stats stats() const;
};
}
```

**Data structures.**

```cpp
struct Batch {
    Sequence lo, hi;                   // inclusive, hi < first at creation
    PublicKey peer;
    uint64_t sentAt;
    std::set<Sequence> missing;        // sequences not yet received
};
struct PeerState {
    uint64_t nextRequestAt = 0;        // pacing and cooldowns
    bool paced = false;                // AwaitAnswer seen: 6 s between batches (order 35 active)
    uint64_t markedUntil = 0;          // 1 h after a failure; forever after NoSuchBlocks-after-AwaitAnswer
    unsigned failures = 0;
    bool awaitingAnswer = false;       // AwaitAnswer received for the batch in flight
};
std::map<Sequence, Batch> inFlight_;               // by lo
std::unordered_map<PublicKey, PeerState> peers_;
std::map<Sequence, csdb::Pool> buffer_;            // received, not yet stored; ≤ 8 × batch
Sequence next_;                                     // first - 1 (from the anchor)
csdb::PoolHash expectedHash_;                       // anchor.prevHash
std::optional<csdb::Pool> lastStored_;              // for the one-block-late confirmations check
TokenBucket rate_;                                  // backfill_rate blocks/s, burst = 4 batches
uint64_t lastFlushAt_; size_t storedSinceFlush_;    // flush every 10 000 blocks or 60 s
```

**`tick(ctx)`:**
1. If not active (storage full from 0, `backfill=off`, or stopped): return. On the first active
   tick log `BACKFILL: full node with blocks N..M: fetching 0..N-1 from peers in the background`.
2. `drain()` (below) in case blocks wait.
3. Expire batches: a batch older than 30 s is reassigned (its `missing` set kept; the peer gets
   `nextRequestAt = now + 60 s`, `failures++`, marked 1 h at 2 failures).
4. Flush when due: `chain.flushBackfill()`.
5. Allowed to request? `!ctx.forwardSyncRunning && !ctx.bootstrapRound && ctx.lastSeq + 3 >=
   ctx.round`. If not, log the pause once (and the resume later) and return.
6. While `inFlight_.size() < 4` and the token bucket has `backfillBatch` tokens: pick the next range
   (`hi` = lowest `lo` in flight − 1, or `next_`; `lo = max(0, hi − batch + 1)`; stop at `hi` below
   0), pick a peer: not marked, `nextRequestAt <= now`, no batch in flight, `lastSeq >= hi`,
   round-robin over the candidates. None: return. Send `[lo..hi]` ascending; record the batch;
   `peer.nextRequestAt = now + (paced ? 6000 : 200)`.
7. Every 100,000 stored blocks: `BACKFILL: stored down to #N, R blocks/s over the last 100,000,
   P peers, about H h left`.

**`onBlocks(sender, blocks)`:** find the sender's batch; if none, drop silently (count). For each
block: `is_valid()`, sequence in `[lo, hi]` and in `missing`, else drop; otherwise `buffer_[seq] =
block`, erase from `missing`. If the buffer exceeds 8 × batch, drop the batch's remaining blocks
and log once. If `missing` is empty, erase the batch and set `peer.nextRequestAt = now + (paced ?
6000 : 200)`. Then `drain()`.

**`onSyncroMessage(sender, msg)`:** `AwaitAnswer`: `paced = true`, `awaitingAnswer = true`, log
once per peer `peer X answers with AwaitAnswer: one batch every 6 s`. `NoSuchBlocks`: if
`awaitingAnswer`, mark for ever (`markedUntil = max`), log `peer X has no blocks below N and keeps
our request in its log: not used again`; else mark 1 h; reassign its batch. `DuplicatedRequest`:
`paced = true`, `nextRequestAt = now + 10 s`, reassign its batch, log. `IncorrectRequest`: mark 1 h.
`Sent`: nothing.

**`drain()`:** while `buffer_` holds `next_`: take the block, `verify(block)`; on success
`chain.storeBackfilledBlock(block)`; on a storage failure (full disk) keep the block in the buffer,
set `retryAt = now + 60 s`, return; on success `confirmationsCheck(lastStored_, block)`,
`lastStored_ = block`, `expectedHash_ = block.previous_hash()`, `--next_`, counters; at block 0:
`chain.flushBackfill()`, log `BACKFILL: full history stored, blocks 0..M`, deactivate (nothing
else: the index, the hash databases and every API call already cover the chain, the design's
sections 6 and 7). On a verification failure: drop every buffered
block of that batch's range, mark the peer 1 h (`failures++`), reassign the batch, log
`BACKFILL: block N from peer X rejected (<reason>): batch requested again`.

**`verify(block)` (the exact rules, section 3 of the design):**

```
1. block.is_valid() && block.sequence() == next_                      else "sequence"
2. block.hash() == expectedHash_                                      else "hash"
   (hash() is computed from the received bytes; hashingLength_ was checked at parse time, #5)
3. if next_ >= 1:
     mask = cs::Utils::bitsToMask(block.numberTrusted(), block.realTrusted())
     NodeUtils::checkGroupSignature(block.confidants(), mask, block.signatures(), hash)
                                                                       else "signatures"
```

`confirmationsCheck(upper, lower)` (upper = block k just stored before, lower = k − 1): for
`k > 1`, unless `BlockChain::isBootstrap(upper)` or the mask
`bitsToMask(upper.numberConfirmations(), upper.roundConfirmationMask())` has at most one entry:
`trustedHash = calculateHash(seq k ‖ upper.confidants())` as `finalizeBlock` builds it
(`blockchain.cpp:881-888`), `checkGroupSignature(lower.confidants(), mask,
upper.roundConfirmations(), trustedHash)`; on failure `cserror() << "BACKFILL: the round
confirmations of block " << k << " do not verify against the confidants of block " << k-1`
and a counter; never a rejection (the data is inside the hash, so it is the chain's, not the
peer's). The genesis block (0) and block 1 need no confirmations; block 0 needs no signatures.

**Resume.** The constructor reads nothing: `tick` takes `chain.getBackfillAnchor()` when it
starts and whenever `inFlight_` and `buffer_` are empty, so a restart resumes at
`first_sequence() − 1` with no watermark.

### 3c. Node wiring (`node.cpp`)

- Member `std::unique_ptr<cs::BackwardSync> backwardSync_`, created in `Node::init` after
  `initPoolSynchronizer` (`node.cpp:295-299`) with `SendRequest` = `Node::sendBackfillRequest`:
  the body of `sendBlockRequest` (`node.cpp:2148-2156`) without `status_ = Synchronization`
  (factor the packet building into one helper).
- A `cs::Timer backfillTimer_` started at 500 ms with `RunPolicy::CallQueuePolicy` (as
  `poolsynchronizer.cpp:101-105`) whose slot builds the `Context` (`transport_->forEachNeighbour`,
  `poolSynchronizer_->isSyncroStarted()`, `isBootstrapRound_`, `currentRoundNumber()`,
  `getLastSeq()`) and calls `backwardSync_->tick`.
- `Node::getBlockReply` (`node.cpp:1890-1937`): the early return at 1896 becomes
  `if (!isSyncOn && !isBlockchainUncertain && !backwardSync_->hasRequestTo(sender)) return;`
  After decompression and the necessary-block branch (1918-1921): `if
  (backwardSync_->hasRequestTo(sender) && poolsBlock.front().sequence() <
  blockChain_.getFirstStoredSequence()) { backwardSync_->onBlocks(sender, std::move(poolsBlock));
  return; }`.
- `Node::getSyncroMessage` (`node.cpp:1706-1715`): parse first, call
  `backwardSync_->onSyncroMessage(sender, msg)` always, then the existing gate and
  `poolSynchronizer_->getSyncroMessage`. (A peer sends these only when its own order-35 round has
  come; our gate is about our knowledge of it, and the backfill needs the message in any case.)
- `Node::stop` (`node.cpp:340-351`): `backwardSync_->stop()` before `poolSynchronizer_->stop()`;
  `blockChain_.close()` (`node.cpp:363`) already flushes the storage.
- The periodic status line (`node.cpp:3993`) gets `, oldest stored = K` while the backfill is
  active, so `ci/testnet/run.sh` can read the progress.

### 3d. Unit tests (`backwardsync_tests.cpp`)

A fake chain implementing the three `BlockChain` calls (anchor, store, flush) over a vector of
generated blocks: blocks 0..300 built with `csdb::Pool`, confidants = 3 generated key pairs
(`cscrypto::generateKeyPair`), `set_confidants`, `numberTrusted`/`realTrusted` mask, signed with
`cscrypto::generateSignature` over `hash().to_binary()` (as `tests/csnode/pool_tests.cpp` and
`blocktakeover_tests.cpp` build signed pools), serialized with `to_binary()` and parsed back with
`from_binary` so that the hashed bytes are what a peer would send. The fake chain starts at
`first = 201`. The `SendRequest` callback records requests; the test feeds replies.

Cases: (1) four batches go to four distinct peers, ascending inside, consecutive downwards;
(2) replies out of order (batch 2 before batch 1) are stored strictly downwards, `first` reaches 0,
the completion line logged once; (3) a block with one altered signature byte is rejected
("signatures"), its peer marked, the batch resent to another peer; an altered transaction byte
("hash"); a block with the wrong sequence; (4) a peer that never answers: reassignment after 30 s,
cooldown 60 s; (5) `AwaitAnswer` sets 6-s pacing for that peer only, the others keep 200 ms;
`DuplicatedRequest` delays 10 s; `NoSuchBlocks` after `AwaitAnswer` marks for ever; (6) the buffer
bound: a flood of unsolicited blocks is dropped, a flood for a real batch is capped; (7) pause:
`forwardSyncRunning` stops new requests but replies are still stored; (8) resume: a new
`BackwardSync` on the same fake chain continues at `first − 1`; (9) rate: with
`backfill_rate = 200` no more than 200 blocks are requested per simulated second; (10) the
confirmations check: a block whose confirmations do not match its predecessor's confidants is
stored and counted, not rejected; (11) genesis: block 0 without signatures is accepted when its
hash matches.

**Pass criteria for step 3.** All unit tests; `testnet.yml` green (no behaviour change on
pruned nodes and on full nodes from block 0: `isActive()` false); the `storage-switch`
robustness scenario now shows n6 backfilling to 0 (its end check changes in step 5; until then
the scenario is expected to fail on "oldest block after the switch" and is run with the step-5
branch).

## Step 4. Start from a snapshot on any backend, packaging, Docker, documentation (medium)

**Files.** `client/src/dbcheck.cpp`, `tests/csnode/dbcheck_tests.cpp`, `.github/workflows/release.yml`,
`packaging/docker/entrypoint.sh`, `packaging/docker/README.md`,
`packaging/docker/test/test-entrypoint.sh`, `docs/storage.md`, `docs/differences.md`,
`docs/CHANGES.md`, `README.md`, `docs/specs/2026-10-17-backward-sync-design.md` and
`docs/plans/2026-10-24-backward-sync-plan.md` (already on `docs/backward-sync`).

**RocksDB from the LMDB snapshot (the design's section 8, option a).** In the start-up check
(`client/src/dbcheck.cpp`: `Facts`, `decide` at 190, `checkImpl` at 211), a new fact
`restoredSnapshot` = `snapshot.json` exists in the working directory (the parent of `--db-path`,
where `snapshot-restore.sh` puts it; the node never writes that file). When the database in
`db/` is lmdb, the configured backend is rocksdb and `restoredSnapshot` holds, `decide` answers
convert-without-asking, with the log line `the restored snapshot is converted to rocksdb before
the start` (the same path as `--convert-db`, `convertNow` at 120). Any other combination keeps
today's rules; to berkeleydb the conversion itself refuses a first block above 0
(`storage_convert.cpp:328-334`), and the message gains `a snapshot cannot start a berkeleydb
node: use lmdb or rocksdb, or sync from block 0`. Unit test in `dbcheck_tests.cpp` on `decide`.

**Packages.** `release.yml` adds `packaging/docker/snapshot-restore.sh` to both packages next to
the node binary (it needs only `curl`, `zstd`, `tar`, `python3`); the full package's
`config.ini` keeps its `[storage]` section with `db_backend` only.

**Entrypoint.**
- `entrypoint.sh:123-125`: replace the refusal by `log "switching from pruned to full storage:
  the node keeps its blocks and fetches the older ones in the background (backfill)"`.
- Snapshot restore (`entrypoint.sh:130-141`): also when `cfg_mode = full` and `cfg_backend` is
  `lmdb` or `rocksdb` (the node converts the rocksdb case by itself, above; no `--convert-db`
  from the entrypoint); `berkeleydb` keeps syncing from block 0 (`log "snapshots hold an LMDB
  database that cannot become berkeleydb: syncing from block 0"`). The digest check after a
  restore (`entrypoint.sh:157-192`) applies unchanged (the start-up digest is the checkpoint's,
  whatever the backend).
- `test-entrypoint.sh:158-163` ("pruned to full: refused") becomes "pruned to full: allowed, logs
  the backfill line"; new cases: full+lmdb and full+rocksdb first starts restore the snapshot
  (the stub node sees `snapshot.json`); full+berkeleydb does not restore.

**Docs.** `storage.md`: the three keys in the settings table; "Full and pruned": going from pruned
to full keeps the database and the node fetches the older blocks in the background, from the
lowest stored block down to 0, in the order of the chain, usable as they arrive (histories grow
downwards, nothing to restart); what the log shows; disk space needed (about 400 GB
LMDB/BerkeleyDB, 130 GB RocksDB for the whole chain today, section 10 of the design);
`berkeleydb` conversion allowed once block 0 is stored. A new section "A full node from a
snapshot" with the five steps of the design's section 8 (without Docker: unpack, restore, set
`db_backend`, start, compare the first `STATE DIGEST` with `snapshot.json`), and the rocksdb
conversion at the first start. `docker/README.md`: the storage table ("Pruned to full: allowed;
the node backfills") and the snapshot paragraph (full nodes on lmdb and rocksdb start from the
snapshot too). `README.md`: one line pointing to that section. `differences.md`: a paragraph
under "New options". `CHANGES.md`: the PR lines.

## Step 5. CI: private-network scenarios (medium)

**Files.** `ci/testnet/run.sh`, `ci/testnet/compare_blocks.py` (new),
`.github/workflows/robustness.yml`, `csnode/src/node.cpp` (test-build hook, `#ifdef
CREDITS_TESTNET`).

**`run.sh`.** New parameters:
- `BACKFILL=1` (needs `SNAPSHOT_AT`): the joining node (`JOIN_NODE = NODES + 1`) restores the
  snapshot as a **full** node: `storage_section` writes `db_backend=lmdb` (or `BACKFILL_BACKEND`:
  `rocksdb`, converted by the node at its first start because `snapshot.json` is there, step 4)
  and no `prune_keep_blocks`; its API is opened (`api_settings`, port 9100 + node) for the
  comparisons. The node is started with `BIN`, never `BIN_ALT` (the mixed scenario needs the new
  binary there; today `start_node` picks `BIN_ALT` for even nodes, `run.sh:391-395`).
- `BACKFILL_CRASH=N`: the joining node is killed with SIGKILL N times while its oldest stored
  block is above 0 (read from the status line of step 3c), 20 s apart, and restarted; the second
  kill lands within a second of a `BACKFILL: flushed` log line, and before the third the test
  build skips the index writes of one block (`./testnet-skip-index-once`, read in
  `storeBackfilledBlock`), so that the floor is above the first stored block at the restart.
- `BACKFILL_TAMPER=<node>,<kind>`: a test build reads `./testnet-tamper-replies` (`kind` =
  `signature`: the first signature byte of every served block below `TAMPER_BELOW` flipped;
  `transaction`: a byte of the first transaction's amount) in `Node::sendBlockReply`, as
  `testnet-fail-apply` is read in `applyBlockToCaches` (`blockchain.cpp:972-981`). Two nodes
  tamper, in both ways.
- `BACKFILL_ORDER35`: `SYNCRO_ROUND=100` as `pruned-peers-join` does (`robustness.yml:200-203`).

End checks (after the existing ones, `run.sh:1104-1117`):
- the joining node logged `BACKFILL: full history stored` and its last status line says
  `oldest stored = 0`;
- `compare_blocks.py --a 127.0.0.1:<api of n1> --b 127.0.0.1:<api of join> --from 0 --to <T>`:
  `PoolInfoGet(seq, 0)` on both for every sequence (hash, prevHash, transactionsCount, confidants,
  realTrusted); any difference or a block not found on one side fails. T = the lowest common
  sequence at the end. 3,000 blocks × 2 calls take under a minute on the loopback;
- histories, with no restart (`compare_histories.py`, in the same file as `compare_blocks.py`):
  `TransactionsGet(address, 0, 100)` for every funded key (`signers.py` keys) and for the master key
  on n1 and on the joining node, as lists of (poolSeq, index): once in the middle of the backfill
  (when the joining node's oldest stored block is still above the first funding block), the joining
  node's list must equal n1's list restricted to `poolSeq >= oldest stored`, and
  `WalletTransactionsCountGet` must already be equal (wallet cache); at the end, equal lists. The
  joining node's log must have no `Inconsistent transaction index` and no `trxIndex: rebuilding`
  line (the online index is used, not the fallback);
- `BACKFILL_CRASH`: after the restart that follows the skipped index write, the log has
  `trxIndex: re-indexed 1 backfilled blocks`; the histories check at the end passes;
- `BACKFILL_TAMPER`: the joining node logged `rejected (signatures)` and `rejected (hash)` at
  least once each, and `peer ... marked`; the tampering nodes' blocks are nevertheless all present
  (they came from others);
- `BACKFILL_ORDER35`: the joining node logged `answers with AwaitAnswer` for at least one peer and
  no more than one `DuplicatedRequest` per peer;
- `BACKFILL_CRASH`: N kills happened before block 0, each restart logged
  `fetching 0..` with a lower bound than the previous one, and the comparison passes (no hole).
- `storage-switch` (`run.sh:1119-1130`): n6 must now reach `oldest stored = 0` and log the
  completion; the old check "oldest block after the switch == pruned_before" is replaced; the
  block rate of the network over the backfill window (from `resources.tsv`, #68) must stay within
  20 % of the rate before it.

**`robustness.yml` matrix** (names as in the design, section 11):

```
backfill-join:        DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=2300 BACKFILL=1 TARGET_SEQ=3300
backfill-rocksdb:     DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=2300 BACKFILL=1 BACKFILL_BACKEND=rocksdb TARGET_SEQ=3300
backfill-crash:       DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=2300 BACKFILL=1 BACKFILL_CRASH=3 TARGET_SEQ=3600
backfill-tamper:      DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=2300 BACKFILL=1 BACKFILL_TAMPER=2:signature,3:transaction TARGET_SEQ=3300
backfill-order35:     DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=2300 BACKFILL=1 SYNCRO_ROUND=100 TARGET_SEQ=3300
backfill-trusted:     = storage-switch (renamed; n6 is an initial confidant)
backfill-mixed:       backfill-join with node_alt_ref = the last master commit before step 3 (run by name, fixed: true)
```

Pass criteria: the scenario's own checks above plus the suite's (equal state digests, target
sequence reached, no core dump). Expected durations: the backfill of ~2,300 blocks at 100 per
batch over 7 peers takes under a minute without order 35, about 3 minutes with it.

## Step 6. BerkeleyDB (small, after step 5)

No code expected: `pool_save_backfill` uses `put`, which fills the deleted record `seq + 1`
(`database_berkeleydb.cpp:316-331`). The scenario proves it: `backfill-berkeleydb` =
`all-storage`'s layout (`robustness.yml:258-260`, n4 `berkeleydb:pruned` from block 0) with a
`STORAGE_SWITCH_NODE=4` generalisation of the storage switch (`run.sh:691-703` is fixed to n5/n6
and `DB_BACKEND=lmdb`): at `STORAGE_SWITCH_AT`, n4 is restarted with `prune_keep_blocks=0` and
must reach `oldest stored = 0`; the comparison of step 5 runs between n4 and n1. If BerkeleyDB
refuses a put (it should not), the design's BerkeleyDB paragraph is revisited before this step is
merged. Also a unit test on `DatabaseBerkeleyDB` in `database_berkeleydb_tests.cpp`: put 50..60,
remove 50..54 by hash, put 54, 53: `get(53)` works, iterator first key is 53.

## Step 7. Mainnet test and release (operations, not code)

1. On this host, a node from the release package (not Docker: the way of the design's section 8,
   "Without Docker") in a directory of its own, `db_backend=lmdb`, API on a free port, built from
   the step-5 branch; then the same in a Docker container (`credits-full-c4`,
   `CREDITS_STORAGE=full`, `CREDITS_DB_BACKEND=rocksdb`) to cover the entrypoint and the
   conversion at the first start. The pruned container `credits-pruned-c3` is not touched.
2. Record every hour for 24 h: oldest stored block, `BACKFILL:` rate lines, CPU and memory,
   database size, the forward `Last written sequence` lag against the pruned node (must stay
   within a few blocks), `DuplicatedRequest`/`AwaitAnswer` counts (tell whether mainnet peers
   have order 35 active).
3. `check_node.py balances --count 50` and `history --count 20 --min-stored` (new option: a
   wallet whose monitor history has blocks below the node's oldest stored block is compared on
   the blocks at or above it, and the node's list must be exactly that suffix) against the
   monitor while it backfills, every few hours: balances equal, histories equal or an exact
   suffix.
4. One stop and restart during the backfill: resume from the lowest stored block, no
   `trxIndex: rebuilding`, no hole (`compare_blocks.py` cannot run against mainnet: check instead
   that `PoolInfoGet` finds every block in 20 random windows of 1,000 below the old lowest).
5. Report: rate, ETA for the whole chain, disk, CPU, whether the default `backfill_rate` should
   change; release as community.12 with `docs/CHANGES.md` updated.
6. Later, when the backfill reaches 0 (days): `history` without `--min-stored` must equal the
   monitor's; `--convert-only` to berkeleydb and open the folder with the official 4.2.532 binary
   (the upgrade-compat check in reverse).

Later option, not in this plan (the user accepted the residual risk): a sample cross-check of
block bytes from a second peer for the smart-signature tails the node cannot verify.

## Security considerations (summary for the reviewer)

- Untrusted peers feed old blocks: accepted only for sequences of a batch in flight, from its
  peer; each block must hash to the previous hash of the verified block above it (anchored by the
  snapshot's lowest block) and its signatures must verify against the confidants inside the hash;
  the buffer holds at most 8 batches; 4 requests in flight; `backfill_rate` caps the load.
- Not verified: smart-signature tails (residual risk accepted by the user). Not needed: the round
  confirmations are inside the hash and are checked only to report an inconsistent chain.
- The online index never writes an entry pointing to itself (the condition that stops the node,
  `blockchain.cpp:2394-2410`) and never changes a pointer the forward index wrote above a wallet's
  tail; a crash leaves at worst unindexed stored blocks, re-indexed at the next start.
- The responders' anti-duplicate log (order 35) is detected per peer and respected; a peer that
  would refuse every lower request is not asked again.
- Nothing consensus-relevant: no change to how blocks are built, signed, chosen or applied; the
  forward sync path and `storeBlock` are untouched except for the routing of replies below the
  first stored block, which that path drops today.

## Sizes

| Step | Content | Size |
| --- | --- | --- |
| 1 | `Storage::pool_save_backfill` + tests | S (≈150 lines + tests) |
| 2 | BlockChain anchor/store/flush, online backward indexing (`indexBackfilledBlock`, tails, floor, repair), `BlockHashes::repairBottom`/`flush`, non-empty map merge + tests | M (≈450 lines + ≈400 tests) |
| 3 | `cs::BackwardSync`, Node wiring, config keys, unit tests | L (≈900 lines + ≈500 tests) |
| 4 | Snapshot conversion at the first start (dbcheck), packages ship `snapshot-restore.sh`, entrypoint, Docker tests, docs incl. "A full node from a snapshot" | M |
| 5 | `run.sh` parameters, `compare_blocks.py`/`compare_histories.py`, tamper and skip-index hooks, 6 scenarios, storage-switch update | M |
| 6 | BerkeleyDB scenario and unit test | S |
| 7 | Mainnet test (package and Docker), release | operations, 2-3 days of wall clock |
