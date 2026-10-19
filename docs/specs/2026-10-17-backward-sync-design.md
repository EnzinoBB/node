# Backward sync: design

Status: draft for review, 2026-10-17, updated 2026-10-19 (see **Since the first draft**). No implementation plan yet.

## Goal

A node that starts from a pruned snapshot takes part in consensus at once. In the background it then downloads the older blocks from peers, newest first, down to block 0, and becomes a full node. Today a full node must sync from block 0 for days before it can do anything.

Decisions taken with the user:
- **No final verification of the snapshot state.** A full replay from block 0 is not run at the end. The downloaded history itself is verified, by hash chain and signatures.
- **All three block databases** (BerkeleyDB, LMDB, RocksDB) support backward sync.

Out of scope:
- Serving snapshots from node to node.
- The full/pruned ratio guard.

## Since the first draft

What changed on master between 2026-10-17 and 2026-10-19, and what it changes here:

- **The fixes of section 8 shipped** in 4.2.532-community.5: the genesis crash (#45) and `TransactionsListGet` on pruned blocks (#46).
- **No pruned snapshot is published yet.** The release `pruned-snapshot` does not exist: the first snapshot comes by hand from a pruned mainnet node still syncing. Backward sync starts from a snapshot, so its mainnet test, and its use, wait for it.
- **The transaction index can be rebuilt from the stored blocks after a quick start** (#53 for pruned nodes, #54 for all nodes, 4.2.532-community.7), about 2.4 times faster than a slow start on mainnet data (~15 min extrapolated to the whole chain, optimistic since the first blocks are nearly empty). Section 6 now relies on it instead of linking the index backwards block by block.
- **Block database conversion** (#49–#52). A full node whose first block is above 0 counts as pruned for it: it cannot be converted to berkeleydb before the backfill reaches block 0 (section 1).
- **BerkeleyDB Recno.** The conversion showed that a first put far above record 1 into an empty Recno table creates every lower record at once and runs out of locks. Backward sync never does that: it writes records below existing ones (section 4).

## Compatibility

- **No protocol change.** Blocks are requested with the existing `BlockRequest` message, so official 4.2.532 nodes, all full, serve the history as they serve any sync today. Their replies (`RequestedBlock`) are handled on a separate path in our node.
- **No change to blocks or consensus.** Backfilled blocks are stored, never applied to the wallet caches. The state already comes from the snapshot.

## What the code does today (investigation, file:line on master of 2026-10-17)

The line numbers are those of the first draft; the implementation plan refreshes them.

- **Requests and replies.** The request is an arbitrary list of sequences (`nodecore.hpp:42`). The responder serves any stored block of any age and skips missing ones (`node.cpp:1777-1829`).
- **Peers.** Our requester tracks only peers ahead of it (`poolsynchronizer.cpp:288-295`).
- **Old blocks are ignored.**
  - Replies are dropped when forward sync is off (`node.cpp:1837-1840`).
  - Blocks older than the last are skipped (`poolsynchronizer.cpp:134-137`, `blockchain.cpp:1774-1779`).
- **Saving.** `Storage::pool_save` accepts only the block after the head (`storage.cpp:686-689`). All three backends can physically store any sequence (BerkeleyDB with an explicit recno, LMDB with an integer key, RocksDB with a big-endian key).
- **Signatures.** A pool hash covers the bytes up to `hashingLength_` and not the signatures (`pool.cpp:149-174`). Signatures are checked with `NodeUtils::checkGroupSignature` against the confidants and the trusted mask, as in `finalizeBlock` (`blockchain.cpp:870-888`).
- **Pruning** deletes from `firstStoredSeq_` upwards (`blockchain.cpp:277-317`). It would delete backfilled blocks.
- **Holes.** A slow start does not notice a hole in the database (`storage.cpp:339-341`).
- **The responders' anti-duplicate log.** When special order 35 has set `syncroChangeRound` (`node.cpp:1751-1774`), each request whose last sequence is not above the previous one from the same sender is refused for 5 s. Requests for blocks the responder lacks leave an entry that never expires.

## Design

### 1. When it runs

- Backward sync runs when the storage is full (`prune_keep_blocks=0`) and the first stored block is above 0.
  - A pruned node never backfills.
  - Switching a pruned node to full starts the backfill. The Docker entrypoint, which today refuses pruned to full, allows it from this release on.
- It runs only while the node is synced with the network. Forward sync and consensus come first: backward sync pauses whenever forward sync is running.
- `[storage] backfill=off` disables it. It is on by default for full nodes.
- **Backends.** A node from a snapshot runs on LMDB (or RocksDB after a conversion) and backfills there. A BerkeleyDB node can backfill only if it was a pruned BerkeleyDB node from block 0 switched to full: the conversion refuses a database whose first block is above 0 to berkeleydb, until the backfill has reached block 0.

### 2. Requests

- **Target.** The block below the lowest stored one, `first - 1`.
- **Batches.** Up to 100 sequences per request, ascending inside the batch: `[first-100 .. first-1]`.
- **Peers.** Peers whose last sequence is above the batch. A peer that answered no block of a batch is marked "not full" for 1 hour; that also stops leaving unanswered entries on peers that pruned.
- **Pacing, for the responders' anti-duplicate log:**
  - at most one batch in flight per peer, and at least 6 s between batches to the same peer;
  - batches go round-robin over the peers;
  - at most 4 batches in flight in total.

  Expected rate with 10 full peers: about 100 blocks / 6 s / peer, that is about 600,000 blocks an hour, or about 13 days for 186 M blocks.
- **Timeouts.** A batch not answered in 30 s is requested again from another peer.

### 3. Verification of each block, before it is stored

For the next expected block `k` (the one below the lowest stored block `k + 1`, whose `previous_hash` is known):

1. `pool.sequence() == k`.
2. `pool.hash()`, recomputed locally from the received bytes, equals `previous_hash` of block `k + 1`. This verifies the whole hashed part, confidants included.
3. The signatures check out: `checkGroupSignature(confidants, mask(numberTrusted, realTrusted), signatures, hash)`, as for a new block. Smart-contract signatures are checked the same way where the forward path checks them.

A block that fails any check is dropped and its batch is requested again from another peer. The peer is marked "not full" for 1 hour. Blocks inside a batch may arrive in any order. They are stored strictly downwards, so out-of-order blocks wait in a small buffer, at most 4 batches.

### 4. Storage

- **A new `Storage::pool_save_backfill(pool)`:**
  - it writes the block through the backend's `put`, and for RocksDB through the write queue;
  - it leaves `last_hash`, the head and `pools_cache` untouched;
  - it runs under the same lock as forward saves.
- **Strictly downwards.** After a crash the stored range has no hole.
- **BerkeleyDB.** The block table is a Recno table opened without `DB_RENUMBER`: pruning leaves the records below the first stored block deleted and numbers do not shift. Each backfilled block fills the record just below the lowest stored one, so no put ever lands far above the existing records (the lock exhaustion seen in the conversion cannot happen). On start, `first_sequence()` is where backward sync resumes; no other watermark is needed.
- `count_pool` (`storage.size()`) grows with each backfilled block, so the API counts stay right.

### 5. Caches

- `blockHashes_->update(pool)`: `seqdb`/`hashdb` learn each backfilled block, so lookups by sequence and by hash work.
- `firstStoredSeq_` drops to `k` once the block is durable. It becomes atomic.
- Nothing else is touched: the wallet caches, `trxIndex` forward state, `lastNonEmptyBlock_` and the counters already describe the full history.

### 6. Transaction index

The index links each wallet's transactions by a pointer to its previous block. During the backfill it is left as the snapshot brought it, and live blocks keep updating it as on any node:

- an API history follows the pointers down to the oldest block whose own entry exists, so it stops at the snapshot's range: truncated but correct, as on a pruned node;
- at block 0 the node records in the index database that the index must be rebuilt at the next start (a key of its own: the incomplete flag would keep the node from the Trusted role until then). The next start quick-starts and rebuilds the whole index from the stored blocks, as #54 does for a lost index. The log line "full history stored" says so.

Rejected: completing the index backwards with a persisted map of pending pointers (the first draft). It kept a second, incremental way of building the index next to the forward one, with its own crash cases, for a result the rebuild after a quick start already gives in minutes.

### 7. Progress and completion

- **Visibility.** A log line every 100,000 blocks with the lowest stored block, the rate and the estimated time left. No API change in this version.
- **At block 0:** a log line "full history stored, the transaction index is rebuilt at the next start", and the slow-start check (`storage.cpp:297-306`) accepts the database again.

### 8. Fixes found on the way

Shipped on their own in 4.2.532-community.5:

- **Genesis crash** (#45). A one-block reply for block 0 erased `end()` of an empty vector, because `neededSequence_` is always 0.
- **`TransactionsListGet` on an invalid pool** (#46). It read `rbegin() + offset` on the empty pool of a pruned block.

## Tests

**Private network** (`robustness.yml`), all with matching state digests:

| Scenario | What happens | Must show |
| --- | --- | --- |
| `backfill-lmdb` | A node joins from a snapshot taken after two prunings, switches to full and backfills | It reaches block 0; its blocks are byte for byte the same as a full node's (compared by hash for every sequence) |
| `backfill-berkeleydb`, `backfill-rocksdb` | The same on BerkeleyDB and on RocksDB (a pruned node of that backend switched to full) | The same |
| `backfill-crash` | The backfilling node killed three times | It resumes with no hole |
| `backfill-tamper` | One peer serves blocks with an altered signature tail, or an altered transaction | Each is refused, the block is taken from another peer, and the peer is marked |
| `backfill-mixed` | Master (official-like) nodes as the only full peers | The history comes from nodes without this code |
| `backfill-order35` | Special order 35 sets `syncroChangeRound`, activating the responders' anti-duplicate log | The pacing keeps the backfill moving |

**Index.** During the backfill, API histories stop at the snapshot's range and never show a wrong entry. After block 0 and one restart, the transaction history of every wallet through the API equals a full node's.

**Mainnet.** A Docker node from a snapshot, switched to full, backfills for a day without slowing forward sync. Balances and histories are compared with the monitor, as in the upgrade test.

## Open points

- **Rate.** With the pacing of section 2 the whole chain takes about 13 days, longer than a full sync from block 0 (35 to 42 hours at the 1,200–1,500 blocks/s of the upgrade test). The gain is a node that works at once, not an earlier full history. Whether official mainnet nodes have order 35 active decides whether the pacing can be relaxed: without it, the anti-duplicate log is off and batches can follow each other faster.
- **The first published snapshot** is a prerequisite for the mainnet test (see **Since the first draft**).
- **Docker.** The entrypoint still refuses pruned to full (`entrypoint.sh:110`, "this node pruned its old blocks, so it cannot become a full node"); it must allow it, and say that the node backfills, in the release that ships backward sync.
