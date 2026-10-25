# Snapshots over the node's p2p: evaluation

Date 2026-10-25, against `origin/master` cb4d2125 (steps 1-3c of the snapshot-serving plan merged),
PR #98 (`origin/feat/snapshot-server`, +1281/-15, 16 files), the design on
`origin/docs/snapshot-serving` and the backward-sync design on `origin/docs/backward-sync`.
The p2p library is the submodule `p2p` pinned at 8fb3c516 (the checkout
`/home/agent/Projects/credits/p2p` is 7 commits ahead; every p2p reference below was read at the
pinned commit with `git show 8fb3c51:src/...`). `csnode`/`net` files are CRLF; line numbers are of
the stripped files.

The user's idea: treat snapshots like blocks, request them in groups from neighbours over the
node's own p2p until complete, instead of (or besides) HTTP.

## 1. Facts

### 1.1 The transport

- **TCP for messages, UDP only for discovery.** `net::Host` listens on a TCP acceptor
  (`p2p/src/host.h:63`, `TcpListen`); a `Connection` is a `bi::tcp::socket` (`connection.h:17`,
  `:65`); one connection per peer id (`host.cc:229-236`, `IsConnected` returns the first match).
  Kademlia datagrams use UDP with `kMaxDatagramSize = 1472` (`routing_table.h:68`); they carry
  routing information, never node messages.
- **Framing, no size limit.** A packet is a 77-byte header, `type(1) + data_size(size_t 8) +
  sender(32) + receiver(32) + reserved(4)` (`common.h:96-107`), followed by `data_size` bytes. The
  receiver does `packet_.data.resize(header.data_size)` with only a `catch (...)` around it
  (`connection.cc:91-97`): there is no protocol limit on a packet, and a peer can make the
  receiver allocate whatever it announces. The node-side packet adds flags(1), type(1), round(8)
  (`net/include/net/packet.hpp:68-75`).
- **One FIFO per connection, no priority on the wire.** `Connection::Send` serialises the packet
  and appends to `send_queue_`; `StartWrite` sends one packet at a time with `async_write`
  (`connection.cc:172-207`). Everything to that peer, consensus stages and round tables included,
  waits behind a large packet already queued. Connections to other peers are not affected.
- **10-second connection deadline.** `ResetTimer` arms a 10 s timer (`connection.h:51`,
  `connection.cc:19-40`) that is reset only when an `async_read` or `async_write` completes
  (`connection.cc:76`, `104`, `190`); a timeout drops the connection (`:26`). A single packet
  whose transfer takes longer than 10 s survives only because the other direction keeps
  completing (the 2-second pings of `Neighbourhood::pingNeighbours`, `neighbourhood.hpp:26`,
  `neighbourhood.cpp:50-55`), which is not a guarantee: a 16 MiB packet on a 10 Mbit/s link takes
  13 s.
- **Before the connection exists** packets wait in `Host::send_queue_`, at most 1,000 packets
  for all peers together; the oldest peer's queue is dropped above that (`host.h:100`,
  `host.cc:369-380`).
- **Inbox.** `Transport::OnMessageReceived` validates and pushes into a two-priority queue
  (`net/src/transport.cpp:165-183`); `RequestedBlock`, `BlockRequest` and every type not listed
  are second priority (`net/src/packetsqueue.cpp:50-80`); above 131,072 packets or 512 MB the
  whole second-priority queue is discarded (`packetsqueue.hpp:30-31`, `packetsqueue.cpp:90-97`).
- **One processing thread.** `processorRoutine` dispatches every node message, consensus and sync
  alike, on one thread (`transport.cpp:252-280`, `315-402`). `Node::getBlockRequest` runs there:
  it loads every requested block and lz4-compresses the batch synchronously
  (`csnode/src/node.cpp:1870-1879`, `1954`); a reply is one `RequestedBlock` packet with the whole
  batch (`:1958`). Today's forward sync is already bulk data on the consensus path, with no limit
  on the batch a responder accepts (`node.cpp:1828-1888`; the requester asks `blockPoolsCount`,
  default 100, `client/config/config.hpp:57`, 150 in the mainnet `config.ini`).
- **Compression.** Block batches are lz4 (`csnode/include/csnode/compressor.hpp:7`, `:47`); the
  `Compressed` flag is set on every node packet (`node.cpp:2311`), the `Signed` flag is set by
  `sendBlockRequest` (`node.cpp:2154`) but never checked by a receiver (`isSigned` is used only
  by `operator<<`, `net/src/packet.cpp:121`).
- **No per-peer bandwidth or rate control** exists anywhere in `net/` or `p2p/`.

### 1.2 Unknown messages, bans, versions

- **An unknown `MsgTypes` is a warning and a drop** on every node, official 4.2.532 included:
  `PacketValidator::validateNodePacket` logs `Packet validator> Unknown message type ...` and
  returns false (`net/src/packetvalidator.cpp:146-149`, upstream commit af4de3f8 of 2019-12-09);
  the dispatcher's `default: break` never runs for it (`transport.cpp:399-400`). Nothing is
  counted, nothing is banned. (`Packet::messageTypeToString` of an unknown value is printed, so
  the line is harmless.)
- **An unknown `NetworkCommand`** (the network-flag packets: VersionRequest/Reply, Ping/Pong,
  `net/include/net/networkcommands.hpp:6-12`) logs `Unexpected network command` and calls
  `transport_->ban(sender)` (`neighbourhood.cpp:32-34`), which is a no-op: `Transport::ban` has
  `//host_.Ban(...)` commented out (`transport.cpp:209-211`, upstream commit fba25d7c of
  2021-09-14 "bad block trap"). So no node bans anyone for a message it does not know.
- **A malformed packet** (empty, or shorter than its header) is dropped by `isHeaderValid`
  (`net/src/packet.cpp:84-97`); a malformed p2p frame drops the TCP connection
  (`connection.cc:86-97`, `kProtocolCorrupted`), and the peer reconnects.
- **Version negotiation.** `VersionReply` carries `NODE_VERSION`, the blockchain uuid, the last
  sequence and the round (`neighbourhood.cpp:62-70`); a peer is a neighbour when its version is at
  least `min_compatible_version` and the uuid matches (`:125-136`), at most 16 neighbours
  (`neighbourhood.hpp:23`). `NODE_VERSION` is 532 in the fork as in the official node
  (`client/config/config.cpp:30`); `MINOR_NODE_VERSION` (`:31`) is not sent. **Nothing in the
  handshake distinguishes a community node from an official one.**
- **A free capability channel exists.** The p2p `host_data` is a 64-bit value in every
  `NodeEntrance` (`p2p/src/common.h:45`, `:65`), serialised into Kademlia datagrams
  (`common.cc:27`, `:39`; `kademlia_datagram.cc:18-43`) and so present in every node's routing
  table. The node fills bits 0-7 with the platform and 8-23 with the version
  (`transport.cpp:35-43`) and decodes only those (`:45-51`, `:450-453`). Bits 24-63 are unused and
  ignored by official nodes: a community node can advertise "serves snapshots" (and even the HTTP
  port) there, and any node can read it from `Transport::getKnownPeers` (`transport.cpp:438-468`)
  without a new message. This is useful for option A's discovery, not only for p2p transfer.

### 1.3 The block part through `BlockRequest`

- **The responder serves any stored block**: `cs::answerBlockRequest(first, firstStored, last)`
  says `NoSuchBlocks` only when the first requested sequence is outside `[firstStored, last]`
  (`csnode/include/csnode/blockrequest.hpp:17-22`, `node.cpp:1850-1856`, #61). Official nodes
  are full (`firstStored = 0`); a pruned community node stores at least `checkpoint_every +
  prune_keep_blocks` blocks below its latest checkpoint, which is the format-2 range by
  construction (`snapshot_blocks` default 101,000, `csnode/include/csnode/snapshot_builder.hpp:25`,
  design section 2). It does not check who asks beyond the p2p registration.
- **The anti-duplicate log** (`node.cpp:1841-1845`, `1857`, `1956`) is active only under special
  order 35 and refuses a request whose last sequence is not above the previous one from the same
  sender: ascending batches from one peer are never refused; descending ones need the 6-second
  pacing of backward sync's section 2.
- **The requester side is forward-only**: `PoolSynchronizer` requests from `last + 1` upwards
  (`csnode/src/poolsynchronizer.cpp:346`, `386-390`), one batch per neighbour round-robin
  (`:349-374`), keeps only neighbours ahead of us (`:288-295`), skips blocks below the last one
  (`:134-137`), and `Node::getBlockReply` drops every reply when sync is off (`node.cpp:1896`).
  A node with an empty database could not use it to fetch `F..S`: it would have to start from
  block 0. Every "fetch blocks below my head from neighbours" machinery is what backward sync
  designs (`origin/docs/backward-sync`, sections 2-4: batches of 100 downwards, 4 in flight,
  per-peer marks and pacing, verification by hash chain from the block above, `BlockVerifier`,
  `pool_save_backfill`). `cs::BlockVerifier` is merged (step 3b, `csnode/src/blockverifier.cpp`).
- **Rates.** The local pruned node applied 25,026 → 5,826 blocks in 84 s on 2026-10-24 (about
  230 blocks/s, `docker logs -t credits-pruned-c3`, "SYNC: Blocks remaining"), 260 blocks/s over
  10 h in the backward-sync design's measurement (section 10). Transfer without applying is
  bounded by the responders (100-150 blocks loaded and compressed per request) at an estimated
  1,000 blocks/s over 10 peers, or 170 blocks/s when every peer paces at 6 s (order 35). So
  101,000 blocks take 2 to 10 minutes from neighbours, against about 70 s for 140 MB over HTTP
  at the 2 MB/s serve cap from one source.
- **Sizes to correct.** Format 2 carries `snapshot_blocks` = 101,000 blocks, not 160,000 (that is
  the format-1 LMDB table: 161,891 blocks, 356 MB, design "What exists today"). At about 2.1 KB
  per block (backward-sync section 10) the format-2 block stream is about 210 MB raw, roughly
  140 MB lz4 (ratio 1.5, design section 12, not yet measured). The block part is two thirds of a
  snapshot's bytes.

### 1.4 The state part

- `qs/<S>` of the local node (`/home/agent/Projects/credits/mainnet-pruned-c3/csnode/qs/186500000/`):
  `walletscache.dat` 181.6 MB, `apihandler.dat` 67.0 MB, `blockchain.dat` 62.6 MB,
  `smartcontracts.dat` 59.0 MB, `walletsids.dat` 6.6 MB, `roundstat.dat` 0.23 MB, seven small
  files; 368 MB on disk (`du`: 360 MiB). About 60 MB lz4 (caller's figure; 35 MB with zstd -3 in
  the design, section "What exists today"); not re-measured here. Plus `contracts.bin` about 9 MB
  raw.
- **Chunking over p2p.** Given 1.1, a chunk must stay far below what the 10 s timer and the
  per-connection FIFO tolerate: 256 KiB to 1 MiB (a 1 MiB packet holds the connection for 0.8 s
  at 10 Mbit/s). 60 MB is 60 to 240 request/reply pairs per source. The manifest's 4 MiB chunk
  hashes (design section 2) do not fit; a p2p chunk table would be a second list of hashes in the
  manifest, or a Merkle root, or whole-file hashes checked only at the end (then a bad chunk
  cannot be attributed to a source).
- **The serving side** would answer on the transport processor thread (1.1): a file read of the
  chunk size per request, cheap, but with no rate control unless the node adds one; the HTTP
  server of #98 has its own threads, connection cap and `serve_rate_kbps` token bucket (design
  section 5).

### 1.5 Where the fetch runs, and the start-up order

- The planned fetch runs in `client/src/main.cpp` **before the keys are read and before the
  node exists**: `dbcheck::check` at `main.cpp:193-201`, `--import-snapshot` at `:203-206`
  ("no keys needed"), keys at `:208-217`, `cs::Peer` (which builds `Node`, `Transport`,
  `BlockChain`) at `:232`. A node started on an empty database writes the genesis block
  (`csnode/src/blockchain.cpp:258-266`) and syncs forward from 0.
- So any p2p-based fetch needs one of: (a) a second p2p client in `main.cpp`, a `net::Host` with
  the node key and a handler that speaks VersionRequest/Reply and Ping/Pong well enough to be
  accepted as a neighbour (`neighbourhood.cpp:138-158`: a peer answers our VersionRequest and
  adds us if compatible; our uuid is 0 and passes `:131`), plus the new snapshot messages, plus
  NAT/UPnP and bootstrap that `Host::Run` does; or (b) a "bootstrap mode" of `Node` that runs
  the transport with no blockchain, which touches every place that assumes `blockChain_` is
  initialised (`sendVersionReply` reads `getLastSeq()` and `uuid()`, the validator reads the
  conveyer round, `chooseMessageAction` reads `good_`). (a) is a duplicate of about 300-500
  lines of handshake and a second owner of the p2p port; (b) is a start-up refactor of the most
  fragile code in the node.
- Exception: a design that **starts the node from the state alone** (option B2 below) needs no
  pre-start p2p: the blocks come after the node is up, through the transport it already runs.

### 1.6 Trust

- **The p2p layer does not authenticate node keys.** The registration packet carries the sender's
  `NodeId` (= public key) and its endpoint, nothing signed (`p2p/src/network.cc:249-252`,
  `host.cc:328-346`; no signature or challenge anywhere in `p2p/src`: `git grep -i "sign\|verif\|
  nonce" 8fb3c51 -- src` finds only `serialization.cc`). Consensus messages carry their own
  signatures; `BlockRequest`/`RequestedBlock` do not, and the `Signed` flag is never checked
  (1.1). So "sources are neighbours, already authenticated by the handshake" does not hold: a
  p2p peer can claim any key. A snapshot offered over p2p needs the same Ed25519-signed manifest
  as over HTTP, and the agreement of `min_sources` on `(S, head_hash, state_digest)` stays as
  designed (design sections 6.3, 7). Blocks need no source trust in any option: the hash chain
  anchored at the agreed `head_hash` plus the pool signatures (`BlockVerifier`) make the source
  irrelevant, as backward sync's section 3 says.
- Over p2p the /24 rule of section 6.3 still works (the peer's address is known), GitHub stays
  the anchor that no node key can impersonate, and it needs HTTP (`curl`) regardless.

## 2. Options

### A. HTTP only, as designed (steps 5a, 5b, 6, 7, 8)

- Protocol changes: none on p2p. One TCP port per serving node (9511), the server of #98.
- Official nodes, consensus: untouched. A serving community node spends upstream bandwidth under
  `serve_rate_kbps` on threads of its own; the builder takes `dbLock_` in batches.
- Size: 5a large, 5b medium, 6 small, 7 medium, 8 medium (plan table).
- Risks: few community sources at first (`min_sources=3`, fallback GitHub); the block part makes
  every snapshot about 200 MB, served by a handful of HTTP sources while 16 neighbours, official
  ones included, hold the same blocks and already serve them to every syncing node.
- One cheap improvement from 1.2: advertise "serves snapshots on port P" in bits 24-63 of
  `host_data`, so that `probe_known_hosts` probes only peers that say so (and finds nodes that are
  not in `known_hosts.txt` yet), instead of every known host at 9511.

### B. Hybrid: blocks through `BlockRequest`, state through HTTP

Two ways to do the block part; they differ in everything that matters.

**B1: fetch `F..S` before the node starts, over p2p.** Needs 1.5 (a) or (b): a second p2p
client or a bootstrap mode, plus a forward-batched requester with per-peer marks and timeouts
(backward sync's section 2 rewritten upwards), buffering 101,000 blocks to a staging stream and
handing them to the merged importer (which checks the chain to `head_hash`; the manifest's
`blocks.bin.lz4` entry is skipped since two honest nodes do not hold byte-identical blocks, #80).
Keeps all of 5a for the state (manifest fetch, agreement, chunked HTTP download of `qs-*`):
saves nothing in 5a, adds a large step. Not recommended.

**B2: the snapshot carries the state and block `S` only; blocks `F..S-1` come after the start
through backward sync with a floor.** The importer writes `qs/<S>`, the contracts and the single
block `S` (verified against `head_hash` and its signatures as today); the node quick-starts from
`qs/<S>` (`tryQuickStart` finds `S` in `seqdb`, `blockchain.cpp:84-118`), syncs forward from
`S + 1` at once (forward sync needs only the last block and its confidants,
`blockchain.cpp:1602-1612`), and backward sync backfills from `S − 1` down to `F`
(= `S − snapshot_blocks`) exactly as it is designed, blocks verified by the chain from `S`,
from every neighbour that has them (official full nodes included, `firstStored = 0`).

- Protocol changes: none. Blocks move as blocks do today.
- Official nodes, consensus: a new node draws about 101,000 extra blocks from its neighbours over
  2 to 10 minutes (1.3), the same traffic the node's own forward sync draws for a snapshot a day
  old; load on each responder as today's sync. Nothing new on the wire.
- Snapshot size: about 70 MB (state + contracts + one block) instead of about 200 MB; HTTP
  sources serve one third of the bytes; `snapshot_blocks` becomes the backfill floor.
- Size: step 3a/3c change small (a block count of 1 is the existing builder with
  `snapshotBlocks = 1`; the importer already writes any count); backward sync's steps are
  needed first (its plan, `origin/docs/backward-sync`, is not implemented); the floor is a small
  addition to its section 1/7 ("stop at `backfill_floor` instead of 0", pruned nodes keep their
  floor at `F`, full nodes continue to 0). Steps 5a/5b/6/7/8 unchanged in shape; 5a shrinks a
  little (one file fewer, the biggest).
- Risks: (1) for the minutes of the backfill the node's API history below `S` is incomplete and
  the transaction index covers blocks as they arrive: backward sync's section 6 ("indexed online,
  no restart") must hold for this window, and `TransactionsListGet`'s `previousNonEmpty_` walk
  skips blocks it cannot load (#46); (2) start-up code that reads blocks below `S` at a quick
  start: the hash-database consistency check (`blockchain.cpp:266-279`, counts within the stored
  range, fine with one block), the index rebuild (`:143-153`, nothing to rebuild), contract state
  validation at start (`node.cpp:213`, `250`: uses the deploys kept by the contracts, #74) — each
  to be verified in the `snapshot-serve-backend`-style scenario; (3) the dependency on backward
  sync being merged, which postpones the snapshot release behind it, unless the snapshot keeps
  the full block stream until then (both formats can coexist: a manifest with
  `first_block = seq` is just a short stream).

### C. Hybrid: blocks through `BlockRequest`, state through a new p2p message among community nodes

- Protocol changes: new `MsgTypes` values (`SnapshotManifestRequest/Reply`,
  `SnapshotChunkRequest/Reply`; values above 39 are free, `packet.hpp:24-66`), their entries in
  the validator's never-cut list (`packetvalidator.cpp:94-109`) and `chooseMessageAction`, a
  capability bit in `host_data` to find community peers (1.2), a chunk table that fits 1.4
  (≤ 1 MiB pieces), request/reply bookkeeping with timeouts and per-source marks (the HTTP
  fetcher's logic over a message channel), a serving-side rate limit on the processor thread.
  The block part as B1 or B2.
- Official nodes: a request sent to one by mistake is one warning line and a drop (1.2); with the
  capability bit none is sent. Consensus on the serving community node: each chunk reply is a
  file read and a packet on the shared processor thread and on the one connection to the
  requester (a node that is not in consensus), so the direct effect is small; the upstream
  bandwidth effect is the same as HTTP's and needs the same cap, implemented anew. The receiver's
  inbox puts the replies at second priority, discarded whole under load (1.1): the fetcher must
  retry.
- Start-up: the state is needed before the node starts, so C needs 1.5 (a) or (b) in full. This
  is the cost that decides it.
- Size: the HTTP client stays for GitHub (`curl`) and the server of #98 can go, but a second p2p
  client or a bootstrap mode (1.5), the new messages, chunking and rate control, and tests of
  the transport path replace it: at least 5a's size again on top of a reduced 5a; the private
  network scenarios of step 7 need a p2p variant each.
- Risks: a new wire format shared with nobody else; bulk data on the connection that also
  carries the serving node's consensus packets to that peer (acceptable only because the
  requester is not a confidant); the unauthenticated sender (1.6) means every reply is trusted
  only through the signed manifest, as over HTTP; the 10 s connection deadline on slow links.

### D. Everything over p2p

C plus the blocks before the start over the same channel (B1), and no HTTP at all: GitHub stays
only as a `curl` fallback. Everything C costs, plus B1, and the Docker/CI publication of
format 2 (step 8) still writes files. No advantage over C.

## 3. Recommendation

**Keep HTTP for the state (option A as designed), and take the user's idea for the blocks in
its cheapest form, B2: a snapshot is the state plus its anchor block; the blocks below come
from the neighbours as blocks, through the existing `BlockRequest`, after the start, by backward
sync with a floor.** This is "snapshots managed like blocks" where it is true: blocks are
blocks, every neighbour serves them, verification is the chain, and no official node sees a
message it does not know. The state is not a block: it has no chain to anchor it, two honest
nodes do not produce the same bytes, it is needed before the node runs, and the p2p transport
has no size limit, no priority and no rate control; moving it onto p2p (C, D) would need a
second p2p client or a start-up refactor plus a new protocol, to replace an HTTP server that is
already written (#98) and a client that is still needed for GitHub.

Two additions to A that cost little:

1. Advertise the snapshot service in `host_data` bits 24-63 (1.2) and use it in
   `probe_known_hosts`: discovery through the routing table instead of blind probing.
2. Keep the full block stream in format 2 until backward sync is merged (the manifest's
   `first_block` already says how many blocks a snapshot holds); then switch the default
   `snapshot_blocks` to 1 and let the floor do the rest. Both forms import with the same code.

## 4. How the plan's remaining steps change

| Step | A (as planned) | With B2 added |
| --- | --- | --- |
| 5a fetcher | unchanged | unchanged now; later drops `blocks.bin.lz4` from the download when `snapshot_blocks = 1` |
| 5b start-up, Docker | unchanged | plus: after the quick start, hand `F = S − snapshot_blocks` to backward sync as its floor on a pruned node (full nodes keep 0) |
| 6 discovery job | unchanged | plus `host_data` capability bit read from the job's node (`getKnownPeers`) as a second discovery input |
| 7 scenarios | unchanged | plus `snapshot-state-only`: n8 restores a 1-block snapshot, reaches the head, backfills to `F`, API history complete at the end, digests match |
| 8 GitHub format 2 | unchanged | later: the published snapshot shrinks to about 70 MB; the balance cross-check stays |
| new | — | backward sync steps 1-4 (its plan) before `snapshot_blocks = 1` becomes the default; a `backfill_floor` option (small) |

Nothing in C or D is recommended; if the user wants p2p for the state anyway, the prerequisites
to settle first are 1.5 (where the fetch runs) and 1.1 (a chunk size and a rate limit on the
processor thread), and the protocol would need a size cap on `data_size` at the p2p receiver,
which does not exist today for any packet.

## 5. Open questions for the user

1. Is the dependency of B2 on backward sync acceptable (snapshot release first with full block
   streams, state-only snapshots once backward sync is merged), or should the block stream stay
   in the snapshot for good?
2. Should the `host_data` capability bit be used for discovery (it is visible to every node on
   the network, official ones included, as an unused value), or is `known_hosts.txt` plus the
   weekly job enough?
3. Were the p2p facts above (no key authentication, no size cap, 10 s deadline) known? They also
   matter for backward sync's threat model and for #61-style fixes, independently of snapshots.
