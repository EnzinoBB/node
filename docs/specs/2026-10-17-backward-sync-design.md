# Backward sync: design

Status: draft for review, 2026-10-17.

## Goal

A node that starts from a pruned snapshot takes part in consensus at once. In the background it then downloads the older blocks from peers, newest first, down to block 0, and becomes a full node. Today a full node must sync from block 0 for days before it can do anything.

Decisions taken with the user:
- **No final verification of the snapshot state.** A full replay from block 0 is not run at the end. The downloaded history itself is verified, by hash chain and signatures.
- **All three block databases** (BerkeleyDB, LMDB, RocksDB) support backward sync.

Out of scope:
- Serving snapshots from node to node.
- The full/pruned ratio guard.

## Compatibility

- **No protocol change.** Blocks are requested with the existing `BlockRequest` message, so official 4.2.532 nodes, all full, serve the history as they serve any sync today. Their replies (`RequestedBlock`) are handled on a separate path in our node.
- **No change to blocks or consensus.** Backfilled blocks are stored, never applied to the wallet caches. The state already comes from the snapshot.

## What the code does today (investigation, file:line on master)

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
- **Strictly downwards.** After a crash the stored range has no hole. On start, `first_sequence()` is where backward sync resumes; no other watermark is needed.
- `count_pool` (`storage.size()`) grows with each backfilled block, so the API counts stay right.

### 5. Caches

- `blockHashes_->update(pool)`: `seqdb`/`hashdb` learn each backfilled block, so lookups by sequence and by hash work.
- `firstStoredSeq_` drops to `k` once the block is durable. It becomes atomic.
- Nothing else is touched: the wallet caches, `trxIndex` forward state, `lastNonEmptyBlock_` and the counters already describe the full history.

### 6. Transaction index

The index links each wallet's transactions by a pointer to its previous block. Backwards, that pointer is known only when the older block arrives, so:

- a map `pending[address] = newest backfilled block of that address whose pointer is not yet known`, persisted in the index database every 10,000 blocks;
- for each backfilled block `k` and each address `A` in it: if `pending[A]` exists, write `(A, pending[A]) -> k`, then set `pending[A] = k`;
- the entry `(A, s)` of the oldest stored block, which today points into the pruned range, is completed the same way. Its target block just becomes loadable;
- at block 0, every pending address gets "no previous block".

Until the index reaches block 0, an API history stops at the oldest block it can follow: truncated but correct.

### 7. Progress and completion

- **Visibility.** A log line every 100,000 blocks with the lowest stored block, the rate and the estimated time left. No API change in this version.
- **At block 0:** a log line "full history stored", and the slow-start check (`storage.cpp:297-306`) accepts the database again.

### 8. Fixes found on the way (separate commits, cherry-pickable)

- **Genesis crash.** A one-block reply for block 0 erases `end()` of an empty vector (`node.cpp:1856` → `5116-5119`), because `neededSequence_` is always 0.
- **`TransactionsListGet` on an invalid pool.** It reads `rbegin() + offset` on the empty pool of a pruned block (`apihandler.cpp:2727-2760`).

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

**Index.** After the backfill, the transaction history of every wallet through the API equals a full node's.

**Mainnet.** A Docker node from a snapshot, switched to full, backfills for a day without slowing forward sync. Balances and histories are compared with the monitor, as in the upgrade test.

## Open points

- Whether official mainnet nodes have order 35 active. The pacing works either way; only the rate differs.
- The order of releases: the two fixes of section 8 can ship first, on their own.
