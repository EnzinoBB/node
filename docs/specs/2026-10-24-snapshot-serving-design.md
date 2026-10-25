# Snapshots served by community nodes: design

Status: draft for review, 2026-10-24, against master b5f74ecd (4.2.532-community.11). The
implementation plan is `snapshot-serving-plan.md`, next to this file. Every file:line reference
below was checked on b5f74ecd. It continues the section "Next phase: snapshots served by community
nodes" of `docs/specs/2026-10-16-docker-and-pruned-snapshots-design.md`.

## Goal

A new node gets its starting snapshot from other community nodes, with the GitHub release as one
more source and as the fallback, so that nobody has to fetch it from the internet when the network
itself can provide it. Requirements added on 2026-10-24:

1. Both pruned and full nodes can expose snapshots.
2. It works for an operator who runs only the downloaded binary: the feature lives in the node,
   is switched on and off in `config.ini` while the node runs, and needs no restart.

Out of scope: a full history for full nodes (that is backward sync, `docs/specs/2026-10-17-backward-sync-design.md`
on `origin/docs/backward-sync`); snapshots of the official node's BerkeleyDB folder as such.

## Decisions in short

| Question | Decision |
| --- | --- |
| Where | In the node binary: a `[snapshot]` section of `config.ini`, read by the existing configuration observer; a snapshot builder, an HTTP server and an HTTP client in `csnode`. The Docker side service of the 2026-10-16 direction is dropped (section 1). |
| Protocol | Plain HTTP/1.1 on its own TCP port (default 9511), `GET`/`HEAD` of a manifest and of fixed-name files with `Range`. No p2p change; no TLS (integrity comes from hashes and signatures, nothing is secret). |
| What a snapshot is | Format 2: the node's own periodic checkpoint `qs/<S>` plus the blocks of the last `checkpoint_every + prune_keep_blocks` sequences before `S`, as a canonical block stream, plus the contract data; each file compressed and hashed per 4 MiB chunk; a manifest signed with the node's key. Built by the node itself after each periodic checkpoint, on any backend, pruned or full (section 2, 4). |
| Digest of the checkpoint | Computed and stored when the checkpoint is saved, logged again when a checkpoint is loaded at a quick start, before the following blocks are applied (section 3). This closes the open problem of the 2026-10-16 design. |
| Trust model | Sources are identified by their node key, the same key as in `known_hosts.txt`; manifests are signed with it. A snapshot is used when at least `min_sources` sources (default 3, GitHub counts as one, at most one per /24) declare the same block, block hash and state digest; the restored node refuses to run on a state whose digest is not the declared one; every imported block is checked by hash chain from the agreed block hash and by its signatures (sections 6, 7). |
| Digest divergence between honest nodes | Agreement is first on `(S, head_hash)`, then on the digest group; the largest group wins if it reaches the threshold, or if GitHub is in it; everything is logged and the weekly job publishes the groups seen on mainnet (section 7.1). |
| GitHub | Keeps publishing, in both formats for one release, then in format 2 only; it moves from head snapshots to checkpoint snapshots so that it can agree with nodes (section 10). |

## What exists today (b5f74ecd)

- **Snapshot v1.** `packaging/docker/snapshot-make.sh` packages a *stopped* pruned LMDB node:
  `db/lmdb` and `caches/*` (but `poolcachedb`) with `mdb_copy -c`, each copy checked by
  `lmdb-check.sh`, `qs/` as it is; `latest.json` with `seq`, `state_digest`, `wallets`,
  `checkpoint`, `created`, `node`, `file`, `size`, `sha256`, `run`. The digest is the last
  `STATE DIGEST` line of the node's logs, that is the digest of its head, obtained by an offline
  start (`ci/snapshot/digest_offline.sh`). `snapshot-restore.sh` downloads, checks the SHA-256,
  extracts into `.restore-tmp` and moves the directories into place; the Docker entrypoint
  (`packaging/docker/entrypoint.sh:128-139`) runs it on an empty volume and then compares the
  node's start-up digest with the manifest (`entrypoint.sh:152-190`), refusing to start again
  after a mismatch (`entrypoint.sh:117`). `.github/workflows/snapshot.yml` refreshes the release
  `pruned-snapshot` every 12 hours, with the balance cross-check of `ci/snapshot/check_node.py`.
- **The published snapshot** `credits-pruned-186460890.tar.zst` is 278 MB compressed. Inside
  (measured on the local copy): `db/lmdb/data.mdb` 356 MB (tables `blocks` 161,891 entries,
  81,264 pages; `seq_no` 3,450 pages; `contracts` 2,344 entries, 2,113 pages, about 8.6 MB);
  `qs/186300000` and `qs/0`, each about 375 MB: `walletscache.dat` 182 MB, `apihandler.dat`
  67 MB, `blockchain.dat` 63 MB, `smartcontracts.dat` 59 MB, `walletsids.dat` 6.6 MB,
  `roundstat.dat` 230 KB, `tokens.dat`, `consensus_pending.dat`, `head.bin`,
  `quick_start_hashes.dat`, `schema_version`, `sentinel.bin`; `caches/hashdb` 16 MB,
  `caches/seqdb` 8.7 MB, `caches/indexdb` 12 KB. No `roundstat.dat` or `smartcontracts.dat`
  outside `qs/`: the top-level copies in `snapshot-make.sh` are vestigial.
- **The state digest** (`csnode/src/statedigest.cpp:111-128`, `multiwallets.cpp:98-118`) is a
  ristretto255 multiset hash over every wallet with a non-zero state: key, balance, delegated
  amount, delegation sources and targets. It is order-independent and incremental. `Node`
  logs it at every 1,000th stored block and at start-up after the whole database is read
  (`csnode/src/node.cpp:111-113`, `254-255`, `4470-4481`): on a quick start the line names the
  head, not the checkpoint.
- **Checkpoints.** A periodic checkpoint `qs/<S>` is saved live when `S % checkpoint_every == 0`
  (`csnode/src/blockchain.cpp:1012-1043`; during a slow start `409-432`), once block `S` is on
  disk and before the next block reaches the caches, so its caches hold exactly block `S`. It is
  never modified afterwards; `pruneCheckpoints(checkpoint_keep)` removes the oldest ones
  (`blockchain.cpp:1034`); `qs/0` is rewritten at every clean stop (`blockchain.cpp:1300-1352`).
  Its files and the loading order are in `caches_serialization_manager.cpp:84-92`, `392-460`;
  `head.bin` holds the sequence, head hash and previous hash. A quick start trusts a checkpoint
  only if its head is on the node's chain (`blockchain.cpp:84-118`, `cs::isCheckpointOnChain`,
  which looks the sequence up in `caches/seqdb`), then reads the following blocks from the
  database. The transaction index is rebuilt after a quick start when it is empty
  (`blockchain.cpp:143-153`); the block hash databases are rebuilt when they do not match the
  stored range (`blockchain.cpp:266-279`); restored contract states are validated against the
  chain (`node.cpp:213`, `250`).
- **Configuration at run time.** `cs::config::Observer` re-reads `config.ini` every
  `DEFAULT_OBSERVER_WAIT_TIME` (5 minutes, `client/config/config.hpp:34`,
  `client/config/observer.cpp:38-66`) and swaps the configuration in `cs::ConfigHolder` when it
  differs (`client/src/peer.cpp:111-117`). The Thrift API threads already use this: each loop
  reads the port from the current configuration, and a port of 0 is checked again every 10 s
  (`api/src/csconnector.cpp:36`, `99-150`). The node's key pair is in the configuration;
  `cscrypto::generateSignature` and `verifySignature` are the Ed25519 primitives
  (`node.cpp:578`, `1199`).
- **Blocks and backends.** `csdb::Database` gives raw block bytes by sequence and by hash and
  stores them with `put(hash, seq, bytes)` on every backend (`csdb/include/csdb/database.hpp:39-43`);
  contract data has `updateContractData`/`getContractData`/`forEachContractData` (`57-63`).
  `Storage::pool_load(seq)` parses a block; `Pool::to_binary()` returns the stored bytes.
  Conversion to BerkeleyDB refuses a database whose first block is above 0
  (`csdb/src/storage_convert.cpp:328-334`).
- **Dependencies.** Boost 1.83 built from source in CI (`.github/workflows/build.yml:18`, `56-68`,
  static), so Boost.Asio and Boost.Beast (header-only) are available; libsodium (SHA-256,
  Ed25519, ristretto255); lz4 (`third-party/lz4`, `csnode/include/csnode/compressor.hpp`);
  `boost::process` through `lib/include/lib/system/process.hpp:46-82` (the contract executor is
  launched with it). No HTTP, TLS or zstd library is linked today.
- **Discovery.** `.github/workflows/known-hosts.yml` and `ci/known-hosts/run-weekly.sh` run a node
  for 25 minutes and `ci/known-hosts/collect.py` lists the peers that served old blocks, at most
  3 per /24, with their keys; the list goes to the release `known-hosts` and to a pull request on
  `packaging/mainnet/known_hosts.txt`. The node rewrites its own `known_hosts.txt` at stop with
  `ip:port key` lines (`client/config/config.cpp:223-232`).
- **Lessons of 2026-10-24.** `caches/seqdb` of the local node had a page leak: `mdb_copy -c`
  failed with `MDB_INCOMPATIBLE` while the data was whole (memory note). Replaying some contracts
  is not reproducible (official behaviour, #83): a node that syncs can hold a balance that
  differs by a fee from other nodes, and mainnet nodes disagree among themselves on such wallets
  (7 against 13 of 20 nodes on one wallet; 80 random wallets matched).

## 1. Where the feature lives

**Decision: in the node.** A `[snapshot]` section of `config.ini`; three new parts in `csnode`:
the builder (section 4), the server (section 5) and the fetcher (section 6); one command-line
pair for scripts and CI: `--export-snapshot <dir>` and `--import-snapshot <manifest file or URL>`.

Why not the alternatives:

- **A side service in the container (the 2026-10-16 direction), or a bundled script started by
  the node.** It needs Docker, or Python 3 and the LMDB tools on the host; a binary-only
  operator does not have them. A script cannot read the node's state: it would copy LMDB
  environments while the node runs, which is where the page leak of 2026-10-24 bit (`mdb_copy
  -c` refused a whole database), and it cannot know the digest of the checkpoint it copies. It
  could not be switched on from `config.ini` without something that watches the file. And the
  fetching side (agreement on signed manifests, chunked download, verification) would be a
  second implementation in another language.
- **A Thrift service next to the API.** The node already runs `TThreadedServer`s, so a
  `SnapshotGet(file, chunk)` service would reuse that infrastructure and the Python test client.
  Against it: a Thrift endpoint is not usable with `curl`, a browser or a reverse proxy; it
  carries no `Range`, `Content-Length` or caching semantics; and GitHub, the fallback source,
  speaks HTTP anyway, so the client needs HTTP regardless. The connection and bandwidth limits
  the operator needs are easier to express on a server the node owns completely.
- **HTTP with Boost.Beast, header-only, already in the Boost build.** No new dependency for the
  server and the client. HTTPS to GitHub needs TLS, which the node does not link: the GitHub
  source is fetched through the system `curl` when it is present (as the executor is launched
  through `boost::process`), and skipped with a log line when it is not. Adding OpenSSL to the
  static build is the alternative, listed as an open question (section 14).

The snapshot made by the CI workflow is the one the node builds for itself (`--export-snapshot`),
so the published snapshot is made by the code every node runs, and the scripts of format 1 go
away after one release of overlap.

## 2. Snapshot format 2

A snapshot of checkpoint `S` is a directory of fixed-name files and a manifest. Names contain no
`/`, so that they can be GitHub release assets as they are.

| File | Content | Size (mainnet, 2026-10-24) |
| --- | --- | --- |
| `snapshot.json` | the manifest below | < 64 KB |
| `blocks.bin.lz4` | the blocks `F..S` in ascending order, each as `u32 seq, u32 len, bytes` (little-endian, bytes = the stored block, `Pool::to_binary()`), lz4-compressed as one frame | 356 MB raw, 187 MB with zstd -3; lz4 to be measured (plan step 3) |
| `contracts.bin.lz4` | every contract-data record: `u32 klen, key, u32 vlen, value`, sorted by key | about 9 MB raw |
| `qs-<name>.lz4` | each file of `qs/<S>/`: `walletscache.dat`, `walletsids.dat`, `smartcontracts.dat`, `blockchain.dat`, `roundstat.dat`, `tokens.dat`, `apihandler.dat`, `consensus_pending.dat`, `head.bin`, `quick_start_hashes.dat`, `schema_version`, `sentinel.bin`, `state_digest.bin` (section 3) | 375 MB raw, 35 MB with zstd -3 (16x); lz4 to be measured |

`F = max(first stored block, S − snapshot_blocks)`, `snapshot_blocks` default 101,000
(`checkpoint_every + prune_keep_blocks` of the pruned package), so that a restored pruned node
holds the same range as one that pruned by itself. Hash databases (`caches/hashdb`, `seqdb`) and
the transaction index are not shipped: the importer rebuilds the first two while it writes the
blocks and the node rebuilds the index after the quick start (section 6.4). Nothing else is
shipped: no keys, configuration, logs, `known_hosts.txt`, ban lists, `poolcachedb`, `qs/0`.

**Manifest** (`snapshot.json`), written by hand (flat JSON with unquoted numbers; read with
`boost::property_tree`):

```json
{ "format": 2, "seq": 186300000, "head_hash": "…64 hex…", "state_digest": "…64 hex…",
  "wallets": 147463, "first_block": 186199000, "created": "2026-10-24T05:00:00Z",
  "node": "4.2.532-community.12", "node_key": "<base58>",
  "chunk_size": 4194304, "compression": "lz4",
  "files": [
    { "name": "blocks.bin.lz4", "size": 0, "raw_size": 0, "sha256": "…",
      "chunks": ["…", "…"] },
    { "name": "qs-walletscache.dat.lz4", "…": "…" }
  ],
  "signed": "credits-snapshot/2 186300000 <head_hash> <state_digest> <files_hash>",
  "signature": "…128 hex…" }
```

`files_hash` is the SHA-256 of the lines `name size sha256\n` in manifest order; `signature`
is the Ed25519 signature of the `signed` line's bytes with the node's private key. A client
verifies the signature with `node_key`, recomputes `files_hash` from the file entries, and uses
only files whose names are in the fixed table of section 6.4. Chunks are SHA-256 of each
`chunk_size` piece of the compressed file (the last one shorter).

Why per-chunk hashes but no cross-source chunk mixing by default: two honest nodes do not produce
byte-identical files. Blocks are stored as received and a node may hold a block with more
signatures than another for the same hash (`BlockChain::updateLastBlock` keeps the block with
the most signatures, #80); the `.dat` files are serialized from in-memory containers. So each
file is downloaded from sources that declare the same `sha256` for it (often several: nodes
restored from the same snapshot serve identical `qs-*` files until they make their own
checkpoint), and a chunk that fails its hash is fetched again from another source of that group
or, failing that, the file is restarted from another group. The agreement that matters is on
`(seq, head_hash, state_digest)`, not on bytes.

## 3. The digest of a checkpoint

The 2026-10-16 open problem: the digest must refer to the checkpoint block, and the node does not
log it at a quick start.

- **At save.** `BlockChain::saveLiveCheckpoint` (`blockchain.cpp:1012-1043`) and the slow-start
  path (`409-432`) compute `multiWallets().stateDigest()` right before
  `serializationManPtr_->save(S, head)`: at that moment the caches hold exactly block `S`, the
  same moment at which `Node::logStateDigest` logs the line for a stored block
  (`node.cpp:111`, `storeBlockEvent`). The digest and the wallet count go into
  `qs/<S>/state_digest.bin` (format: `u8 version = 1`, 32 bytes, `u64 wallets`) and into the log
  as `STATE DIGEST #<S> <hex> wallets <n> checkpoint`. The file is optional for older
  checkpoints and ignored by older builds; `head.bin` and `schema_version` do not change.
  Cost: the multiset digest is incremental once computed (`multiwallets.cpp:98-118`), so it is
  a 32-byte read.
- **At load.** After `tryQuickStart` succeeds (`blockchain.cpp:165-171`, where
  `successfullQuickStartEvent` is emitted and before the database is opened), the node logs
  `STATE DIGEST #<S> <hex> wallets <n> checkpoint loaded` from the caches just loaded, and
  compares it with `state_digest.bin` when present: a difference is an error that quarantines the
  checkpoint (`quarantineVersion`, as a hash mismatch does today, `caches_serialization_manager.cpp:424-429`)
  and lets the next one be tried. The first full computation costs one pass over the wallets
  (about 150,000 ristretto255 operations, a few seconds).
- **Existing lines are unchanged.** The line at start-up after the database is read
  (`node.cpp:255`) and the line every 1,000 blocks stay as they are, so `entrypoint.sh`,
  `digest_offline.sh`, `check_node.py` and `run.sh` keep working; the added words come after
  `wallets <n>`, which their patterns ignore.

The manifest's `state_digest` is this value. A node that restored a snapshot verifies it in
process (section 6.5); the `STATE DIGEST #(S|S+1)` wait of the entrypoint is only needed for
format 1.

## 4. Building a snapshot

The builder runs in the node, in its own thread at low priority, when `[snapshot] serve_port`
is above 0 (or on demand with `--export-snapshot`). Trigger: the event a periodic checkpoint
save emits (new `checkpointSavedEvent(S)` from `saveLiveCheckpoint`), and at the server's start
for the latest periodic checkpoint present in `qs/` without a built snapshot.

1. Copy `qs/<S>/*` to `snapshot/<S>.tmp/` (plain file copies; the directory is immutable until
   `pruneCheckpoints` removes it, 2 × `checkpoint_every` blocks later; the copy takes seconds).
   Check `quick_start_hashes.dat` against the copied files with the manager's own check, and
   `state_digest.bin` must exist (a checkpoint of an older build has none: not served).
2. Read the blocks `F..S` under `dbLock_` in batches of 256 sequences with
   `Storage::pool_load` (or a raw `Database::get(seq)` added for this), releasing the lock
   between batches so that sync and consensus are not held; write `blocks.bin.lz4`. About
   160,000 blocks: the conversion reads and writes 6,400 blocks/s (`docs/storage.md`), so
   under a minute. The hash chain of the batch is checked as it is written (`previous_hash` of
   `k+1` equals the hash of `k`, and the hash of `S` equals `head.bin`'s): a node whose own
   database fails this builds nothing and logs it.
3. `forEachContractData` into `contracts.bin.lz4`.
4. Hash every file and its chunks, write `snapshot.json`, sign it, rename `<S>.tmp` to `<S>`,
   remove any older `snapshot/<X>` directory. Disk: at most two snapshots during a build.
5. Log `SNAPSHOT: built block S, N files, M MB, digest …`.

Pruned and full nodes build the same thing from their own state, on BerkeleyDB, LMDB or RocksDB:
the export reads blocks through the storage, never copies database files, so the page leak of
`mdb_copy` cannot recur and no LMDB tool is needed. A full node with the default
`checkpoint_every=500000` builds a snapshot every 500,000 blocks (about 6 days); operators who
want fresher ones set `checkpoint_every=100000` (`docs/storage.md` says so). The build is
skipped, with a log line, when the checkpoint is tainted (`cachesTainted_`), when the first
stored block is above `S − 1000` (nothing to serve), or when free disk space is below twice the
estimated size.

`--export-snapshot <dir>` builds the snapshot of the latest periodic checkpoint of a stopped node
directory and exits: the CI workflow and the test network use it instead of `snapshot-make.sh`.

## 5. Serving

A Boost.Beast HTTP/1.1 server owned by `Node` next to the API (`csconnector`), started and
stopped from a thread that reads `[snapshot]` from `cs::ConfigHolder` every 10 s as the API
thread does (`csconnector.cpp:99-150`): setting `serve_port` in `config.ini` starts it within the
observer's period plus 10 s, setting it to 0 stops it, changing the limits applies at once.
Nothing restarts.

Requests, `GET` and `HEAD` only, HTTP/1.1, keep-alive up to 64 requests per connection:

| Path | Answer |
| --- | --- |
| `/snapshot/latest.json` | the manifest of the latest built snapshot; `404` while there is none |
| `/snapshot/<seq>/<name>` | that file, `name` from the manifest's fixed set (`[a-z0-9.-]{1,64}`); `Accept-Ranges: bytes`; one `Range: bytes=a-b` of at most 16 MiB is answered with `206` and `Content-Range`, a larger or multi-range request with `416`; `404` for any other path or sequence |
| anything else | `404`; any other method `405` |

Limits, each a `[snapshot]` key: `serve_max_connections` (default 8; above it `503` with
`Retry-After: 5`), `serve_rate_kbps` (default 0 = unlimited; a token bucket shared by all
connections, applied before every write), request line and headers at most 8 KB, idle timeout
15 s, write timeout 30 s. The server reads only `snapshot/<seq>/` directories the builder wrote,
by a name looked up in the manifest it loaded, never a path from the request; it has no
directory listing and no default document. `serve_bind` (default every interface, IPv6 too when
`[params] ipv6=true`) allows `127.0.0.1` behind a reverse proxy.

Log lines: start and stop with the port, one line per snapshot served to completion
(`SNAPSHOT: served block S to <ip>, N MB`), and a count every hour when there is traffic.

## 6. Fetching at first start

### 6.1 When

In `client/src/main.cpp` before `dbcheck::check` (`main.cpp:192`), when `[snapshot] fetch=auto`
(the default), `db/` holds no database (`csdb::backend::detect` finds nothing), `qs/` has no
checkpoint, and `db_backend` is `lmdb` or `rocksdb`. A BerkeleyDB node is told why it syncs from
block 0 (a database whose first block is above 0 cannot be BerkeleyDB, as for the conversion;
backward sync keeps the same rule). A full LMDB or RocksDB node restores a snapshot too, and
backfills from there when backward sync is on, which is what that design's section 8 asks for
in Docker, now for every operator. `fetch=off` keeps today's behaviour. `--import-snapshot
<manifest file or URL>` runs the same code on demand and exits.

Interrupting the fetch (Ctrl-C, SIGTERM) leaves `snapshot-tmp/` in the data directory; the next
start removes it and fetches again. Nothing is placed in `db/`, `qs/` or `caches/` before the
whole snapshot is downloaded and verified (section 6.4).

### 6.2 Sources

In this order, all optional, duplicates removed by key:

1. `[snapshot] sources=` : `host:port` entries given by the operator (a mirror, a friend's node).
2. `sources_file=snapshot_sources.txt` : `host:port key` lines, shipped in the packages and kept
   up to date by the weekly job (section 9).
3. `probe_known_hosts=true` : every peer of `hosts_filename` (`known_hosts.txt`), at
   `serve_port`'s default 9511 on the peer's IP. Only peers with a plain `ip:port` address.
4. `github_manifest=` : the URL of `snapshot.json` on the release `pruned-snapshot` (the default;
   empty disables it). Fetched with `curl -fsSL --max-time 30`; when `curl` is not on the `PATH`
   the source is skipped and logged.

Each source's manifest is fetched with a 5 s connect and 10 s total timeout, at most 64 KB, in
parallel (8 at a time); malformed manifests, bad signatures and a `node_key` that differs from
the key in the sources file or in `known_hosts.txt` are dropped and logged with the reason.
GitHub's manifest is accepted on the strength of HTTPS and the run URL, not of its signature
(the workflow's node has a throwaway key).

### 6.3 Agreement

Manifests are grouped by `(seq, head_hash)` and, inside, by `state_digest`. A source counts once;
sources in the same /24 (IPv4) or /48 (IPv6) count as one; GitHub counts as one.

1. Candidate blocks: the `(seq, head_hash)` groups with at least `min_sources` (default 3)
   counting sources, highest `seq` first. With none, the fallback of step 3.
2. In the chosen block, the digest group with most sources wins if it reaches `min_sources`, or
   if GitHub is in it and `fallback=github` (the default). Otherwise the next candidate block.
3. With no acceptable group: if `fallback=github` and GitHub's manifest is present, it is used
   alone (today's trust level, with a warning); otherwise the node logs every group seen and
   syncs from block 0. `min_sources=1` lets an operator use one source they trust (their own
   mirror).

Every group is logged as `SNAPSHOT: block S hash … digest … : N sources (keys …)`, so that the
divergence of section 7.1 is visible on every start.

### 6.4 Download and import

- Files are downloaded into `snapshot-tmp/` from the sources of the chosen group, up to
  `fetch_connections` (default 4) at a time, each file from the sources that declare its
  `sha256`, chunk by chunk with `Range`, each chunk verified against the manifest; a bad chunk
  marks the source for that file and is fetched elsewhere; a file with no remaining source is
  restarted from another `sha256` group; a whole-file SHA-256 after the last chunk. `fetch_rate_kbps`
  caps the download. Timeouts and sizes: 30 s per chunk, at most 16 MiB per chunk, at most 8 GB
  for the whole snapshot, at most 32 files; a manifest beyond these limits is rejected before any
  download. GitHub files are downloaded with `curl -r a-b` through the same chunk logic.
- Names are mapped to destinations by a fixed table: `blocks.bin.lz4` and `contracts.bin.lz4` to
  the importer, `qs-<name>.lz4` to `qs/<S>/<name>` for the twelve names of section 2. Anything
  else in a manifest rejects it (no path from the network is ever used as a path).
- Import, after every file is verified:
  1. decompress `qs-*` into `qs/<S>/`; check `quick_start_hashes.dat` the way `loadVersion`
     does; check that `head.bin` carries `S` and `head_hash`, and `state_digest.bin` the agreed
     digest;
  2. open a fresh storage of the configured backend in `db/` (as `Storage::open` does on an
     empty folder, writing `db/.backend`) and write the blocks of `blocks.bin` ascending with
     `Database::put(hash, seq, bytes)`, verifying each one before it is written: `Pool` parses,
     `sequence()` is the expected one, the hash computed from the bytes equals `previous_hash`
     of the block already accepted above it when importing downwards, or, importing upwards as
     here, each block's `previous_hash` equals the hash of the one before, and the hash of block
     `S` equals the agreed `head_hash` (which anchors the whole range); for `k ≥ 1` the pool
     signatures verify against the block's own confidants, as backward sync's section 3 does
     with the shared `cs::BlockVerifier` (plan step 3b). A failure discards everything and
     marks the group's file sources;
  3. update `caches/hashdb` and `caches/seqdb` with every block as it is written
     (`cs::BlockHashes::update`), because `tryQuickStart` looks the checkpoint's head up in
     `seqdb` before the rebuild of `blockchain.cpp:266-279` could run; leave `caches/indexdb`
     empty so that the node rebuilds it after the quick start (`blockchain.cpp:143-153`);
  4. write the contract data with `updateContractData`;
  5. `flush()`, write `snapshot.json` next to the data as today (with the sources used), remove
     `snapshot-tmp/`.
  Blocks: about 160,000 Ed25519 checks of 3 to 5 signatures each, well under a minute; the writes
  as for the conversion.

### 6.5 Verification at start-up

The node then starts as usual: quick start from `qs/<S>`, index rebuild, sync from the peers.
When `snapshot.json` is present and not yet marked verified, the digest logged at the checkpoint
load (section 3) must equal the manifest's: the node writes `snapshot.verified`; on a mismatch
it writes `snapshot.mismatch`, logs `SNAPSHOT DIGEST MISMATCH …` and stops with exit code 4, and
refuses to start while the file exists. The Docker entrypoint keeps its handling of
`snapshot.mismatch` (`entrypoint.sh:117`) and drops its own digest wait for format 2.

## 7. Trust and security

**What a source can and cannot do.**

| Attack | Defence |
| --- | --- |
| A forged chain (blocks the network did not produce) | `(S, head_hash)` needs `min_sources` independent declarations; every imported block is chained to that hash and its pool signatures are checked against its confidants; the network then has to continue from `S`, which it does only if `head_hash` is its block |
| A forged wallet state | The digest needs the same agreement; the restored node refuses to run on a state whose digest is not the agreed one (section 6.5) |
| A forged checkpoint whose wallets match but whose other caches do not (contract states, consensus settings, tokens, round stats) | Not covered by the digest today. Contract states are validated against the chain at start (`node.cpp:213`, `250`). Consensus settings and the pending settings of `blockchain.dat` are small and deterministic: the open question in section 14 proposes to put them into `state_digest.bin` and the manifest as a second digest now; contract and token state later |
| Garbage, slow or huge replies | Sizes, counts and timeouts of section 6.4; a source is marked per file; at most 4 connections; everything streams to disk, nothing large stays in memory |
| Sybil sources | A source is a node key, free to make; an IP in a reviewed list is not. Sources from `snapshot_sources.txt` and `known_hosts.txt` reach the packages only through the weekly job (p2p participation verified, at most 3 per /24) and a reviewed pull request; the agreement counts one source per /24; GitHub is an anchor that no node key can impersonate (HTTPS); an operator can pin `sources=` and `min_sources` |
| Path traversal, file disclosure | Server: three fixed routes, names matched to the manifest's, no path from the request, only the `snapshot/<seq>/` directories it built. Client: fixed name table; a manifest with any other name is rejected. Keys, `config.ini`, logs, `known_hosts.txt` are never read by the server and never written by the importer |
| Denial of service on the serving node | Connection cap, bandwidth cap, header limit, idle timeout, keep-alive cap; building runs at low priority in batches that release `dbLock_`; serving is off by default in the binary |
| A compromised GitHub release | Same as today for a GitHub-only start; with nodes serving, a GitHub manifest that disagrees with 3 nodes is a minority group and is logged |

What agreement does not give: a wrong-by-a-fee balance held by honest nodes (section 7.1) is
indistinguishable from a forged one with the same support; and nothing here verifies that the
state at `S` is the result of the chain from block 0 (that is a replay, the same trust as any
checkpoint sync).

### 7.1 Honest digest divergence

The digest covers balances and delegations of every wallet (section "What exists today"). A
difference of one fee on one wallet gives a different digest. Known facts:

- On 2026-10-23 the local node's balance of one wallet differed from the network's by exactly one
  fee after replaying a dice contract that reads its own balance (#83); 7 of 20 mainnet nodes
  agreed with us and 13 did not on that wallet; 80 random wallets matched everywhere.
- So the official full nodes form at least two digest families today, and the number can grow by
  one wallet at a time with every such contract call that is replayed rather than executed.
- A node that catches up from a snapshot replays the blocks it missed, so two nodes restored from
  the same snapshot that caught up separately may also diverge, slowly. Nodes that were at the
  head when the contract ran apply the states the blocks carry and should agree. How fast
  families form is not known; nobody has measured it.

Measured on 2026-10-24 (plan step 0): the local pruned node (synced from block 0, at the head
since then) and two nodes restored from the two published snapshots (blocks 186,460,890 and
186,466,024, each catching up on its own through about 5,000 replayed blocks) logged the same
`STATE DIGEST` at every common sequence, 186,467,000 to 186,470,000. One family inside the GitHub
lineage so far. Official nodes log no digest, so their families can only be seen through
balances (the 7/13 split above).

What follows for the design:

- Agreement on `(S, head_hash)` is cheap, strong and independent of this problem; it is required.
- The digest is used as a majority vote inside that block, with GitHub as the tie-breaker
  (section 6.3). While most community nodes descend from the GitHub lineage, they will usually
  agree; a node of another family is a logged minority, not a failure.
- The first measurement is free: the local pruned node, the CI run and any fresh container log
  `STATE DIGEST` at every multiple of 1,000; comparing them at the same sequence tells how many
  families there are already (plan step 0). The weekly job publishes the groups it sees
  (section 9), which is also useful for the report on the fee attribution.
- If families turn out to be common, the fallback is in place (`fallback=github`), and a
  divergence-free digest would need the fee attribution fix (consensus-relevant, postponed) or
  a digest that leaves contract callers out; neither is in this design.

### 7.2 What the serving node exposes

A manifest signed with the node key links the node's IP to its key and to the block it is at,
which the p2p layer exposes already (`known_hosts.txt` lines are `ip:port key`; peers see the last
sequence). The snapshot carries the same state any peer can request block by block. No private
data of the operator is in it; the wallet cache is public chain state.

## 8. Operator configuration

`[snapshot]` in `config.ini`, all keys optional; without the section nothing changes for an
existing node. Read by the observer, so every key but `fetch` takes effect while the node runs
(the fetch happens at a start on an empty database by nature).

| Key | Default | Meaning |
| --- | --- | --- |
| `serve_port` | `0` | TCP port of the snapshot service; `0` = off. The pruned package and Docker set `9511` when the operator asks (`CREDITS_SERVE_SNAPSHOT=on`) |
| `serve_bind` | all interfaces | address to listen on |
| `serve_max_connections` | `8` | concurrent connections; more get `503` |
| `serve_rate_kbps` | `0` | upload cap, all connections together; `0` = unlimited |
| `snapshot_blocks` | `101000` | blocks before the checkpoint included in a snapshot, bounded by the first stored block |
| `fetch` | `auto` | `auto`: restore a snapshot at a start with an empty database (lmdb or rocksdb); `off`: never |
| `sources` | empty | `host:port` list, comma-separated |
| `sources_file` | `snapshot_sources.txt` | `host:port key` lines; missing file = no entries |
| `probe_known_hosts` | `true` | try every known host at the default port 9511 |
| `github_manifest` | the release URL | empty = no GitHub source |
| `min_sources` | `3` | independent sources that must agree |
| `fallback` | `github` | `github`: GitHub alone when fewer agree; `none`: sync from block 0 instead |
| `fetch_connections` | `4` | parallel downloads |
| `fetch_rate_kbps` | `0` | download cap; `0` = unlimited |

Docker (`.env`, written into `config.ini` on the first start as today): `CREDITS_SERVE_SNAPSHOT=on|off`
(default `off`; `on` sets `serve_port=9511`, and `docker-compose.yml` publishes `9511/tcp`),
`CREDITS_SNAPSHOT=auto|off` maps to `fetch`, `CREDITS_SNAPSHOT_URL` to `github_manifest`,
`CREDITS_SNAPSHOT_MIN_SOURCES` to `min_sources`. The entrypoint stops restoring by script and
stops waiting for the digest line: the node does both. `snapshot-restore.sh` is kept one release
for the image of the previous version and then removed with `snapshot-make.sh`, `lmdb-check.sh`
staying as a diagnostic tool.

Disk on a serving node: up to two snapshots of about 0.4 GB (lz4, estimate) in `snapshot/`.
Bandwidth: a full download is about 0.4 GB per new node; the cap is the operator's choice.

## 9. Discovery

The weekly job (`known-hosts.yml`, `run-weekly.sh`) gets one more step, `ci/known-hosts/probe_snapshots.py`:
for every peer of the verified list, `GET http://<ip>:9511/snapshot/latest.json` (5 s, 64 KB),
verify the signature with the peer's key (PyNaCl), and record `seq`, `head_hash`,
`state_digest`, `created`, `node`. Output:

- `snapshot_sources.txt`: `ip:port key` of the peers whose manifest verified, same /24 rule as
  `known_hosts.txt`; uploaded to the release `known-hosts` and proposed in the same pull request
  as `packaging/mainnet/snapshot_sources.txt`, shipped by `make-package.sh` and the image;
- `snapshots.md`: the groups `(seq, head_hash, state_digest)` with their sources, the record of
  section 7.1.

A node that is not in the list yet is still found by `probe_known_hosts`, and an operator can
name it in `sources=`. The job's node runs with `fetch=off` (it must sync from block 0 to test
who serves old blocks).

## 10. GitHub workflow and Docker

- `snapshot.yml` (format 2): after the node reached the head and the checks passed, stop it and
  run `./node --export-snapshot out/` (the latest periodic checkpoint in `qs/`); publish
  `snapshot.json` and the files as assets of `pruned-snapshot`, each file as `s<seq>-<name>`
  because the assets of one release share a namespace (the manifest keeps the plain `name`; the
  client composes the asset name for the GitHub source only), keeping the files of the last 3
  sequences. When the latest checkpoint is the one the run started from (no multiple of 100,000
  crossed in 12 hours), nothing new is published. The published block is therefore up to
  100,000 blocks behind the head (about a day); a restored node syncs the rest in minutes. For
  one release the workflow also publishes format 1 (`latest.json`, `credits-pruned-<seq>.tar.zst`)
  for the previous image; then format 1 goes.
- The balance cross-check stays as it is (section 4 of the 2026-10-16 design), with the known
  hosts' Thrift APIs as more sources being a separate small change (`check_node.py balances
  --api-also host:port,…`), not part of this design's steps.
- `test-snapshot.sh` and `test-entrypoint.sh` follow the scripts they test.

## 11. Consistency with backward sync

- Both designs share the block verifier (hash chain, pool signatures against the block's own
  confidants) and `Database::put` outside the write queue. The plan puts the verifier in its own
  step so that either branch can land first.
- A full LMDB or RocksDB node restores a snapshot through this design and backfills through
  that one: backward sync's section 8 ("a full RocksDB node restores the snapshot and starts with
  `--convert-db`") becomes unnecessary, since the importer writes the configured backend
  directly. BerkeleyDB full nodes sync from block 0 in both designs.
- A full node that has backfilled to block 0 serves the same snapshot as everyone else: the export
  range is `snapshot_blocks` before the checkpoint, whatever is stored below.
- `previousNonEmpty_` and the other maps of `blockchain.dat` come with the checkpoint, as in
  format 1 (backward sync's "Non-empty blocks").

## 12. Measurements to make

- lz4 ratio on `walletscache.dat`, `blockchain.dat`, `apihandler.dat`, `smartcontracts.dat` and
  on the block stream (zstd gives 16x on the `.dat` files and 1.9x on blocks; lz4 is expected
  between 4x and 10x on the `.dat` files and about 1.5x on blocks). If the total is well above
  the 278 MB of format 1, zstd as a third-party library is the alternative (section 14).
- Export time and CPU on the local pruned node and on a full RocksDB node; the batch size of
  256 blocks under `dbLock_` against the block rate.
- Import time (signature checks, writes, hash databases) on lmdb and rocksdb.
- Digest families on mainnet (plan step 0, then weekly).

## 13. Tests

**Unit tests** (`tests/csnode`): manifest write, read, sign, verify, `files_hash`; the fixed
name table and rejection of any other name; agreement on synthetic manifest sets (threshold,
/24 collapsing, GitHub tie-break, fallback, highest block first, minority logging); chunk
hashing and the range arithmetic; export and import round trip on LMDB and RocksDB with
generated signed blocks (as `pool_tests.cpp` and backward sync's tests build them), including
a block with an altered transaction, an altered signature, a wrong sequence, a wrong head hash,
and a checkpoint whose `state_digest.bin` differs; the HTTP server on an ephemeral port against
the client: manifest, file, ranges, 416, 404, 405, header limit, connection cap, rate cap, a
slow client against the idle timeout; `state_digest.bin` written and read by the checkpoint
manager; the config keys and the observer's comparison.

**Private network** (`robustness.yml`, `ci/testnet/run.sh`), all with the network's digests
matching at the end:

| Scenario | What happens | Must show |
| --- | --- | --- |
| `snapshot-serve-join` | n4 (full BerkeleyDB), n6 (pruned LMDB) and n7 (full RocksDB) serve; n8 joins with `min_sources=3`, no GitHub | three manifests agree; n8 restores, its checkpoint digest matches, it reaches the target; `snapshot.verified` present |
| `snapshot-serve-few` | only n6 serves, `min_sources=3`, `fallback=none` | n8 logs the groups, restores nothing, syncs from block 0 and reaches the target |
| `snapshot-serve-forged` | a test build makes n7 serve a checkpoint with an altered balance, then a block with an altered signature | the altered digest is a logged minority; the bad block fails the import from n7 and comes from n4; n8 completes |
| `snapshot-serve-dynamic` | `serve_port` written into n6's `config.ini` while it runs, later set to 0 | the service starts within the observer period, serves a manifest, stops; no restart |
| `snapshot-serve-limits` | n6 with `serve_rate_kbps=2000`, `serve_max_connections=1`; two clients | the rate measured is within 20 % of the cap; the second client gets `503` |
| `snapshot-serve-dropped` | n8 is killed in the middle of a download, then started again | `snapshot-tmp/` is removed, the fetch is redone, n8 completes |
| `snapshot-serve-backend` | n8 as a full RocksDB node, `fetch=auto` | it restores into RocksDB directly and, with backward sync present, backfills from there |
| `snapshot-join` (existing) | as today, with the format 2 export and import in place of the scripts | as today |

**Docker:** `test-entrypoint.sh` covers `CREDITS_SERVE_SNAPSHOT` and the mapping of the
`CREDITS_SNAPSHOT*` variables; the mainnet smoke test of `docker.yml` starts with
`CREDITS_SNAPSHOT_MIN_SOURCES=1` against GitHub.

**Mainnet:** the local pruned node serves (`serve_port=9511`, 2 MB/s cap) and a fresh container
on this host restores with `sources=127.0.0.1:9511`, `min_sources=2` and GitHub, which is the
first agreement between a node and the workflow; then the weekly job's first `snapshots.md`.

## 14. Decisions (2026-10-24)

1. **HTTPS to GitHub through the system `curl`.** No OpenSSL in the static build.
2. **Serving on by default in the packages and in Docker** (port 9511, `serve_rate_kbps=2000`).
   The binary alone defaults to off; `config.ini` turns it off without a restart.
3. **`min_sources=3` with `fallback=github`.** To be reviewed once enough nodes serve.
4. **Compression**: decided in step 3a on the lz4 numbers (zstd as a library if lz4 is far behind).
5. **GitHub moves to checkpoint blocks** (section 10), so that it can agree with nodes.
6. **Second digest over the consensus settings**: now, in `state_digest.bin` and the manifest.
7. **Signature checks on import**: kept.
8. **Step 0**: the local node's logs are read (not touched) for the digest-family measurement.

## 15. Decisions (2026-10-25), after the evaluation of snapshots over p2p

See [2026-10-25-snapshots-over-p2p-evaluation.md](2026-10-25-snapshots-over-p2p-evaluation.md).

1. **B2**: the state travels as a snapshot over HTTP (this design); the blocks before the checkpoint
   come from the neighbours, official nodes included, through the existing BlockRequest, by backward
   sync with a floor (`F = S - snapshot_blocks`). Until backward sync is merged, snapshots keep the
   full block stream; then they shrink to the state and the anchor block S (about 70 MB). The importer
   handles both (the manifest's `first_block`).
2. **Discovery** also through the p2p `host_data` bits that official nodes ignore (24-63): a
   community node advertises that it serves snapshots and on which port.
3. The p2p findings (no frame size limit, no node-key authentication, one queue per connection) go to
   the private report for the official developers.

