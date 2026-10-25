# Snapshots over the node's p2p: design

Status: draft for review, 2026-10-25, against `origin/master` cb4d2125 (4.2.532-community.12: steps
1-3c of the 2026-10-24 plan merged, that is `[snapshot]` configuration #88, builder and
`--export-snapshot` #89, `BlockVerifier` #90, importer and `--import-snapshot` #92, checkpoint
digests #87). The p2p library is the submodule at 8fb3c516 (`p2p/src/...` references are to that
commit). `csnode`, `net` and `client` files are CRLF; line numbers are unchanged by that. The
implementation plan is `snapshot-p2p-plan.md`, next to this file.

This document **replaces sections 5 (serving), 6 (fetching), 9 (discovery) and 10 (GitHub and
Docker)** of `docs/specs/2026-10-24-snapshot-serving-design.md` and amends 7, 8, 11-15. Sections
1-4 of that design (where the feature lives, format 2, the checkpoint digest, the build) hold and
are merged; what changes in them is listed in section 1 below. The evaluation that led here is
`docs/specs/2026-10-25-snapshots-over-p2p-evaluation.md` (option D, extended by the user).

## Decisions (user, 2026-10-25)

| Question | Decision |
| --- | --- |
| Transport | Snapshots move **only over the node's p2p layer**, between community nodes. No HTTP server, no HTTP client in the node's normal start. PR #98's Beast server is set aside; its builder service survives (section 5.4). |
| Who serves | Any community node, pruned or full, whatever its stake, **only while synced** with the network and **only a complete snapshot** it has built; otherwise it answers "not ready". Serving on by default in the packages and the Docker image, off in the bare binary. |
| Who is asked | Neighbours that advertise the snapshot protocol in the p2p `host_data` bits 24-63, which official nodes ignore (section 4.1). Official nodes are never asked for snapshots; they are asked for blocks as today. |
| What a snapshot is | The state (`qs/<S>` files) and the anchor block `S`, as format 2. Until backward sync is merged the snapshot keeps the block stream `F..S` (`snapshot_blocks`, 101,000); afterwards `snapshot_blocks` defaults to 1 and the blocks below come from every neighbour, official ones included, through the existing `BlockRequest` by backward sync with a floor (section 7). |
| Agreement | At least `min_sources` independent community sources (one per /24, or per /48 for IPv6) with signed manifests declaring the same `(S, head_hash, state_digest, settings_digest)`. GitHub is no longer a source and no longer a fallback. |
| Too few sources | The node waits up to `fetch_wait` for agreement, then syncs from block 0 and logs how to import the GitHub snapshot by hand (section 6.4). `min_sources`: recommendation 2 for now, question for the user. |
| GitHub | Stays published by `snapshot.yml` (format 2, checkpoint blocks) **for manual use only**. The node gets an on-request tool, `--download-snapshot [URL]` (system `curl`, then the existing importer, then exit). The normal start never contacts GitHub. |
| Docker | The entrypoint calls `node --download-snapshot` explicitly on a first start with an empty database, then starts the node; if the download fails the node's own p2p fetch takes over at the start. `snapshot-restore.sh` goes after one release. |
| Testnet first | Every step runs on private networks and the GitHub-hosted ~50-node testnet (`scale.yml`) before the local mainnet node serves; mainnet defaults change last. |

## 1. What holds from the 2026-10-24 design, and what changes in it

Holds, merged, unchanged: section 2 (format 2: `snapshot.json`, `blocks.bin.lz4`, `qs-<name>.lz4`,
per-chunk SHA-256, Ed25519 signature over the `signed` line, the fixed name table
`SnapshotManifest::isKnownFileName`, `csnode/include/csnode/snapshot_manifest.hpp:47-50`); section 3
(the checkpoint digest, now with the consensus-settings digest, `statedigest.hpp:51-69`, checked at
every quick start, `blockchain.cpp:105-116`); section 4 (the builder, `snapshot_builder.hpp:26-49`);
the importer (`snapshot_importer.hpp:32-34`: self-consistency of files, hashes, chain, signatures,
checkpoint head and digests; everything removed on failure); `--export-snapshot` and
`--import-snapshot` (`client/src/snapshot_export.cpp:63-146`, `148-183`).

Changes:

- **Chunk size.** The builder's default chunk is 4 MiB (`snapshot_builder.hpp:34`, manifest
  `chunk_size`). A p2p piece is one manifest chunk, so the builder writes **512 KiB chunks**
  (section 4.3 says why). The importer, hashes and the manifest format do not change; the manifest
  of a 200 MB snapshot has about 400 chunk hashes (26 KB of JSON, limit `kMaxJsonSize` 1 MiB,
  `snapshot_manifest.hpp:30`).
- **No `contracts.bin.lz4`.** PR #98 found that the contracts database holds each contract's latest
  state, later than `S`, while the checkpoint carries the states at `S` (`smartcontracts.dat`); the
  builder of #98 writes no contract file and `--export-snapshot` follows. That change is taken over
  (plan step P3). The importer keeps accepting the file for older snapshots.
- **Section 7 (trust).** GitHub is no longer an anchor of the automatic path. What replaces it:
  section 11.
- **Section 8 (configuration).** The HTTP keys go; section 12 has the new table.
- **Section 11 (backward sync).** Section 7 below: the floor, the ordering of the two plans.
- **Section 13 (tests).** Section 13 below replaces the scenario table.
- **Section 14/15 (decisions).** Superseded by the table above where they differ: decision 1
  (HTTPS through `curl`) now applies to `--download-snapshot` only; decision 3 (`min_sources=3`,
  `fallback=github`) is reopened (section 6.4).

## 2. Transport facts this design relies on

All on the pinned p2p commit and on `net/` of cb4d2125; the evaluation's section 1 has the longer
list.

- One TCP connection per peer, one FIFO send queue per connection, one `async_write` at a time
  (`p2p/src/connection.cc:172-207`); a packet is a 77-byte header with a `size_t data_size` and no
  cap, the receiver resizes to it (`connection.cc:91-97`).
- A 10-second deadline per connection, reset only by a completed read or write on that
  connection (`connection.cc:19-40`, `76`, `104`, `190`). Neighbours exchange Ping/Pong every 2 s
  (`neighbourhood.hpp:26`, `neighbourhood.cpp:50-55`, `72-86`), which keeps both sides' timers
  alive during a long transfer **only when the requester has the responder in its neighbour
  table** (it then pings; the responder pongs any pinger). Section 4.3 draws the rule from this.
- Packets to a peer without a connection wait in `Host::send_queue_` (at most 1,000 for all peers,
  `host.h:100`, `host.cc:369-380`); `SendDirect` to a peer not in the routing table starts a
  Kademlia lookup (`host.cc:154-168`).
- The node's inbox is two priority lists; everything not listed in `PacketsQueue::getPriority` is
  second priority (`net/src/packetsqueue.cpp:50-80`) and the whole second list is discarded above
  131,072 packets or 512 MB (`packetsqueue.hpp:30-31`, `packetsqueue.cpp:90-97`). One processor
  thread handles every node message (`net/src/transport.cpp:252-280`, `315-402`).
- An unknown `MsgTypes` value is a warning and a drop on every node, official 4.2.532 included
  (`net/src/packetvalidator.cpp:146-149`); an unknown network command calls `Transport::ban`, which
  is a no-op (`transport.cpp:209-211`).
- `host_data` is a 64-bit value in every `NodeEntrance`, serialised in every Kademlia datagram and
  kept in every node's k-buckets (`p2p/src/common.h:40-55`, `common.cc:22-41`,
  `routing_table.cc:174-186`); the node fills bits 0-23 (`transport.cpp:35-43`) and decodes only
  them (`45-51`, `450-453`). It is set once at `Network::Init` (`network.cc:199`) from
  `Config::host_data`.
- The p2p layer does not authenticate node keys (registration carries an unsigned id and
  endpoint, `p2p/src/network.cc:249-252`, `host.cc:328-346`).
- A node started on an empty database writes the genesis block (`csnode/src/blockchain.cpp:256-266`)
  and syncs forward from 0; `Node::init` opens the chain (`node.cpp:217-242`) inside the
  constructor (`node.cpp:122`), before the transport runs (`Peer::onRun`, `client/src/peer.cpp:130-134`,
  `Node::run`, `node.cpp:321-324`).
- `cs::Signal` emits without a lock (`lib/include/lib/system/signals.hpp:70-77`) while
  `cs::Connector::connect` takes one (`:384-400`): connecting a slot while another thread emits is
  a data race. Section 6.6 keeps every connection in the constructor because of this.

## 3. Architecture

```
 serving community node (any stake, synced, snapshot built)        joining node (empty database)
 ┌──────────────────────────────────────────┐                      ┌─────────────────────────────┐
 │ SnapshotService  (from #98: thread,      │                      │ Node in BOOTSTRAP MODE      │
 │   10-s config poll, build on checkpoint) │                      │  transport runs, chain not  │
 │   └─ builds snapshot/<S>/ (512 KiB chunks)│                      │  opened, every other message│
 │ SnapshotProvider (new, processor thread) │   ManifestRequest    │  dropped                    │
 │   manifest + pieces from snapshot/<S>/   │ <─────────────────── │ SnapshotFetcher (new,       │
 │   token bucket, per-peer table,          │   ManifestReply      │  processor thread, timer):  │
 │   "not ready" unless synced and built    │ ───────────────────> │  discover (host_data bit)   │
 │                                          │   PieceRequest ×N    │  collect manifests, agree,  │
 │ host_data bit 24: "snapshot protocol 1"  │ <─────────────────── │  fetch pieces, verify       │
 └──────────────────────────────────────────┘   PieceReply ×N      │  → snapshot-tmp/            │
                                                ───────────────────>│ worker thread: importer,    │
 official 4.2.532 node                                              │  BlockChain::init (quick    │
   never asked for snapshots (no bit)                               │  start from qs/<S>), then   │
   serves blocks F..S-1 to backward sync as today                   │  the node runs as usual     │
                                                                    └─────────────────────────────┘
 GitHub release pruned-snapshot: format 2, checkpoint blocks, manual only:
   node --download-snapshot [URL]  →  curl  →  verify  →  importSnapshot  →  exit
```

Nothing changes in how blocks are made, signed, chosen or applied; the new messages are between
community nodes only; the node's start-up order changes only on an empty database with
`fetch=auto`.

## 4. Protocol

### 4.1 Capability flag in `host_data`

Bits 24-31 of the node's `host_data` become a capability byte (`getHostData`, `transport.cpp:35-43`):

| Bit | Meaning |
| --- | --- |
| 24 | speaks the snapshot protocol version 1 (this build, with `[snapshot] serve=on` at start) |
| 25-31 | reserved, 0 |

Bits 32-47 carry `MINOR_NODE_VERSION` (`client/config/config.cpp:31`, today not sent anywhere), so
that the weekly job can tell community builds apart; 48-63 stay 0.

Reading: `Transport::getKnownPeers` (`transport.cpp:438-468`) decodes the byte into a new
`cs::PeerData::capabilities` (`lib/include/lib/system/common.hpp:50-59`), and a new
`Transport::peerCapabilities(const cs::PublicKey&)` looks one peer up in the routing table's
known nodes (`Host::GetKnownNodes`, `host.h:40`, `routing_table.cc:62-66`). Official nodes relay
the value unchanged in their FindNode replies, so a joining node sees it for every community node
in its routing table, not only for direct contacts.

Limit: the value is fixed for the life of the process (`network.cc:199`). Switching `serve` in
`config.ini` at run time still starts or stops serving (the provider answers "not ready" when
off), but a node that started with `serve=off` is not discovered until its next start. A
`Network::SetHostData()` in the p2p submodule (refreshed by the next ping of each peer) is a small
later change, not in this plan.

### 4.2 Messages

Four new `MsgTypes` values (`net/include/net/packet.hpp:24-66`; 40-43 are free, `NodeStopRequest`
is 255). All are direct packets (`Node::sendDirect`, `node.cpp:2307-2312`), flags
`BaseFlags::Clear` (the payloads are lz4 files or JSON; the node's `Compressed` flag is only a
marker, compression is explicit per type), round = the sender's current round (0 on a joining
node). Serialisation with `cs::ODataStream` as every node message (strings and byte arrays carry a
`u64` length first), read back by a bounded reader of the codec (P1); every limit below is checked on the receiving side before anything is parsed
further, and a packet beyond it is dropped and counted.

| Value | Name | Payload | Limit |
| --- | --- | --- | --- |
| 40 | `SnapshotManifestRequest` | `u8 protocol = 1` (0 refused; a higher value may be followed by fields of that protocol, which a version 1 node skips) | 16 bytes |
| 41 | `SnapshotManifestReply` | `u8 status` (0 ready, 1 not serving, 2 not synced, 3 building), `u64 last_seq`, `u64 round`, `u8 serving_trusted` (0/1), then if ready `string manifest_json` | 256 KB |
| 42 | `SnapshotPieceRequest` | `u64 seq`, `string name` (from the fixed table, at most 64 chars), `u32 chunk` | 128 bytes |
| 43 | `SnapshotPieceReply` | `u8 status` (0 ok, 1 unknown snapshot/name/chunk, 2 busy, 3 not serving), `u32 retry_ms` (always present, meaningful when busy), `u64 seq`, `string name`, `u32 chunk`, then if ok `bytes data` | `chunk_size` from the manifest, at most 1 MiB |

Wiring: the four values in `Packet::messageTypeToString` (`net/src/packet.cpp`), in the
validator's never-cut list (`packetvalidator.cpp:94-109`: a joining node is at round 0, so the
round cut at `:112-115` would drop every reply), second priority in the inbox (the default of
`getPriority`: the fetcher retries what the inbox discards), `Process` in `chooseMessageAction`
(`node.cpp:2222-2225`, next to `BlockRequest`), four cases in `dispatchNodeMessage`
(`transport.cpp:326-401`).

What official nodes see: nothing, unless a community node sends them a request by mistake (it
does not: requests go only to peers with bit 24). A request that reaches one is `Packet
validator> Unknown message type` at warning level and a drop (`packetvalidator.cpp:146-149`); no
ban, no counter. Replies never reach them.

### 4.3 Piece size and the connection rules

- **One piece = one manifest chunk = 512 KiB** (the builder writes `chunk_size = 524288`; the
  fetcher refuses a manifest whose `chunk_size` is above 1 MiB or not a power of two, and the
  provider refuses to serve one). Why 512 KiB: at 10 Mbit/s a piece occupies the connection's
  FIFO for 0.4 s, which bounds the delay of any packet the serving node has for that peer
  (section 8); the per-piece cost on the serving node's processor thread is one `pread` and one
  packet, about a millisecond, so smaller pieces cost nothing on that side; 400 chunk hashes keep
  the manifest small. 1 MiB is the upper bound because the receiver allocates whatever the
  header announces (section 2) and the fetcher must bound what a source can make it allocate per
  reply.
- **Sources must be neighbours of the joining node.** The fetcher asks a peer for pieces only
  while `Transport::hasNeighbour(key)` (`transport.cpp:430-432`): then the joining node pings it
  every 2 s and the peer pongs, so the 10-s deadline of both ends is reset during a transfer even
  on a slow link. The fetcher brings the serving peers it discovers into the neighbour table with
  `Transport::addToNeighbours` (`transport.cpp:470-472`: a VersionRequest; the peer answers and is
  added, `neighbourhood.cpp:138-158`) and, since the table holds 16 peers first-come
  (`neighbourhood.hpp:23`, `:147-150`), it marks the discovered serving peers permanent for the
  duration of the bootstrap (`setPermanentNeighbours`, `transport.cpp:418-420`) so that they are
  not refused for lack of room; the permanent set is restored to the initial confidants when the
  chain comes up.
- **At most 2 pieces outstanding per source, 8 in total**, so that a source never has more than
  1 MiB queued for us and the joining node never has more than 4 MiB of replies in flight.

### 4.4 Frame size on the receiving side

The node-level limits of 4.2 are applied after the p2p layer has already allocated the announced
size. A cap on `data_size` in `Connection::StartRead` (`p2p/src/connection.cc:91-97`) is the real
defence, and it protects every message type, not only these. It is a change to the p2p submodule
and so a separate, flagged step (plan P9) with its own question: the official node has no cap,
so the cap must stay above the largest legitimate packet (a `RequestedBlock` of 150 blocks is
usually under a megabyte, but a block's size is unbounded in principle). Proposed: 256 MiB, half
the inbox's byte limit, with a counter log line.

## 5. Serving

### 5.1 `cs::SnapshotProvider`

A small object owned by `Node`, used on the processor thread (where every node message is
handled, `transport.cpp:252-280`), with no thread of its own:

- holds the manifest of the newest complete `snapshot/<S>/` (loaded by `SnapshotService` after a
  build, section 5.4, swapped under a mutex), the open file handles of its files, and the
  per-chunk offsets (`chunk × chunk_size`);
- `onManifestRequest(sender)`: answers status 1 when `[snapshot] serve=off`, 3 when no snapshot is
  built, 2 when not synced (5.2), 0 with the JSON otherwise; always with `last_seq`, `round` and
  `serving_trusted`;
- `onPieceRequest(sender, seq, name, chunk)`: the name must be in `SnapshotManifest::isKnownFileName`
  and in the loaded manifest, `seq` must be the loaded snapshot's, `chunk` below the file's chunk
  count; else status 1. Then the budget (5.3): status 2 with `retry_ms` when exhausted. Then one
  `pread` of `chunk_size` bytes (the last chunk shorter) from the file at `chunk × chunk_size`,
  one `sendDirect`. The file name is never taken from the request to build a path: the request
  names a manifest entry, the provider opens only files under `snapshot/<S>/` it listed itself.
- logs: `SNAPSHOT: serving block S (N files, M MB)` when a snapshot is loaded; one line per peer
  that completed a whole snapshot (`SNAPSHOT: served block S to <key> (<ip>), M MB in T s`); one
  summary line an hour when there was traffic (peers, pieces, bytes, busy answers).

### 5.2 Only synced nodes serve, only complete snapshots

Synced: `!poolSynchronizer_->isSyncroStarted()`, `!isBootstrapRound_`, and
`blockChain_.getLastSeq() + 3 >= cs::Conveyer::instance().currentRoundNumber()` (the
`wait-head` rule of `ci/snapshot/check_node.py`, as in #98's `SyncedTo`). Complete: a
`snapshot/<S>/snapshot.json` that verifies and whose files have the declared sizes, loaded by the
service after the builder's rename of `<S>.tmp` to `<S>`. A node that stops being synced answers
status 2 from the next request on; a download in progress from it simply gets busy/not-synced
answers and the fetcher moves to another source of the same `sha256` group (6.5).

### 5.3 Budget

- `serve_rate_kbps` (default 2000 in the packages and Docker, section 12): a token bucket in
  bytes over all peers, refilled per request from a steady clock, burst of 4 MiB. Empty: status 2
  with `retry_ms` = the time to the next piece's worth of tokens (at most 5 s).
- Per peer: at most 2 pieces answered per 100 ms and at most half the global rate, so that one
  requester cannot take the whole budget; `serve_max_peers` (default 8) peers served within the
  last 30 s, others get status 2 with `retry_ms = 5000`.
- When the node is a confidant or the writer of the current round (`myLevel_ != Level::Normal`),
  the budget is halved and `serving_trusted = 1` is reported, so that the fetcher prefers other
  sources when it has them (section 8). The user's decision keeps trusted nodes serving; the
  `snapshot-p2p-trusted` scenario measures the effect.
- Nothing is kept per requester beyond the 30-s table (key, last request time, pieces in the
  window): a flood of requests from unknown keys costs one map lookup and one small reply each,
  bounded by the per-peer and global buckets; the map is capped at 64 entries (oldest evicted).

### 5.4 What of PR #98 survives

Reused as they are, moved to the first serving step (plan P3): `csnode/src/snapshot_service.cpp`
and its header (the thread at low priority, the 10-s configuration poll, the build on
`checkpointSavedEvent` and at start for the latest checkpoint without a snapshot, the disk-space
check, the retry a minute apart, five attempts), `BlockChain::checkpointSavedEvent`,
`BlockChain::blockBytes`, `BlockChain::pruneTarget` (start above the blocks pruning is removing),
`Storage::pool_bytes`, the builder's streaming copy of the checkpoint files, the removal of
`contracts.bin.lz4`, `run.sh`'s `snapshot_section` and the `[snapshot]` wiring. Dropped:
`snapshot_server.hpp/.cpp` (582 lines of Beast), `snapshot_server_tests.cpp`, the HTTP part of the
`snapshot-serve` scenario, `Storage::forEachContractData`/`BlockChain::forEachContractData` (no
longer used). The service's `SnapshotServer::apply/reload` calls become `SnapshotProvider::load(dir)`
and `unload()`.

## 6. Start-up on an empty database

### 6.1 Decision: a bootstrap mode of `Node`, not a second p2p client

A node whose `db/` holds no database must get the state before `BlockChain::init` runs, and must
speak p2p to get it. Two ways were weighed in the evaluation (1.5):

- **(a) a separate client in `main.cpp`**: a second `net::Host` with its own handler for
  VersionRequest/Reply, Ping/Pong and the four messages. Against it: `net::Network` is a process
  singleton initialised in `Host`'s constructor (`host.cc:17-26`, `network.cc`), with the UDP
  socket, NAT traversal and the host contacts; a second `Host` later in the same process is
  untested ground in the library and the first one cannot be stopped and restarted (there is no
  stop; the destructor stops `io_`). The handshake (200 lines of `Neighbourhood`) and the routing
  table reading would be duplicated, and the connections and routing table warmed during the
  fetch would be thrown away. A child process (`node --fetch` run by `main.cpp`) avoids the
  singleton but duplicates the same code and adds a second process to supervise.
- **(b) a bootstrap mode of `Node`** [chosen]: the node is constructed as today (transport,
  solver, API object), the transport runs, but the chain is not opened; every message but the
  snapshot replies is dropped; when the snapshot is imported the chain is opened and the node
  continues exactly as a node started on a restored directory. What the evaluation feared ("every
  place that assumes `blockChain_` is initialised") is enumerable and short (6.6): the transport
  reads two atomics of an un-opened `BlockChain` (`lastSequence_`, `uuid_`:
  `neighbourhood.cpp:62-70`, `76-86`, `125-136`; `blockchain.cpp:384-386`, `2484-2486`), both 0,
  which is what a node at genesis reports today; the slots reached from the transport are four
  and are gated by one flag.

### 6.2 Phases

Conditions, checked in `Node::init` before anything touches `db/` (new `Node::needsSnapshot()`):
`[snapshot] fetch=auto`, `csdb::backend::detect(dbPath, false)` finds no database
(`csdb/include/csdb/storage_backend.hpp:17`), `qs/` holds no numbered checkpoint directory,
`db_backend` is `lmdb` or `rocksdb` (a BerkeleyDB node cannot start above block 0: the conversion
rule, `csdb/src/storage_convert.cpp:328-334`), no `--set-bc-top`. A node with `snapshot.mismatch`
in its directory refuses to start (6.8). `snapshot-tmp/` left by an interrupted run is removed.

1. **Transport up, chain closed.** `Node::init` runs its first part (6.6) and returns `true` with
   `bootstrapping_ = true`; `Peer::onRun` calls `Node::run` → `Transport::run` as today
   (`transport.cpp:132-153`): the p2p host joins the network from `hosts_filename`
   (`known_hosts.txt` of the package), neighbours are added, pings flow.
2. **Discover** (fetcher tick every 500 ms on the processor thread, a `cs::Timer` with
   `RunPolicy::CallQueuePolicy` as `PoolSynchronizer`'s, `poolsynchronizer.cpp:101-105`): every
   known peer with bit 24 (`Transport::getKnownPeers`), plus the keys of `sources=` and the
   `sources_file`, are brought into the neighbour table (4.3). Logged every 30 s:
   `SNAPSHOT: N peers serve snapshots (M neighbours), K manifests collected`.
3. **Collect manifests.** One `SnapshotManifestRequest` to each serving neighbour, repeated every
   30 s while the bootstrap lasts (a node that was "building" or "not synced" may become ready).
   A reply is kept when the JSON parses (`SnapshotManifest::fromJson`), `verify()` passes with
   its `node_key`, the `node_key` equals the sender's p2p key (otherwise the manifest is some
   other node's, relayed: dropped and logged), and `chunk_size` is within 4.3's bounds.
4. **Agree** (6.3) as soon as the collected manifests allow it; otherwise wait (6.4).
5. **Fetch** (6.5) the files of the agreed group into `snapshot-tmp/`, piece by piece, verified.
6. **Import and open** (6.7) on a worker thread: `importSnapshot` into `db/`, `qs/<S>`,
   `caches/`; then `BlockChain::init` (quick start from `qs/<S>`), the rest of `Node::init`, and
   `bootstrapping_ = false` on the processor thread. From here the node is a node started on a
   restored directory: forward sync from `S + 1`, backward sync below `S` when it exists.

Time: discovery and the neighbour handshake take 10 s to a minute (`kDiscoveryInterval` 60 s,
`routing_table.h:82`; a VersionRequest round trip per peer); 70 MB from three sources at
2 MB/s each takes about 15 s, 200 MB (with the block stream) under a minute; the import about a
minute (signature checks and writes, as measured for the importer); the quick start loads
375 MB of caches, seconds. A join should take 2 to 5 minutes end to end on mainnet.

### 6.3 Sources and agreement

A source is a p2p peer: its key is the sender of the manifest reply, its address the one in the
routing table (`NodeEntrance.address`, the endpoint Kademlia verified by a ping exchange). The
manifest's signature binds the key to the content; the p2p layer does not bind the key to
anything (section 2), so the independence of sources comes from addresses: sources in the same
/24 (IPv4) or /48 (IPv6) count once. Keys listed in `sources=` are the operator's trusted ones
and count once each whatever their address.

Agreement (the 2026-10-24 rules without GitHub): group by `(seq, head_hash)`, inside by
`(state_digest, settings_digest)`; candidate blocks are the groups with at least `min_sources`
counting sources, highest `seq` first; in the chosen block the digest group with most sources
wins if it reaches `min_sources`, else the next block. Every group is logged as
`SNAPSHOT: block S hash … digest … : N sources (keys …)`. Pure functions over a vector of
`{key, address, trusted, manifest}` in `csnode/src/snapshot_agreement.cpp` (planned in the
2026-10-24 step 5a, never written: new in plan P4).

### 6.4 Too few sources

This is the early-network case: mainnet shows 23 nodes, most official; the community nodes that
serve may be one or two for a while. Rules:

- The fetcher keeps collecting for `fetch_wait` (default 600 s) from the first tick, re-asking
  every 30 s; the 30-s log line names the groups seen and how many sources are missing
  (`SNAPSHOT: 1 source declares block S (need 2): waiting, 7 min left`).
- When `fetch_wait` expires without agreement and `fetch_fallback=sync` (default): the node logs
  at warning level, once, the groups seen and the manual way out, and continues **as a node with
  an empty database does today**: the chain opens, the genesis block is written, forward sync
  from block 0 (a pruned node from block 0 is supported, `all-storage` scenarios). The line:

  ```
  SNAPSHOT: no agreement of 2 community sources after 10 min (seen: 1 source for block S …).
  Syncing from block 0 (about 35 h on a fast machine). To start from the published snapshot
  instead: stop the node and run  ./node --download-snapshot  in this folder, then start it again.
  ```

- `fetch_fallback=wait`: the node keeps waiting, with the line every 5 minutes, until agreement or
  a stop; for operators and images that would rather wait than sync for 35 hours.
- `min_sources=1` with `sources=<key>` pins one trusted node (an operator's own mirror).

**`min_sources` default: 2 for now (question 1 for the user).** With 3 the automatic path would
fail on mainnet until at least three serving community nodes in different /24 networks are
online, which the weekly job will show; a default the network cannot meet is a default of
"sync from 0". With 2, the local pruned node plus one other serving node (or a Docker user who
left serving on) already give an automatic join, and a forger needs two addresses in different
/24 networks instead of three: a cost bar rather than a proof either way (section 11). The
recommendation is 2 until the weekly report shows five or more serving sources, then 3 by a
configuration default change in a release.

### 6.5 Download and verification

- Each file is fetched from the sources of the agreed group that declare the same `sha256` for
  it (two honest nodes do not produce byte-identical `.dat` files, 2026-10-24 section 2); the
  biggest group of sources per file first. Pieces are requested round-robin over those sources,
  at most 2 outstanding per source, 8 in total, each with a 30-s timeout; a `busy` reply delays
  that source by `retry_ms`; a `not synced`/`not serving` reply or a timeout marks the source for
  60 s (twice: for the rest of the bootstrap); a piece whose SHA-256 differs from the manifest's
  marks the source for the rest of the bootstrap and the piece is asked elsewhere; a file with
  no source left is restarted from another `sha256` group; a group with no file source left ends
  the attempt, and the fetcher goes back to 6.3 with the sources it has not marked.
- Pieces are written at their offset in `snapshot-tmp/<name>` (pre-sized), so there is no
  reassembly; a piece already verified on disk is never asked again within the run. Across runs
  `snapshot-tmp/` is discarded (the whole fetch is minutes).
- After the last piece the whole-file SHA-256 is recomputed and must equal the manifest's; then
  `importSnapshot` checks everything again (sizes, chunk and file hashes, checkpoint head and
  digests, blocks) before anything reaches `db/`.
- `fetch_rate_kbps` caps the requests (a token bucket on requested bytes).
- Memory: at most 8 pieces of at most 1 MiB in flight, written to disk on arrival; the manifest
  JSON at most 256 KB per source, at most 64 sources kept.
- Progress: `SNAPSHOT: fetching block S: 37 % (52 of 140 MB), 3 sources, 2.1 MB/s` every 10 % and
  every 30 s.

### 6.6 Bootstrap mode inside `Node`: the exact touch points

`Node::init` (`node.cpp:141-293`) is split into three functions called in sequence by `init()`
today and in two steps in bootstrap mode:

| Part | Lines on cb4d2125 | Bootstrap mode |
| --- | --- | --- |
| `initCommon()`: initial confidants and permanent neighbours, the API object and its signal connections, `solver_->init`, `startDefault`, the serialisation manager binding | 142-166 | run at construction, as today |
| `initChain()`: `blockChain_.init` (both branches), consensus settings, contract rehydration and validation, the start-up digest line | 168-255 | run on the worker thread after the import |
| `initRunning()`: `api_->run()`, transport check, conveyer key, `tryResolveHashProblems`, `initBootstrapRP`, `EventReport::sendRunningStatus` | 257-292 | run on the processor thread after `initChain()`, then `bootstrapping_ = false` |

The five signal connections at 276-281 (`checkPing`, `pingChecked → onPingChecked`,
`sendingTimer_ → processTimer`, `packetFlushed`, `poolSynchronizer_->sendRequest →
sendBlockRequest`) move to the constructor next to the others (102-117), because connecting while
the processor thread emits is a race (section 2). They are inert or gated until the chain is up:

- `Node::onNeighbourAdded` (`node.cpp:384-392`) sends a `RoundPackRequest` when the neighbour's
  round is above ours: `if (bootstrapping_) return;` at its top (the reply would be dropped
  anyway).
- `Node::onPingChecked` (`node.cpp:2134-2145`) requests a round table when a peer is ahead: the
  same one-line gate.
- `PoolSynchronizer::onPingReceived` (`poolsynchronizer.cpp:288-295`) records peers ahead of
  `getLastSeq()` (0): harmless, and useful: when the chain comes up the list is already there.
  `sync()` is only called from round handling, which is dropped.
- `Node::processTimer` runs only once `sendingTimer_` is started inside a round (`node.cpp:4035-4037`).
- `Node::chooseMessageAction` (`node.cpp:2169-2258`): after the `good_` check, `if (bootstrapping_)
  return (type is SnapshotManifestReply or SnapshotPieceReply) ? Process : Drop;`. Requests are
  dropped too: a bootstrapping node does not serve.
- `Neighbourhood::sendVersionReply`/`sendPong`/`isCompatible` read `uuid()` (0) and `getLastSeq()`
  (0): peers see a compatible node at block 0, as they see a node started on an empty database
  today.
- `Transport::run` is unchanged; `Node::run` is unchanged.

Threads: the fetcher state machine runs only on the processor thread (its timer and the two
reply handlers), like `BackwardSync` in its plan; it has no locks. The import and `initChain()`
run on one worker thread started by the fetcher when the files are verified, while the processor
thread keeps draining the inbox and dropping (so the inbox never grows). `initRunning()` and the
flag change are queued to the processor thread (`CallsQueue::instance().insert`) when the worker
is done, so that the switch from "drop everything" to "process" happens on the thread that
processes. The worker thread is joined in `Node::stop`.

### 6.7 Import and hand-over

The worker thread calls `importSnapshot(snapshot-tmp/, manifest, ".", dbPath, backend, &stop_)`
(`snapshot_importer.hpp:32-34`), which writes `qs/<S>`, the database with the blocks of the
stream (one block, `S`, once `snapshot_blocks = 1`), `caches/hashdb` and `caches/seqdb`, and
`snapshot.json` with the sources used appended by the fetcher (`sources`: keys and addresses, for
the operator and the weekly job). On failure (a file at fault is named): the importer has removed
what it created; the sources of that file's `sha256` group are marked and the fetcher restarts
from 6.3 (at most 3 attempts per bootstrap, then 6.4's fallback). On success: `initChain()`: the
quick start finds `S` in `seqdb` (`isCheckpointOnChain`, `blockchain.cpp:100`), compares the
digest file with the loaded caches (`:105-116`), logs `STATE DIGEST #S … checkpoint loaded`
(`:125`), opens the storage from `S + 1` (`:229-233`), rebuilds the index from the stored blocks
(`:273-280`), checks the hash databases (`:284-295`); then 6.8; then `initRunning()`.

### 6.8 Verification at start-up (kept from 2026-10-24, 6.5)

When `snapshot.json` is present and `snapshot.verified` is not, the digest the quick start
computed from the loaded caches must equal the manifest's `state_digest` and `settings_digest`
(the quick start already refuses a checkpoint whose own `state_digest.bin` differs, and the
importer already checked that file against the manifest, so this is a belt over braces that
costs nothing): `snapshot.verified` is written, or `snapshot.mismatch` and the node stops with
exit code 4 and refuses to start while the file exists. The Docker entrypoint keeps its handling
of `snapshot.mismatch` (`packaging/docker/entrypoint.sh:111`) and drops its own digest wait
(`:156-190`).

### 6.9 Stop and crash

`Node::stop` (`node.cpp:340-352`): `fetcher_->stop()` (sets the stop flag the importer checks
between blocks, `snapshot_importer.cpp:84-86`, joins the worker) before `poolSynchronizer_->stop()`.
`Node::destroy` (`:354-368`): `blockChain_.close()` on a storage that was never opened must
return at once: `close()` calls `tryFlushDeferredBlock`, writes `qs/0` and checks
`storage_.flush()` (`blockchain.cpp:1350-1390`); a guard `if (!storage_.isOpen()) { stop_ = true;
return; }` at its top (plan P5). A kill during the fetch leaves `snapshot-tmp/`, removed at the
next start; during the import the importer's own cleanup runs at the next start too
(`--import-snapshot` has the same case). `db/.lock` is taken by the importer before it writes.

## 7. Blocks

- **Now (until backward sync is merged):** a snapshot carries `blocks.bin.lz4` with `F..S`
  (`snapshot_blocks` = 101,000, `snapshot_builder.cpp:290-291`), about 140 MB lz4 on mainnet,
  moved as pieces like every other file. Nothing new is needed for the blocks; the cost is two
  thirds of the bytes.
- **Then:** `snapshot_blocks` defaults to 1: the stream holds block `S` alone (the importer
  already writes any count; `config.cpp:1073` must accept 1, today the floor is 1000). The node
  starts with blocks `S..` and backward sync fills `F..S−1` from every neighbour, official nodes
  included, exactly as its design says, with one addition: **a floor**. A pruned node backfills
  down to `F = S − (checkpoint_every + prune_keep_blocks)` (so that it holds what a node that
  pruned by itself holds, and the responder rule of #61 is met for the range it serves); a full
  node continues to 0. In backward sync's terms (its design, section 1): "a pruned node never
  backfills" becomes "a pruned node backfills to its floor"; `BackwardSync::isActive()` and the
  stop condition in `drain()` take the floor (plan BS-F).
- **Ordering of the two plans.** The p2p snapshot steps come first (P0-P6): they work with the
  block stream and give the whole feature without waiting for backward sync; backward sync's
  steps 1-2 (storage and index, no transport) can be done by a second worker in parallel; step 3
  (the `BackwardSync` class) after P5 so that both transport-side state machines are reviewed
  against the same `Node`; then the floor (BS-F) and the switch to `snapshot_blocks = 1` (P10)
  with the `snapshot-p2p-state-only` scenario. Both forms import with the same code and both can
  be served at the same time (`first_block` says which); the GitHub snapshot shrinks when the
  default changes.
- Until the backfill reaches `F`, the API's history below the lowest stored block is incomplete,
  as on a pruned node today (backward sync's section 6, #46, #62); the index is updated online.

## 8. Consensus safety and load

Nothing consensus-relevant changes: no block, hash, signature or round rule; no change to how a
block is chosen or applied; the new messages are never seen by official nodes.

Load on a serving node that is a confidant: each piece is one `pread` and one packet on the
processor thread (about a millisecond at 512 KiB; `getBlockRequest` already does more per
request, `node.cpp:1870-1879`, `1954`). The bytes go on the connection to the requester, which is
not in consensus; what that FIFO also carries is the serving node's **broadcasts** to that peer
(round tables, stages, transaction packets go to the k nearest nodes, `host.cc:175-182`,
`121-128`), so one piece delays one hop of a broadcast by its transfer time: 0.4 s at 10 Mbit/s,
40 ms at 100 Mbit/s, for one of 5 replication paths (`routing_table.h:77`). With the halved
budget of 5.3 and 512 KiB pieces this is below the round's time budget; it is measured, not
assumed: the `snapshot-p2p-trusted` scenario (13) runs joins against a network whose serving
nodes are confidants and compares the block rate and the round time distribution with and
without joins (the resource sampler of #68 gives the rate; the round times come from the logs).
If the testnet shows an effect, the next knobs are: a smaller chunk (256 KiB), `serving_trusted`
sources used only when no other exists (the fetcher already prefers others), or no serving while
trusted (a one-line policy change; the user's decision today is to serve).

Upstream bandwidth: `serve_rate_kbps` (2000 default in the packages). Disk: two snapshots during
a build (`snapshot/<S>` and `<S+1>.tmp`), about 0.4 GB with the block stream, 0.15 GB without.

## 9. GitHub: manual download, the workflow, Docker

- **`node --download-snapshot [URL]`** (plan P6), the only code in the node that speaks HTTP, and
  only when the operator runs it: `URL` defaults to
  `https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/snapshot.json`. It refuses
  to run next to a running node or on a non-empty database (as `runImport`,
  `snapshot_export.cpp:150-160`), runs `curl -fsSL --max-time 60 -o snapshot-tmp/snapshot.json
  URL` through `cs::Process` (`lib/include/lib/system/process.hpp:46-90`, as the contract
  executor is launched), parses and `verify()`s the manifest (the signature is the workflow
  node's own key: the check proves consistency, the trust is HTTPS and the release page, as
  today for `latest.json`), refuses a manifest beyond the limits, downloads each file whole with
  `curl -fL -C - --max-time 3600 -o snapshot-tmp/<name> <base>/s<seq>-<name>` (assets of one
  release share a namespace, so the workflow publishes files as `s<seq>-<name>`; the manifest
  keeps plain names), checks size, chunk and file hashes, then `importSnapshot` and exit 0, or
  exit 2 with the reason and `snapshot-tmp/` left for a retry (`-C -` resumes). No `curl` on the
  `PATH`: exit 2 with `install curl, or download the files by hand and run --import-snapshot`.
  The node's normal start never calls it.
- **`snapshot.yml`** keeps running every 12 hours: after the node reached the head and the checks
  passed, `./node --export-snapshot out/` (the latest periodic checkpoint, so the published block
  is a checkpoint block that nodes can agree with), the publish job checks the manifest and
  every file's size and hash, uploads `snapshot.json` (clobber) and `s<seq>-<name>`, keeps the
  files of the 3 newest sequences, skips when the checkpoint is the one the run started from.
  Format 1 (`latest.json`, `credits-pruned-<seq>.tar.zst`) is published alongside for one release
  for the previous image, then removed with `snapshot-make.sh`, `snapshot-restore.sh` and
  `digest_offline.sh`. When `snapshot_blocks` becomes 1 the published snapshot shrinks to about
  70 MB with no workflow change (the exporting node's configuration decides).
- **Docker** (user's decision): on a first start with an empty data directory and
  `CREDITS_SNAPSHOT=github` (the default), the entrypoint runs `node --download-snapshot
  "$CREDITS_SNAPSHOT_URL"` in the data directory, logs its result, and starts the node whatever
  the result: with a restored database the node starts from it; without one (GitHub unreachable,
  a failed check) the node's own `fetch=auto` tries the neighbours (6.2) and falls back as 6.4
  says. `CREDITS_SNAPSHOT=p2p` skips the download (neighbours only), `off` sets `fetch=off` (sync
  from 0). `CREDITS_SERVE_SNAPSHOT=on` (default) writes `serve=on`; `CREDITS_SNAPSHOT_MIN_SOURCES`
  maps to `min_sources`. The entrypoint's restore-by-script path (`entrypoint.sh:130-138`) and
  digest wait (`:156-190`) go; its `snapshot.mismatch` refusal (`:111`) stays; `.restore-incomplete`
  handling (`:41-45`, `:138`) goes with the script (the importer is atomic from the node's point of
  view: nothing is in `db/` until it finishes). The image no longer needs `zstd`, `python3` for
  the restore (python stays for the tests).

## 10. Discovery and the weekly job

- **In the node:** discovery is the routing table (4.1): no probing, no port list. `sources_file`
  (`snapshot_sources.txt`, `ip:port key` lines, shipped in the packages) and `sources=` only say
  which peers to bring into the neighbour table first; everything with bit 24 is found anyway
  once the routing table has it.
- **`Node::dumpKnownPeersToFile`** (`node.cpp:135-139`, at stop) also writes `known_peers.json`
  next to `known_hosts.txt`: `{id, ip, port, version, platform, capabilities}` per known peer.
  `known_hosts.txt` keeps its two-column format (older builds parse it).
- **`node --survey-snapshots [seconds]`** (plan P8): runs the bootstrap's discovery and manifest
  phases only (no agreement, no fetch, no import), for the given time, in a directory that may
  hold a database or not, prints every manifest group `(seq, head_hash, state_digest,
  settings_digest)` with its sources and `created`, and exits. It is the bootstrap state machine
  with `dryRun = true`.
- **The weekly job** (`known-hosts.yml`, `run-weekly.sh`): after `collect.py`, run
  `--survey-snapshots 600` with the job's node directory, turn `known_peers.json` into
  `snapshot_sources.txt` (peers with bit 24 that answered a manifest that verified, at most 3 per
  /24, the helper of `collect.py`) and the survey into `snapshots.md` (the groups seen, the
  record the 2026-10-24 section 7.1 asked for, and the count of serving sources that 6.4's
  `min_sources` question depends on). Both go to the release `known-hosts` and to the pull
  request on `packaging/mainnet/`. `ci/known-hosts/probe_snapshots.py` of the 2026-10-24 plan is
  not written.

## 11. Security

| Attack | Defence |
| --- | --- |
| A forged chain or state offered by sources | `min_sources` independent sources (one per /24 or /48, keys bound to content by the signature) must declare the same `(S, head_hash, digests)`; every block of the stream is chained to `head_hash` and its signatures checked (`BlockVerifier`); the node then continues from `S` with the network, which only works if `head_hash` is the network's block. Without GitHub there is no anchor outside the network: `min_sources` is a cost bar (addresses in distinct /24 networks), not a proof; an operator who wants a proof pins `sources=` to keys they trust or imports the GitHub snapshot by hand. The 2026-10-24 design's caveat stands: honest nodes can disagree by a fee (#83), a minority group is logged, not fatal |
| Sybil keys | Keys are free and unauthenticated in p2p (section 2); counting is by address; `known_peers.json` and the weekly job show how many distinct serving addresses exist; `sources=` keys are counted by key because the operator chose them |
| Garbage, slow or huge replies | Every reply is size-checked before parsing (4.2); pieces are hashed on arrival and a bad one marks the source; 30-s timeouts; at most 8 pieces in flight; nothing beyond one piece stays in memory; a manifest JSON is at most 256 KB and parsed by `fromJson` with its own shape checks and limits; `chunk_size` bounded |
| A request flood on a serving node | Stateless answers, per-peer and global token buckets, `serve_max_peers`, a 64-entry peer table; a request from a node not serving is one map lookup; names and chunk indexes are validated against the loaded manifest before any file access; no path comes from the network |
| Memory on the p2p receiver | The library allocates what the header announces (section 2) for every message type: the cap of 4.4 (plan P9, flagged) is the fix; until then the node-level limits bound what is parsed, not what is allocated, as for every other message today |
| Denial of service on a joining node by a majority of bad sources | The fetch ends after 3 attempts or `fetch_wait`; the node syncs from 0 or waits (6.4); nothing of a bad attempt reaches `db/` |
| Path traversal, file disclosure | Provider: only files it listed under `snapshot/<S>/`; importer: the fixed name table; keys, `config.ini`, logs, `known_hosts.txt` never read by the provider nor written by the importer |
| A compromised GitHub release | Reaches only operators who run `--download-snapshot` (and the Docker first start, which is the user's choice for the image); the node checks the manifest's internal consistency and the chain from `head_hash`, and the network refuses to continue from a wrong block |
| The unauthenticated-key and frame-size findings | Reported privately to the official developers (decision of 2026-10-25, 15.3 of the serving design); the frame cap is plan P9 in our fork |

What the serving node exposes: its key, address and last block are already visible to every peer
(`known_hosts.txt`, pings); the snapshot is public chain state.

## 12. Configuration

`[snapshot]` (`client/config/config.hpp:76-91`, `config.cpp:1061-1089`, read by the observer;
every key but `fetch*` takes effect while the node runs):

| Key | Default | Meaning |
| --- | --- | --- |
| `serve` | `off` (binary); `on` in the packages' `config.ini` and in Docker | build snapshots after each periodic checkpoint and answer snapshot requests over p2p; also sets bit 24 at start |
| `serve_rate_kbps` | `2000` | upload cap for pieces, all peers together; `0` = unlimited |
| `serve_max_peers` | `8` | peers served within 30 s; more get busy |
| `snapshot_blocks` | `101000`, later `1` | blocks before the checkpoint in a built snapshot (range 1..10,000,000) |
| `fetch` | `auto` | `auto`: a start with an empty database (lmdb or rocksdb) fetches from the neighbours; `off`: never |
| `fetch_wait` | `600` | seconds to look for agreement before the fallback |
| `fetch_fallback` | `sync` | `sync`: sync from block 0 after `fetch_wait`; `wait`: keep waiting |
| `min_sources` | `2` (question 1) | independent sources that must agree |
| `sources` | empty | base58 keys of trusted nodes, counted once each whatever their address |
| `sources_file` | `snapshot_sources.txt` | `ip:port key` lines of known serving nodes, brought into the neighbour table first; missing file = none |
| `fetch_rate_kbps` | `0` | download cap; `0` = unlimited |

Removed from #88's set: `serve_port`, `serve_bind`, `serve_max_connections`, `probe_known_hosts`,
`github_manifest`, `fallback`, `fetch_connections` (a warning for a key that no longer exists,
then ignored). Docker variables: section 9.

## 13. Tests

**Unit** (`tests/csnode`): message codecs (round trip, every limit, truncated payloads);
capability byte encode/decode; `SnapshotProvider` with a fake clock (ready/not serving/not
synced/building, bad name, bad chunk, last chunk length, busy and `retry_ms`, per-peer and global
buckets, the trusted halving, the peer table cap); agreement (threshold, /24 and /48 collapsing,
`sources=` keys, highest block first, digest majority, nothing chosen); the fetcher state machine
with a fake transport and fake sources (discovery from capability bits, manifests with bad
signatures or another node's key dropped, agreement reached late, pieces round-robin with the
outstanding limits, a corrupt piece marks and refetches, a stalled source times out, busy
backoff, a source that leaves the neighbour table is not asked, `sha256` group restart, the
whole-file check, `fetch_wait` expiry with both fallbacks, 3 failed imports, stop in every
phase); `--download-snapshot` against a local `python3 -m http.server` directory (whole files,
resume, a tampered chunk, a manifest beyond the limits, no `curl` on the `PATH`).

**Private network** (`robustness.yml`, `ci/testnet/run.sh`; state digests must match at the end;
`SNAPSHOT_SERVE_NODES=a,b,c` gives those nodes `serve=on` with `checkpoint_every=1000`;
`JOIN_FETCH=p2p` starts the joining node `NODES + 1` with an empty database, `fetch=auto`, the
network's hosts file and the given `JOIN_MIN_SOURCES`, `JOIN_WAIT`, `JOIN_FALLBACK`):

| Scenario | What happens | Must show |
| --- | --- | --- |
| `snapshot-p2p-join` | n4 (full BerkeleyDB), n6 (pruned LMDB), n7 (full RocksDB) serve; n8 joins, `min_sources=3` | three manifests agree; n8 logs the groups, fetches from all three, imports, quick-starts from `qs/<S>`, `snapshot.verified`, reaches the target; the serving nodes log `served block S to n8` |
| `snapshot-p2p-few` | only n6 serves; `min_sources=2`, `JOIN_WAIT=60`, `fallback=sync` | n8 waits 60 s with the waiting line, logs the manual-import line, slow-starts from block 0 and reaches the target |
| `snapshot-p2p-wait` | as `-few` with `fallback=wait`; n7 switched to `serve=on` after 90 s (config edit, observer) | n8 waits, then agrees once n7 serves (after n7's next checkpoint), joins |
| `snapshot-p2p-forged` | a test build makes n7 serve a checkpoint with an altered balance, then pieces with a flipped byte | the altered digest is a logged minority; the bad piece is refetched from n4 or n6 and n7 marked; n8 completes |
| `snapshot-p2p-dropped` | n8 killed in the middle of the fetch, and once during the import, then started again | `snapshot-tmp/` removed, nothing in `db/`, the fetch redone, n8 completes |
| `snapshot-p2p-trusted` | the serving nodes are initial confidants; two joins in sequence under contract load (`contracts-*` harness) | block rate within 10 % of the run without joins; round time p95 within 20 %; no fork, no stage timeouts attributable to the serving nodes |
| `snapshot-p2p-limits` | n6 alone, `serve_rate_kbps=1000`, `serve_max_peers=1`; n8 and n9 join | the rate measured at n8 is within 20 % of the cap; n9 logs busy answers and completes after n8 |
| `snapshot-p2p-stress` | all initial confidants serve (`serve=on`, half budget while trusted); four joiners n8-n11 start together under contract load, each with a different fault: n8 clean, n9 with `rate 2mbit` (netem on everything it sends and receives), n10 with `loss 5% delay 150ms`, n11 killed every 40-90 s during the fetch and the import and started again; one serving confidant with `rate 4mbit` on its own traffic and one with `serve_rate_kbps=500`; a test client (`snapshot_flood`) asks pieces from every serving node without honouring `busy`/`retry_ms`, drops connections in the middle of replies and sends malformed requests; compared with a baseline run with the same faults and no joiners or flood client | consensus unaffected: block rate within 10 % and round time p95 within 20 % of the baseline, no fork, no stage timeout or writer change attributable to the serving nodes, consensus messages to the slow confidant within the 10-s deadline; the flood client throttled (busy answers, its bucket logged); each serving node at most 64 MiB of resident memory above its idle level; n8-n11 complete (or n9 falls back to `sync` after `fetch_wait` with the logged line); digests equal |
| `snapshot-p2p-mixed` | even nodes on the official binary (`BIN_ALT`); serving nodes odd; n8 joins | n8 never asks an even node; the even nodes' logs have no `Unknown message type`; n8 completes; the blocks below `S` (when state-only) come partly from even nodes |
| `snapshot-p2p-state-only` (after backward sync) | `snapshot_blocks=1` on the serving nodes; n8 pruned joins; n9 full joins | both restore a 1-block snapshot and reach the head; n8 backfills to its floor and stops; n9 backfills to 0; histories equal n1's at the end (backward sync's `compare_histories.py`) |
| `snapshot-download` | a `python3 -m http.server` serving the layout of the release (`snapshot.json`, `s<seq>-<name>`); n8 runs `--download-snapshot http://…` then starts | restored, verified; a tampered file fails with exit 2 and nothing in `db/` |
| `snapshot-join` (existing) | as today, `--export-snapshot` / `--import-snapshot` | unchanged |

**Scale** (`scale.yml`, `scenario` input, `.github/workflows/scale.yml:34-35`, `216-226`): the
`snapshot-p2p-join`, `-trusted` and `-stress` variables (`-stress` with eight joiners on 51) on 33 and 51 nodes (`scale-t7-n51_33-d200-l10`),
with the serving nodes among the low-stake and among the trusted ones; recorded: join time, bytes
per source, round time percentiles with and without joins.

**Docker:** `test-entrypoint.sh` covers the first start with `CREDITS_SNAPSHOT=github` (the stub
node sees `--download-snapshot`), `p2p` (no download), `off`; `docker.yml`'s mainnet smoke test
starts with the default (GitHub download, format 2) once `snapshot.yml` publishes format 2.

**Mainnet** (after the testnet runs): the local pruned node with `serve=on` and the 2 MB/s cap;
a fresh container on this host with `CREDITS_SNAPSHOT=p2p`, `min_sources=1`, `sources=<the local
node's key>`: the first p2p join on mainnet; then the weekly job's first `snapshots.md`; the
package default `serve=on` ships after that.

## 14. Measurements to make

- Piece throughput per source on the testnet runners and on mainnet (bytes/s at 512 KiB
  pieces, 2 outstanding); the effect of 256 KiB and 1 MiB on join time and on round times
  (`snapshot-p2p-trusted`), to confirm 512 KiB.
- Join time end to end (discovery, manifests, fetch, import, quick start) on the testnet and on
  mainnet; where the time goes.
- The size of `known_peers.json`'s serving set on mainnet over the first weeks (the `min_sources`
  decision).
- Export time and lz4 sizes per file (still not recorded in the 2026-10-24 design's section 12).

## 15. Decisions on the open questions (user, 2026-10-25)

1. **`min_sources`**: 2 now; 3 once the weekly job (section 10) shows at least five serving
   sources on mainnet. The change is a default in the packages, not a protocol change.
2. **`fetch_fallback`**: `sync` after `fetch_wait=600` s. Before the slow start the node logs
   one line naming `--download-snapshot` (and the Docker variable) as the faster way.
3. **p2p frame cap**: added in our fork of the p2p submodule at 256 MiB (plan P9); a frame that
   announces more is dropped and the connection closed, before any allocation.
4. **Trusted nodes serving**: they keep serving, at half budget, while they are confidants.
   The `snapshot-p2p-trusted` and `snapshot-p2p-stress` numbers decide whether this holds; if
   they fail, the knobs of section 8 are tried and the question comes back to the user.
5. **`MINOR_NODE_VERSION` in `host_data` bits 32-47**: included (plan P2).
