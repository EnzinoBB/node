# Snapshots over the node's p2p: implementation plan

For `snapshot-p2p-design.md` (same folder), against `origin/master` cb4d2125 (4.2.532-community.12).
Each step is one pull request, in this order unless the table at the end says two can go in
parallel; every step leaves master releasable with the feature off or absent. File:line references
are on cb4d2125 (CRLF files: same line numbers). Steps marked **[protocol]** add or change what
goes on the wire between community nodes; steps marked **[p2p submodule]** change the library;
**no step is consensus-relevant**: no block, hash, signature or round rule changes, official nodes
never see a message they act on, and a joining node takes part in nothing until its chain is open.

Conventions as in the 2026-10-24 plan: log prefix `SNAPSHOT: `; new files
`csnode/include/csnode/snapshot_*.hpp`, `csnode/src/snapshot_*.cpp`; GoogleTest in `tests/csnode/`
(the CMake glob picks up `*.cpp`); plain log texts; `WithDelimiters` for sequences; sizes: small
up to about 300 lines with tests, medium up to about 800, large more.

What PR #98 (`origin/feat/snapshot-server`, not merged) gives: its commits are not merged as a
branch; step P3 cherry-picks the parts listed in the design's 5.4 and drops the HTTP server. #98
is closed with a note once P3 is merged.

Backward sync (`origin/docs/backward-sync`, plan steps BS1-BS6) is interleaved below where the
two plans share machinery; its own steps keep their numbers.

## Step P0. `[snapshot]` configuration for p2p (small)

**Files.** `client/config/config.hpp:76-91` (`SnapshotData`), `client/config/config.cpp:1061-1089`
(`parseSnapshotData`), `1267-1284` (`operator==`), the config test that #88 added in
`tests/csnode/` (extend), `packaging/mainnet/config.ini` (a commented `[snapshot]` section),
`docs/snapshots.md`, `docs/differences.md`.

**Change.** The key set of the design's section 12: add `serve` (`on`/`off`), `serve_max_peers`
(1..64), `fetch_wait` (0..86400), `fetch_fallback` (`sync`/`wait`), `sources` as a list of base58
keys (each decoded with `DecodeBase58`, 32 bytes, else dropped with a warning); keep
`serve_rate_kbps`, `snapshot_blocks` (**range 1..10,000,000**, today 1000 at `config.cpp:1073`),
`fetch`, `sources_file`, `min_sources` (default **2**, design 6.4), `fetch_rate_kbps`; remove
`serve_port`, `serve_bind`, `serve_max_connections`, `probe_known_hosts`, `github_manifest`,
`fallback`, `fetch_connections` (an unknown key under `[snapshot]` logs `config: [snapshot] <key>
is not a setting of this version, ignored`). `getSnapshotSettings()` unchanged in shape; the
comparison covers every key so the observer notices changes.

**Tests.** Defaults without the section; every key; `snapshot_blocks=1` accepted; a bad key in
`sources` dropped; two configs differing only in `serve` compare unequal; the removed keys warn.

**Pass.** Tests green; a node with the old `[snapshot]` keys of #88 starts with warnings only.

## Step P1. Messages and codecs (small) [protocol]

**Files.** `net/include/net/packet.hpp:24-66` (four values: `SnapshotManifestRequest = 40`,
`SnapshotManifestReply`, `SnapshotPieceRequest`, `SnapshotPieceReply`), `net/src/packet.cpp`
(`messageTypeToString`), `net/src/packetvalidator.cpp:94-109` (never-cut list),
`net/src/transport.cpp:326-401` (four `dispatchNodeMessage` cases calling
`node_->getSnapshotManifestRequest/Reply`, `getSnapshotPieceRequest/Reply(data, size, sender)`),
`csnode/src/node.cpp:2222-2225` (`chooseMessageAction`: `Process` for the four, as `BlockRequest`;
the bootstrap gate comes in P5), `csnode/include/csnode/node.hpp` (the four handlers, empty
bodies until P3/P4 fill them: a request is answered `not serving`, a reply is dropped and
counted), new `csnode/include/csnode/snapshot_messages.hpp` and `csnode/src/snapshot_messages.cpp`,
`tests/csnode/snapshot_messages_tests.cpp`.

**Change.** `snapshot_messages.hpp`: the four structs of the design's 4.2 with the status enums,
`encode(const T&) -> cs::Bytes` and `bool decode(const uint8_t*, size_t, T&)` over
`cs::ODataStream`/`cs::IDataStream`, every limit as a `constexpr` (`kMaxManifestJson = 256 KB`,
`kMaxPieceBytes = 1 MiB`, `kMaxName = 64`), `decode` refusing a payload above the limit before
reading it and a name outside `SnapshotManifest::isKnownFileName`. The validator entries keep the
four out of the round cut (`packetvalidator.cpp:112-115`); the inbox priority stays the default
second (`packetsqueue.cpp:77-78`). Flags `BaseFlags::Clear` when sent (a small overload of
`Node::sendDirect` taking the flags, or a sibling `sendDirectPlain`).

**Tests.** Round trip of each message with edge values; truncated payloads; a name of 65 chars;
a piece of 1 MiB + 1 refused; a JSON of 256 KB + 1 refused; `messageTypeToString` names.

**Pass.** Tests green; `testnet.yml` green (nothing is sent yet); the private network's logs show
no `Unknown message type` (nothing sent).

## Step P2. Capability flag and known peers (small) [protocol: `host_data` bits 24-47]

**Files.** `net/src/transport.cpp:35-51` (`getHostData`, `getPlatform`/`getVersion` and two new
decoders), `438-468` (`getKnownPeers` fills `capabilities`), `net/include/net/transport.hpp`
(`bool peerCapabilities(const cs::PublicKey&, uint8_t&) const`), `lib/include/lib/system/common.hpp:50-59`
(`PeerData::capabilities`, `minorVersion`), `csnode/src/node.cpp:135-139` (`dumpKnownPeersToFile`
also writes `known_peers.json`), `client/config/config.cpp:225-234` (unchanged format of
`known_hosts.txt`), `tests/csnode/hostdata_tests.cpp` (new; the encoders moved to a small header
`net/include/net/hostdata.hpp` so that they are testable).

**Change.** Bit 24 set when `getSnapshotSettings().serve` is on at `Transport` construction
(`transport.cpp:118-124`, `createNetConfig` at `65`); bits 32-47 = `MINOR_NODE_VERSION`
(question 5 of the design: include unless the user says no). `peerCapabilities` searches
`Host::GetKnownNodes` (`p2p/src/host.h:40`) for the key; cost one vector scan, called a few
times a second at most by the fetcher. `known_peers.json`: an array of `{id, ip, port, version,
minor, platform, capabilities}`, written next to `known_hosts.txt` at stop.

**Tests.** Encode/decode of the byte and the minor version; bits 0-23 unchanged (the official
decoders at `45-51` still read platform and version); `known_peers.json` written by a stub.

**Pass.** Tests green; on the private network a node with `serve=on` is listed with
`capabilities 1` in a neighbour's `known_peers.json`, and the even nodes on the official binary
(`BIN_ALT`) still accept it as a neighbour.

## Step P3. Serving side: service from #98 and the p2p provider (medium)

**Files.** From #98, cherry-picked: `csnode/include/csnode/snapshot_service.hpp`,
`csnode/src/snapshot_service.cpp` (minus `SnapshotServer`: `server_->apply/reload/stop` become
`provider_.load(dir)`/`unload()`), `csnode/include/csnode/blockchain.hpp` (`checkpointSavedEvent`,
`blockBytes`, `pruneTarget`), `csnode/src/blockchain.cpp` (the emit after a live checkpoint save,
#98's hunk at `1053-1057`; `blockBytes`, `pruneTarget`), `csdb/include/csdb/storage.hpp` and
`csdb/src/storage.cpp` (`pool_bytes` only), `csnode/src/snapshot_builder.cpp` (streaming copy of
the checkpoint files; no contracts file), `client/src/snapshot_export.cpp:126-128` (no contracts),
`csnode/CMakeLists.txt`, `ci/testnet/run.sh` (`snapshot_section`, `SNAPSHOT_SERVE_NODES`).
New: `csnode/include/csnode/snapshot_provider.hpp`, `csnode/src/snapshot_provider.cpp`,
`tests/csnode/snapshot_provider_tests.cpp`; `csnode/src/node.cpp` (the two request handlers of
P1 filled; `snapshotService_` and `snapshotProvider_` owned by `Node`, started in `Node::init`
after the chain as #98 does at `node.cpp:258+`, stopped in `Node::destroy`).

**Change.**

- `SnapshotBuildInput::chunkSize` default 512 KiB (`snapshot_builder.hpp:34`); `--export-snapshot`
  uses it too (the GitHub snapshot gets the same chunking).
- `SnapshotProvider` as the design's 5.1-5.3: `load(dir)` reads and verifies `snapshot.json`,
  checks file sizes, opens the files (kept open; reopened on `load`); `onManifestRequest(sender,
  reply&)` and `onPieceRequest(sender, req, reply&)` are pure functions of the loaded state, a
  `SyncedTo` callback (as #98's: last block and round) and an injectable clock; the global and
  per-peer token buckets, the 30-s peer table (64 entries), the trusted halving through a
  `isTrusted` callback (`myLevel_ != Level::Normal`); `pread` through `std::ifstream::seekg` or
  `::pread` on Linux. Replies go through `Node::sendDirect` with `BaseFlags::Clear`.
- `Node::getSnapshotManifestRequest` / `getSnapshotPieceRequest`: decode (P1), call the provider,
  send the reply; count and log at debug.
- `SnapshotService` keeps #98's behaviour with `serve` in place of `serve_port != 0`; it loads
  the provider after a build and at start for the newest complete `snapshot/<S>` (verifying
  before loading); on `serve=off` it unloads.
- Log lines of 5.1.

**Tests.** `snapshot_provider_tests`: a temporary snapshot built by `buildSnapshot` from a
generated chain (as `snapshot_builder_tests` does): manifest reply ready / not serving / not
synced / building; a piece request for each chunk of each file returns bytes whose SHA-256 is the
manifest's chunk hash, the last chunk shorter; unknown name, unknown seq, chunk out of range →
status 1; a global bucket of 1 MB/s with a fake clock: the third 512 KiB piece within the same
second is busy with `retry_ms` ≤ 1000; per-peer: a second peer still gets its share; `serve_max_peers=1`:
the second peer is busy; trusted halves the rate; the table evicts the oldest at 65 peers.

**Pass.** Tests green; private network `snapshot-p2p-serve` (= #98's `snapshot-serve` row without
the HTTP check): n6 logs `SNAPSHOT: built block S` and `serving block S`; `snapshot-join-*` and
`all-storage` unchanged; a mixed run (`BIN_ALT`) green.

## Step P4. Fetcher: discovery, agreement, download (large)

**Files.** New `csnode/include/csnode/snapshot_agreement.hpp`, `csnode/src/snapshot_agreement.cpp`,
`csnode/include/csnode/snapshot_fetcher.hpp`, `csnode/src/snapshot_fetcher.cpp`,
`tests/csnode/snapshot_agreement_tests.cpp`, `tests/csnode/snapshot_fetcher_tests.cpp`;
`csnode/src/node.cpp` (the two reply handlers of P1 forward to the fetcher when one exists).
Nothing in this step changes a running node's behaviour: the fetcher is constructed only in P5.

**Change.**

- `snapshot_agreement`: `struct SourceManifest { PublicKey key; std::string address; bool
  trusted; SnapshotManifest manifest; }`; `AgreementResult agree(const std::vector<SourceManifest>&,
  size_t minSources)` → the chosen group (block, digests, its sources), every group for logging,
  and the shortfall (how many sources the best group lacks). Collapsing by /24 and /48 with the
  same helper `collect.py` uses in spirit (a `netPrefix(address)` function), `trusted` sources
  counted by key.
- `cs::SnapshotFetcher`, single-threaded on the processor thread, modelled on the `BackwardSync`
  class of the backward-sync plan (step 3b: a `Context` per tick, injectable `Clock`, callbacks
  for sending):

  ```cpp
  class SnapshotFetcher {
  public:
      struct Peers { std::vector<std::pair<PublicKey, uint8_t>> known; /* key, capabilities */
                     std::set<PublicKey> neighbours; };
      struct Hooks {
          std::function<void(const PublicKey&, const SnapshotManifestRequest&)> sendManifestRequest;
          std::function<void(const PublicKey&, const SnapshotPieceRequest&)> sendPieceRequest;
          std::function<void(const std::set<PublicKey>&)> addNeighbours;      // Transport::addToNeighbours + setPermanentNeighbours
          std::function<std::string(const PublicKey&)> addressOf;             // routing table
          std::function<uint64_t()> clock;                                    // ms
      };
      enum class Phase { Discover, Collect, Fetch, Import, Done, Fallback };
      SnapshotFetcher(const SnapshotData& cfg, std::filesystem::path tmpDir, Hooks hooks, bool dryRun = false);
      void tick(const Peers&);                                       // every 500 ms
      void onManifestReply(const PublicKey&, const SnapshotManifestReply&);
      void onPieceReply(const PublicKey&, SnapshotPieceReply&&);
      Phase phase() const;
      // Fetch complete: the manifest and the verified files in tmpDir, for the importer (P5)
      std::optional<std::pair<SnapshotManifest, std::vector<SourceManifest>>> result() const;
      void onImportFailed(const std::string& fileAtFault);          // marks the sha256 group, back to Collect
      std::vector<GroupReport> groups() const;                       // for --survey-snapshots and the logs
      void stop();
  };
  ```

  Data: per source `{manifest?, lastAsked, marks, outstanding pieces, busyUntil}`; per file
  `{sha256 groups → sources, pieces missing, pieces in flight {chunk, source, sentAt}}`; the
  pre-sized output files; a token bucket on requested bytes (`fetch_rate_kbps`); `startedAt` for
  `fetch_wait`; attempt counter (3).

  `tick`: Discover/Collect: every 500 ms add the known peers with bit 24 and the `sources` and
  `sources_file` keys to the neighbour set (through `addNeighbours`, once per key); every 30 s
  send a manifest request to each serving neighbour without a fresh manifest; after each reply
  try `agree`; log the 30-s line of the design's 6.4; on `fetch_wait` expiry → `Fallback`
  (`sync`) or keep going with the 5-min line (`wait`). Fetch: fill up to 8 outstanding pieces
  (2 per source) round-robin over the sources of each file's biggest `sha256` group, skipping
  sources that are not neighbours now, marked, or busy; expire pieces after 30 s (the source
  marked 60 s, twice → the rest of the bootstrap); when a file is complete compute its SHA-256
  and compare; when every file is complete → `Import` (P5 hands the result to the worker; in
  `dryRun` the fetcher stops at the end of Collect and reports the groups).

  `onPieceReply`: must match an outstanding piece (source, seq, name, chunk), else dropped and
  counted; status busy → `busyUntil = now + retry_ms` (capped at 10 s); status 1/3 → mark;
  status 0: SHA-256 of `data` against the manifest's chunk hash: equal → write at
  `chunk × chunk_size`, else mark the source for the bootstrap and requeue the piece.

- `Node::getSnapshotManifestReply` / `getSnapshotPieceReply`: decode (P1); if `fetcher_` exists
  forward, else drop and count (a reply nobody asked for).

**Tests.** `snapshot_agreement_tests`: the cases of the 2026-10-24 step 5a without GitHub, plus
`trusted` keys in one /24 counted separately, /48 for IPv6, the shortfall value.
`snapshot_fetcher_tests` with fake hooks and three fake sources built by `buildSnapshot` from one
generated chain (two with identical `qs-*` bytes, one with a trailing byte in `walletscache.dat`
so its `sha256` group differs) and a fourth with another digest: (1) discovery adds only peers
with bit 24 and the configured keys; (2) manifests with a bad signature or a `node_key` that is
not the sender's are dropped; (3) agreement with 3 of 4, the minority logged; (4) pieces are
spread round-robin, never more than 2 per source and 8 in total; (5) a corrupt piece marks the
source and the piece comes from another source of the group; (6) a stalled source times out and
its pieces are reassigned, the source is marked; (7) busy delays only that source; (8) a source
that leaves the neighbour set is not asked; (9) a file whose group runs dry restarts from the
other group; (10) whole-file hash mismatch (a fake source that serves consistent chunk hashes
but… impossible by construction: the test instead corrupts the output file on disk between the
last piece and the check) → back to Collect; (11) `fetch_wait` expiry with `sync` and with
`wait`; (12) `onImportFailed` marks the group and the second attempt succeeds from the other
group; three failures → Fallback; (13) `dryRun` stops after Collect with the groups; (14) stop in
every phase leaves no outstanding timer; (15) the rate cap: with 1000 kbps no more than 1 MB is
requested per simulated second.

**Pass.** Unit tests green; no behaviour change on the network (nothing constructs the fetcher).

## Step P5. Bootstrap mode of `Node`, start-up, scenarios (large; the delicate one: three PRs)

Planned 2026-10-25 against `origin/master` 0be88cbf (P0 #99, P1 #100, P2 #101, P3 #102, P4 #104
merged; CRLF files, same line numbers) and PR #103 (`origin/ci/official-nodes`: official 4.2.532
nodes on the even positions of CI networks, `node_alt_ref=official`, `BIN_ALT_OFFICIAL=1` in
`run.sh`, a scenario that needs a fork-only feature on an even node is refused). File:line
references below are on 0be88cbf unless marked `[#103]`.

Split into **P5a** (the node: bootstrap mode, hand-over, verification; two CI rows), **P5b** (the
harness: faults, forged and corrupt sources, the late server, official variants, docs) and **P5c**
(optional: the p2p fork's connection-endpoint accessor, only if CI or mainnet show unknown
addresses). P5a is the only one that changes a running node; P5b and P5c are reviewable alone.

### What the code on master already settles

- `Node::init` (`node.cpp:146-321`) is one function: the pre-chain part (`147-171`: initial
  confidants and permanent neighbours `147-151`, the API object and its five connections
  `153-166`, `solver_->init` and `startDefault` `169-170`, `cachesSerializationManager_.bind(stat_)`
  `171`) **already runs before the chain opens**, so `initCommon()` in bootstrap mode is exactly
  today's code; what is new is the time gap after it and the transport running during that gap.
  The chain part is `173-256` (the `--set-bc-top` branch `173-220` returns early at `219` without
  running the API or the snapshot service; the normal branch `222-256`), then `258-260` (the ready
  line and the full digest), `263` (`api_->run()`), `262-287` (the P3 snapshot service),
  `289-301` (transport and solver checks, the conveyer key), `304-309` (five connections),
  `311-321` (`tryResolveHashProblems`, `initBootstrapRP`, `sendRunningStatus`, `globalPublicKey_`).
- `snapshotFetcher_` is a member already (`node.hpp:515`) and the two reply handlers forward to it
  (`node.cpp:2246-2255`, `2272-2282`); nothing constructs it. `sendSnapshotMessage`
  (`node.cpp:2180-2187`) sends `BaseFlags::Clear` packets through `transport_->sendDirect`.
- `chooseMessageAction` (`node.cpp:2305-2422`) returns `Process` for the four snapshot types in its
  always-process list (`2332-2336`) after the `good_` check (`2306-2308`).
- The transport reads two atomics of an un-opened `BlockChain`: `uuid()` (`blockchain.cpp:384-386`,
  `uuid_` is `std::atomic<uint64_t>`, `blockchain.hpp:570`) in `Neighbourhood::sendVersionReply`
  (`neighbourhood.cpp:62-71`) and `isCompatible` (`125-135`: `myUuid == 0` matches anyone), and
  `getLastSeq()` (`blockchain.cpp:2495-2497`, `lastSequence_` is `std::atomic<cs::Sequence>`,
  `blockchain.hpp:571`, value-initialised to 0 and set to 0 again at `init`, `:153`) in
  `sendVersionReply`, `sendPong` (`76-86`) and `PoolSynchronizer::onPingReceived`
  (`poolsynchronizer.cpp:288-295`, harmless: it records peers ahead of 0). `RoundStat::onPingReceived`
  (`roundstat.cpp`) touches no chain. Nothing else on the transport's threads reaches the chain.
- Slots reached from the transport that must be gated (all run on the processor thread through
  `CallsQueue`, `transport.cpp:276-279`): `Node::onNeighbourAdded` (`node.cpp:417-425`: a
  `RoundPackRequest` when the neighbour's round is ahead), `Node::onPingChecked` (`2167-2178`, from
  `RoundStat::checkPing`, connected at `304-305`), **`Node::onRoundTimeElapsed`** (connected in the
  constructor through `setupNextMessageBehaviour`, `329-332`; reached from `Transport::run`'s
  `mainThreadIterated` every 2 s → `RoundStat::checkRoundElapse` → queued emit every
  `round_elapse_time`: an initial confidant would gather a bootstrap table from block 0; a plain
  node only logs `Waiting for next round...` and calls `initBootstrapRP`), and `chooseMessageAction`.
  `RoundStat::checkStoreBlockElapse` → `PoolSynchronizer::onStoreBlockTimeElapsed` is inert (no
  sync started). `Node::processTimer` runs only once `sendingTimer_` starts inside a round.
- `BlockChain`'s constructor (`blockchain.cpp:39-52`) creates and opens `caches/hashdb` and
  `caches/seqdb` (`BlockHashes::initialization`, `blockhashes.cpp:191-206`; `cs::Lmdb`'s
  constructor creates the folder, `lmdbxx/lmdb.cpp:8-18`) and `caches/indexdb` (`TransactionsIndex`,
  `transactionsindex.cpp:40-60`). **The importer refuses to run when `caches/hashdb` or `caches/seqdb`
  exists** (`snapshot_importer.cpp:187-189`) and opens its own `BlockHashes` on that path
  (`:199-200`); two LMDB environments on one path in one process are not allowed. So the bootstrap
  must close and remove the two empty hash databases before the import and reopen them after
  (`releaseHashDbs()` / `reopenHashDbs()` below). `caches/indexdb` is not touched by the importer and
  stays open (the quick start rebuilds it, `blockchain.cpp:273-280`).
- `BlockChain::close()` (`blockchain.cpp:1361-1415`) on a storage that was never opened:
  `tryFlushDeferredBlock` is a no-op, `getHashBySequence` through `blockHashes_` returns empty,
  `storage_.flush()` returns false (`storage.cpp:685-686`: `!d->db`), `storage_.close()` is safe
  (`:702-713`), then `blockHashes_->close()` and `trxIndex_->close()`, then it returns at
  `blocksOnDisk == false` with the warning `block 0 is not in the database, no caches saved`. It
  would not crash, but it logs a misleading warning and touches `qs/`: the guard of the design's
  6.9 goes in anyway, before anything else.
- The importer's own cleanup runs in process only (`snapshot_importer.cpp:98-115`): after a kill in
  the middle of an import, `db/` holds a partial database, `caches/hashdb` is half written and
  `qs/<S>.tmp` exists. The next start's `csdb::backend::detect` would call that a database and
  `needsSnapshot()` would be false: the node would open a chain that starts at `F > 0` without a
  checkpoint. P5a writes a marker before the import and cleans up at the next start, **before
  `dbcheck::check`** (`main.cpp:193-201`), which would otherwise inspect the partial database.
- `dbcheck::check` takes `db/.lock` only when `db/` exists (`dbcheck.cpp:53-62`): on a first start
  nothing is locked for the whole bootstrap, and a second `node` in the same folder would bootstrap
  too. `runImport` creates `db/` first for this reason (`snapshot_export.cpp`, `runImport`).
- The capability bit is fixed for the life of the process (`transport.cpp:37-45`): a node switched
  to `serve=on` by a config edit announces nothing until it restarts. The design's `-wait` row (a
  config edit seen by the observer) cannot work by discovery; the late server is restarted, or
  listed in the joiner's `sources_file` (configured keys are asked whatever they announce,
  `snapshot_fetcher.cpp:359-391`).
- `SnapshotFetcher::Hooks::addressOf` is called at discovery (`snapshot_fetcher.cpp:151`) and again
  when a manifest arrives (`:435`); an empty string makes the source "not counted: address unknown"
  in the 30-s line (`:520-561`). `Transport::getKnownPeers` (`transport.cpp:440-472`) reads the
  routing table only (`Host::GetKnownNodes` → `RoutingTable::GetKnownNodes`); a passive TCP
  connection does not enter the routing table by itself (`p2p/src/network.cc:254-272`) and a
  k-bucket can drop an entry while the connection lives. `Host::connections_` is private
  (`p2p/src/host.h:107`); `Connection::GetEndpoint()` exists (`connection.h:48`). P5a keeps a
  last-seen address per key on the `Node`; P5c adds the accessor to the fork if needed.
- Every node of a private network starts on an empty database, and `fetch=auto` is the default
  (`config.hpp:81`): with `db_backend=lmdb` or `rocksdb` every node of `run.sh` would enter
  bootstrap mode and wait `fetch_wait` for snapshots that nobody has. **The harness writes
  `fetch=off` for every node but the joiner.** The same applies to any genesis network
  (`docs/differences.md` says so). A BerkeleyDB node (the official default) never bootstraps.
- `netPrefix` collapses every CI node into the one /24 of 127.0.0.1: the p2p scenarios give each
  node its own loopback /24 (`[host_input] ip=127.0.<i>.1`, the same address in every `hosts.txt`
  line; Linux routes all of 127/8 to `lo`), or use `sources=` keys (counted by key).
- `CallsQueue::insert` (`structures.hpp:327-345`) is lock-free and called from any thread today
  (`Transport::onPingReceived`, `transport.cpp:193-197`); `callAll` runs on the processor thread
  only (`Transport::process`, `transport.cpp:276-279`), which also drains it during the bootstrap,
  since `Transport::run` (`transport.cpp:126-147`) starts the processor thread as today.
- `cs::Timer::start(msec, Type::Standard, RunPolicy::CallQueuePolicy)` (`timer.hpp`) queues each
  tick to `CallsQueue`, as `PoolSynchronizer` uses it (`poolsynchronizer.cpp:101-105`); `stop()`
  joins the timer thread and may be called from a slot (`poolsynchronizer.cpp:429`, `440`).
- `Node::stopOnError()` (`node.cpp:4315-4318`) stops the node from inside as a signal does;
  `onStopRequested` (`4320+`) runs on the thread that emits, calls `stop()` and `stop()` is called
  again by `Peer::onRun` (`peer.cpp:130-147`). `main.cpp:232-234` returns `peer.executeProtocol()`
  (0 or -1); there is no exit code of the node's own yet.
- `csconnector::connector`'s constructor (`csconnector.cpp:38-55`) starts the executor API thread
  (`BINARY_TCP_EXECAPI`) at construction, before the chain opens, today as in bootstrap mode; the
  public API starts in `run()` only. A contract executor started by the node could call the
  executor API during the bootstrap: nothing runs a contract without a chain, but the handlers
  read `blockchain_` without a guard (a risk below, covered by the contracts row).
- The manifest has no `sources` field (`snapshot_manifest.hpp:25-66`) and `snapshot.json` is the
  signed manifest as the importer writes it (`snapshot_importer.cpp:332-339`): the sources used
  go to a sibling file `snapshot.sources.json` (design 6.7 said "appended": a sibling keeps the
  manifest byte-identical and `fromJson` untouched; P8 reads the sibling).

### P5a. The node: bootstrap mode, hand-over, verification (large)

**Files.** `csnode/include/csnode/node.hpp`, `csnode/src/node.cpp` (`init` split, constructor
`67-128`, `stop` `368-380`, `destroy` `382-401`, `onNeighbourAdded` `417-425`, `onRoundTimeElapsed`,
`onPingChecked` `2167-2178`, `chooseMessageAction` `2305-2308`), `csnode/include/csnode/blockchain.hpp`
and `csnode/src/blockchain.cpp` (`close()` guard at `1361`, `releaseHashDbs`/`reopenHashDbs`),
new `csnode/include/csnode/snapshot_bootstrap.hpp` + `csnode/src/snapshot_bootstrap.cpp` (free
functions, testable without a `Node`, as `csnode/bootstrapgate.hpp` does for the bootstrap table),
`client/src/main.cpp` (the interrupted-import cleanup and `db/` before `dbcheck::check`
`193-201`; the exit code after `232-234`), `lib/include/lib/system/common.hpp` (`PeerData::key`,
the 32 bytes next to the base58 `id`, filled in `transport.cpp:440-472`), `ci/testnet/run.sh`
(`fetch=off` for every node, `JOIN_FETCH=p2p` and its end checks, one loopback /24 per node),
`.github/workflows/robustness.yml` (two rows), `docs/CHANGES.md`, `docs/differences.md` (private
networks set `fetch=off`), `tests/csnode/snapshot_bootstrap_tests.cpp`.

**Step 1. `snapshot_bootstrap.hpp`: the pure parts.** Written and unit-tested first.

```cpp
namespace cs::snapshot_bootstrap {
// why this start does not fetch a snapshot; nullopt: it does (design 6.2). Pure over the file system.
std::optional<std::string> notNeeded(const SnapshotData& cfg, const std::filesystem::path& nodeDir,
                                     const std::string& dbPath, const std::string& backend, bool setBcTop);
// the action for a message while bootstrapping: the two replies are processed, all else dropped
Node::MessageActions action(MsgTypes type, bool bootstrapping, Node::MessageActions otherwise);
// snapshot.json present and snapshot.verified absent: compare the digest the quick start computed
enum class StartupCheck { NoManifest, AlreadyVerified, Verified, Skipped, Mismatch };
StartupCheck startupCheck(const std::filesystem::path& nodeDir, cs::Sequence lastSeq,
                          const CheckpointDigest& computed, std::string& detail);   // writes .verified / .mismatch
// db/.importing: what an interrupted in-process import left; removed before dbcheck
bool cleanupInterruptedImport(const std::filesystem::path& nodeDir, const std::string& dbPath, std::string& what);
bool writeSources(const std::filesystem::path& nodeDir, const std::vector<SourceManifest>& used);   // snapshot.sources.json
}
```

- `notNeeded` returns the reason, in this order: `fetch=off`; `--set-bc-top`; `db_backend` is not
  `lmdb`/`rocksdb` (`a <backend> database cannot start above block 0`: the conversion rule,
  `storage_convert.cpp:328-334`); `csdb::backend::detect(dbPath, false)` not empty (`storage_backend.hpp`;
  `false`: the engine is not opened, the lock is ours anyway); a `qs/<digits>` directory exists
  (`0` included: a previous run's checkpoint means a previous run; the log says to remove `qs/` to
  fetch); `snapshot.mismatch` present is **not** a reason here, it is a refusal (step 4). It also
  removes `snapshot-tmp/` when it returns nullopt and logs `SNAPSHOT: removed snapshot-tmp/ left by
  an interrupted fetch`.
- `action`: `bootstrapping ? (type == SnapshotManifestReply || type == SnapshotPieceReply ? Process : Drop) : otherwise`.
- `startupCheck`: no `snapshot.json` → `NoManifest`; `snapshot.verified` → `AlreadyVerified`;
  `fromJson` fails → `Mismatch` with the detail (a manifest that does not parse is not one the
  importer wrote); `lastSeq != seq` → `Skipped`, `snapshot.verified` written with
  `skipped: chain at <lastSeq>` (the chain moved past the snapshot before a start verified it:
  the digest at `S` is gone); else `state_digest`, `wallets` and `settings_digest` compared with
  `computed` → `Verified` (file written, content `verified at block S`) or `Mismatch`
  (`snapshot.mismatch` written with both digests). Only the imported head is compared: the
  importer's database ends at `S` and the quick start sets `lastSequence_ = S`, so a restored
  node's first start always has `lastSeq == S` (the `S + 1` label of a shutdown checkpoint,
  `blockchain.cpp:128-134`, concerns live checkpoints of earlier builds, not imports).
- `cleanupInterruptedImport`: when `<dbPath>/.importing` exists: remove the backend files of the
  marker's backend (`csdb::backend::files`, as the importer's `failWith` does, `snapshot_importer.cpp:104-108`),
  `.backend`, `caches/hashdb`, `caches/seqdb`, `caches/blockhashes.rebuild`, every `qs/<digits>.tmp`,
  `snapshot.json`, `snapshot.sources.json`, then the marker; `what` lists what was removed. The
  marker's content is the backend name; `db/.lock` is never removed.
- `writeSources`: `[{"key": base58, "address": "ip"}]` of the agreed sources, 0600 like the
  fetcher's files.

**Tests** (`snapshot_bootstrap_tests.cpp`, temporary directories): `notNeeded` for each reason and
for the fetching case (empty folder, `snapshot-tmp/` removed), `qs/0` and `qs/2000` present,
`detect` on an LMDB folder made by `csdb::backend::open`; `action` for every `MsgTypes` value of
the snapshot family and two others, with and without the flag; `startupCheck` in each of the five
outcomes and the files it writes; `cleanupInterruptedImport` on a folder laid out as a killed
import (an LMDB `db/`, `caches/hashdb`, `qs/2000.tmp`, `snapshot.json`) leaves `db/.lock` and
`caches/indexdb`; `writeSources` round trip through a JSON parse.

**Step 2. `Node::init` split (no behaviour change yet).** `init()` becomes:

```cpp
bool Node::init() {
    if (!initCommon()) return false;                 // node.cpp:147-171 as they are
    if (startBootstrap()) return true;               // step 3; false today
    if (!initChain()) return false;                  // 173-256: both branches, the failure handling of both
    if (!verifyAtStart()) return false;              // step 4
    return initRunning();                            // 258-321 minus the five connections
}
```

- The five connections at `304-309` move to the constructor after `117` (`checkPing`, `pingChecked →
  onPingChecked`, `sendingTimer_.timeOut → processTimer`, `packetFlushed →
  onTransactionsPacketFlushed`, `poolSynchronizer_->sendRequest → sendBlockRequest`), with the
  reason in a comment: `cs::Signal` emits without a lock (`signals.hpp:70-77`) and the processor
  thread is emitting during a bootstrap. They are inert until the chain is up: `checkPing` only
  counts; `onPingChecked` is gated; `processTimer` needs `sendingTimer_`; `packetFlushed` needs
  the conveyer's rounds; `sendRequest` needs `sync()`. `globalPublicKey_` (`318-319`) moves to the
  constructor too.
- `initChain()` keeps the `--set-bc-top` branch's early `return true` by returning an enum
  `{Failed, Ready, StopAfterInit}`; `init()` returns true at `StopAfterInit` without `initRunning()`,
  as today (`219`).
- `initRunning()` = `258-321` in that order, without the moved lines; it also keeps the P3
  service block (`262-287`): **a bootstrapping node has no `snapshotProvider_` and no
  `snapshotService_` until `initRunning()`**, so a request during the bootstrap is dropped by the
  gate before the handler (and the handler's `if (!snapshotProvider_) return;`, `2235-2237`,
  covers the rest). Its bit 24 may be announced meanwhile when `serve=on` (fixed at start): a
  requester sees silence, which the fetcher treats as "not ready" (design 5.3a).
- Check: `testnet.yml` green, `snapshot-join`, `all-storage`, `storage-switch`, `checkpoint-ahead`
  (`--set-bc-top` path) unchanged. This step can be its own commit, reviewed as a pure move.

**Step 3. Bootstrap mode.** New members: `std::atomic<bool> bootstrapping_{false}`,
`cs::Timer snapshotTickTimer_` (its `timeOut` connected to `Node::onSnapshotTick` in the
constructor), `std::thread bootstrapThread_`, `std::atomic<bool> bootstrapStop_{false}`,
`std::set<cs::PublicKey> bootstrapPermanent_`, `std::unordered_map<cs::PublicKey, std::string>
peerAddresses_` (processor thread only), `std::chrono::steady_clock::time_point bootstrapStartedAt_`,
`size_t bootstrapTicks_ = 0`, `std::unique_ptr<cs::SnapshotFetcher> snapshotFetcher_` (exists),
`static std::atomic<int> exitCode_`.

`bool Node::startBootstrap()`:

```cpp
const auto& cfg = ConfigHolder::config()->getSnapshotSettings();
if (auto why = snapshot_bootstrap::notNeeded(cfg, ".", dbPath, backend, newBlockchainTop())) {
    if (cfg.fetch == "auto" && *why is the backend reason) cswarning() << "SNAPSHOT: " << *why << ": syncing from block 0";
    else csdebug() << "SNAPSHOT: not fetching: " << *why;
    return false;
}
if (!transport_->isGood()) return false;            // as initRunning would
SnapshotFetcher::Hooks hooks;
hooks.sendManifestRequest = [this](const PublicKey& to, const ManifestRequest& r) { sendSnapshotMessage(to, MsgTypes::SnapshotManifestRequest, snapshot_p2p::encode(r)); };
hooks.sendPieceRequest    = [this](const PublicKey& to, const PieceRequest& r)    { sendSnapshotMessage(to, MsgTypes::SnapshotPieceRequest,    snapshot_p2p::encode(r)); };
hooks.addNeighbours = [this](const std::set<PublicKey>& keys) {
    bootstrapPermanent_.insert(keys.begin(), keys.end());
    transport_->setPermanentNeighbours(bootstrapPermanent_);   // before the VersionRequest: PeerInfo::permanent is read at the reply (neighbourhood.cpp:95)
    transport_->addToNeighbours(keys);                         // neighbourhood.cpp:220-226
};
hooks.addressOf = [this](const PublicKey& key) { auto it = peerAddresses_.find(key); return it == peerAddresses_.end() ? std::string() : it->second; };
snapshotFetcher_ = std::make_unique<SnapshotFetcher>(cfg, "snapshot-tmp", std::move(hooks));
bootstrapping_ = true;
bootstrapStartedAt_ = now;
cslog() << "SNAPSHOT: empty database: looking for snapshots on the network (fetch=auto, min_sources=" << cfg.minSources
        << ", fetch_wait=" << cfg.fetchWait << " s, fetch_fallback=" << cfg.fetchFallback << ")";
snapshotTickTimer_.start(500, Timer::Type::Standard, RunPolicy::CallQueuePolicy);
return true;
```

The permanent set: `initCommon()` set it to `initialConfidants_` when this node is one
(`147-151`); the hook adds the fetcher's keys to that (`bootstrapPermanent_` starts as the set
`initCommon()` installed) so that sources bypass the 16-neighbour limit (`neighbourhood.cpp:147`,
design 4.3); the fetcher bounds the keys (64 discovered + configured, `snapshot_fetcher.hpp`
`kMaxSources`), and it re-adds a key every 60 s (`kNeighbourRetryMs`) until it is a neighbour.
`finishBootstrap()` restores the set `initCommon()` installed.

`void Node::onSnapshotTick()` (processor thread, every 500 ms):

```cpp
if (!snapshotFetcher_ || bootstrapThread_.joinable()) return;   // a tick queued after the hand-over, or the worker runs
SnapshotFetcher::Peers peers;
if (bootstrapTicks_++ % 2 == 0) {                                 // the routing table once a second
    std::vector<PeerData> known; transport_->getKnownPeers(known);
    for (auto& p : known) { peers.known.emplace_back(p.key, p.capabilities); if (!p.ip.empty()) peerAddresses_[p.key] = p.ip; }
    lastKnown_ = peers.known;
} else peers.known = lastKnown_;
transport_->forEachNeighbour([&](const PublicKey& k, Sequence, RoundNumber) { peers.neighbours.insert(k); });
snapshotFetcher_->tick(peers);
switch (snapshotFetcher_->phase()) {
    case Phase::Import:   startBootstrapWorker(true);  break;   // result() is set
    case Phase::Fallback: startBootstrapWorker(false); break;   // the fetcher logged the 6.4 line
    default: break;
}
```

`peerAddresses_` is the fallback the task asks for: an address seen once in the routing table is
kept for the key, so a k-bucket eviction during the download does not turn a counted source into
an unknown one. A peer that was never in the routing table (an inbound connection only) stays
unknown: the 30-s line counts them (`N not counted: address unknown`), the CI rows require 0
(below), and P5c closes the gap if mainnet shows otherwise. `peerAddresses_` is bounded by the
routing table's size (k-buckets); clear it in `finishBootstrap()`.

`void Node::startBootstrapWorker(bool import)` (processor thread): `snapshotTickTimer_` keeps
running (the tick returns at once while the worker runs); `bootstrapThread_ = std::thread([this, import] { bootstrapWork(import); })`.

`void Node::bootstrapWork(bool import)` (the worker thread; it touches only the chain, the
file system and `CallsQueue::insert`):

1. With `import`: `const auto [manifest, sources] = *snapshotFetcher_->result();` (read before
   the thread starts, captured by value: the fetcher is not touched from the worker);
   `cslog() << "SNAPSHOT: importing block " << S << " (" << MB << " MB) from snapshot-tmp/"`;
   write `<dbPath>/.importing` (content: the backend); `blockChain_.releaseHashDbs()`;
   `const auto r = importSnapshot("snapshot-tmp", manifest, ".", dbPath, backend, &bootstrapStop_)`;
   `blockChain_.reopenHashDbs()`; on `!r.ok`: remove the marker, `cswarning() << "SNAPSHOT: the import refused ... "` (the
   fetcher logs its own line too), `CallsQueue::instance().insert([this, file = r.fileAtFault] { joinWorker(); if (snapshotFetcher_) snapshotFetcher_->onImportFailed(file); })`,
   return (the tick resumes: Collect again, or Fallback after 3). On success: `writeSources`,
   remove `snapshot-tmp/` and the marker, `cslog() << "SNAPSHOT: imported block " << S << " in " << t << " s"`.
   `bootstrapStop_` set → the importer returns `stopped` → treated as a failure path that queues
   nothing (`stop()` is in progress).
2. Without `import` (fallback): remove `snapshot-tmp/`; `cslog() << "SNAPSHOT: opening an empty database: the node syncs from block 0"`.
3. `const auto chain = initChain();` on this thread (the quick start finds `qs/<S>`:
   `CachesSerializationManager::load` lists `qs/` at load time, `caches_serialization_manager.cpp:654+`;
   `removeFailedQuickStartData` as today on failure). `Failed` → `CallsQueue::insert([this] { joinWorker(); failBootstrap(1, "the blockchain could not be opened after the snapshot"); })`, return.
4. `verifyAtStart()` (step 4) → `Mismatch` → `CallsQueue::insert([this] { joinWorker(); failBootstrap(4, ...); })`, return.
5. `CallsQueue::instance().insert([this] { joinWorker(); finishBootstrap(); })`.

`void Node::joinWorker()`: `if (bootstrapThread_.joinable()) bootstrapThread_.join();` (the
worker queued this as its last statement: the join is immediate).

`void Node::finishBootstrap()` (processor thread): `initRunning()` (API, snapshot service,
conveyer key, `initBootstrapRP`, `sendRunningStatus`); then `bootstrapping_ = false` (the first
message processed sees a complete node); `snapshotTickTimer_.stop()`; `snapshotFetcher_.reset()`;
`transport_->setPermanentNeighbours(initialConfidants_ contains me ? initialConfidants_ : {})`;
`peerAddresses_.clear()`; `cslog() << "SNAPSHOT: started from block " << S << " fetched from " << N << " sources in " << T << " s"` (T since `bootstrapStartedAt_`), or after a fallback
`"SNAPSHOT: started with an empty database after " << T << " s without a snapshot"`. The
round-table and block requests that a node makes at start (`onNeighbourAdded`, `onPingChecked`)
were suppressed during the bootstrap: `initRunning()`'s `initBootstrapRP` and the next pings
(every 2 s) bring the round table as for any node that starts behind.

`void Node::failBootstrap(int code, const std::string& why)` (processor thread): `cserror() <<
"SNAPSHOT: " << why << "; stopping (exit code " << code << ")"`; `exitCode_ = code`; `good_ = false`;
`Node::stopOnError()`. Never call `stop()` from the worker: `stop()` joins the worker.

**Gates** (one line each, all on the processor thread):

- `chooseMessageAction`, after `2306-2308`: `if (bootstrapping_) return snapshot_bootstrap::action(type, true, MessageActions::Drop);`
  (requests are dropped too: a bootstrapping node does not serve; the drop is silent).
- `onNeighbourAdded` (`417`), `onPingChecked` (`2167`), `onRoundTimeElapsed`: `if (bootstrapping_) return;`
  at the top (`onNeighbourAdded` keeps its log line before the gate).

**Stop and destroy.** `Node::stop()` (`368-380`), before `poolSynchronizer_->stop()`:
`bootstrapStop_ = true; if (snapshotFetcher_) snapshotFetcher_->stop(); snapshotTickTimer_.stop();
if (bootstrapThread_.joinable()) bootstrapThread_.join();` — `stop()` runs twice (`onStopRequested`
and `Peer::onRun`), all of it idempotent. The worker inside `initChain()` sees
`blockChain_.requestStop()` (set by `onStopRequested`, `4321`) and the importer sees
`bootstrapStop_` between blocks (`snapshot_importer.cpp:208-210`); a join waits for the current
block at most. `Node::destroy()` (`382-401`): `if (api_) api_->stop()`; the rest is guarded already.
`BlockChain::close()` (`1361`): first line `if (!storage_.isOpen()) { stop_ = true; blockHashes_->close(); trxIndex_->close(); return; }`.
`BlockChain::releaseHashDbs()`: `blockHashes_->close(); blockHashes_.reset(); remove_all(caches/hashdb, caches/seqdb)`;
`reopenHashDbs()`: `blockHashes_ = std::make_unique<BlockHashes>(cachesPath)` (its `initialization`
connects the LMDB `failed` signals on the worker thread; those signals are emitted by that LMDB
only). Both are called on the worker while nothing reads `blockHashes_` (the chain is closed,
messages are dropped, the public API is not running). `BlockChain::close()`'s early return above
guards `blockHashes_` null with `if (blockHashes_)`.

**Step 4. Verification at start (design 6.8) and the exit code.** `bool Node::verifyAtStart()`:
`startupCheck(".", blockChain_.getLastSeq(), blockChain_.checkpointDigest(), detail)`
(`blockchain.cpp:1078-1085`); `Verified` → `cslog() << "SNAPSHOT: snapshot.json verified at block " << S`;
`Skipped` → `cswarning() << "SNAPSHOT: block S of snapshot.json is behind the chain (last block L): digest not compared"`;
`Mismatch` → `cserror() << "SNAPSHOT DIGEST MISMATCH at block " << S << ": snapshot " << d1 << ", node " << d2 << "; stopping (exit code 4)"`,
`exitCode_ = 4`, return false. Called from `init()` after `initChain()` (a directory restored by
`--import-snapshot` today, or by `--download-snapshot` after P6) and from the worker after the
in-process import. `init()` also refuses to start when `snapshot.mismatch` exists, before
`initCommon()`: `cserror() << "SNAPSHOT: snapshot.mismatch is present: the state restored from a snapshot did not match its digest; remove db/, caches/, qs/, snapshot.json and the file to start again"`,
`exitCode_ = 4`, return false. `main.cpp:233-234`: `int result = peer.executeProtocol(); if (Node::exitCode() != 0) result = Node::exitCode(); return result;`
(a refused init returns -1 from `executeProtocol`; the node's code wins). The entrypoint's own
digest wait (`entrypoint.sh:156-190`) stays until P6; the two agree (same file names, same code 4).

**Step 5. `main.cpp`.** Before `dbcheck::check` (`193`): `fs::create_directories(dbPath)` (so
that `check` takes `db/.lock` for the whole run, including a bootstrap, `dbcheck.cpp:218`; a
missing `db/` meant "first start" only for the lock) and
`if (snapshot_bootstrap::cleanupInterruptedImport(".", dbPath, what)) cslog() << "SNAPSHOT: removed the files of an interrupted import (" << what << ")"`.
Both before `--import-snapshot` (`204-206`), which creates `db/` itself and would refuse on the
partial database otherwise.

**Step 6. `run.sh` (the minimum for two rows).**

- The `[snapshot]` section of every node gets `fetch=off` (`run.sh:402`: the section is written for
  serving nodes only today; now always, `serve=on` added for `SNAPSHOT_SERVE_NODES`).
- `JOIN_FETCH=p2p` (with `JOIN_PRUNED_AT` as the start sequence and `JOIN_FULL` as today): the
  joining node `NODES + 1` is written with `fetch=auto`, `min_sources=$JOIN_MIN_SOURCES` (default 3),
  `fetch_wait=$JOIN_WAIT` (default 300), `fetch_fallback=$JOIN_FALLBACK` (default `sync`), no
  `--import-snapshot`, and starts at `JOIN_PRUNED_AT` through the existing branch (`624-627`).
  With `BIN_ALT_OFFICIAL=1` `[#103]`: `! official_node "$JOIN_NODE" || refuse "a p2p snapshot join into n$JOIN_NODE"`,
  next to the import refusal.
- Loopback /24 per node when `JOIN_FETCH=p2p` (`NODE_NETS=1`, implied): `node_ip i` = `127.0.$i.1`,
  used in `[host_input] ip=` and in every `hosts.txt` line (`run.sh:380`, `392-393`);
  `digest_offline` (`498-528`) rewrites the port only and keeps working; `net_fault_on` matches
  ports; the API stays on 127.0.0.1 (it binds every interface). The official binary reads the same
  keys.
- End checks for `JOIN_FETCH=p2p` (next to `1174-1187`), on the joiner's log: `SNAPSHOT: block <S> hash` …
  `: N sources` with `N >= JOIN_MIN_SOURCES`; `SNAPSHOT: fetched block <S>`; `SNAPSHOT: imported block <S>`;
  `Caches for QUICK START loaded successfully`; `SNAPSHOT: snapshot.json verified at block <S>`;
  `SNAPSHOT: started from block <S> fetched from`; `snapshot.verified` and `snapshot.sources.json`
  exist, `snapshot-tmp/` and `db/.importing` do not; the digest check of `1181-1186` reused
  (`STATE DIGEST #S` equals the manifest's); every `asking <key> for its manifest` line names a
  key of `SNAPSHOT_SERVE_NODES` (never an even node's); every `peers serve snapshots` line, if any,
  says `0 not counted: address unknown`; each serving node whose key is in the agreement line
  logged `SNAPSHOT: served block <S> to <joiner key>`; with `BIN_ALT_OFFICIAL=1`, no even node's
  log contains `Unknown message type` or `unknown` for types 40-43 (the official validator's
  warning, `packetvalidator.cpp:146-149`); state digests equal at the end as for every scenario.

**Step 7. Rows** (`robustness.yml`, next to `snapshot-join`, `275-290`; `fixed: true`; the even
nodes BerkeleyDB so that the same rows run with `node_alt_ref=official`; `NODES=8 TRUSTED_NODES=7`
so that the joiner is n9, odd, in every variant — `TRUSTED_NODES` odd 5-25 and `NODES` above it
are allowed, `run.sh:5-11`):

```yaml
- name: snapshot-p2p-join
  env: NODES=8 TRUSTED_NODES=7 NODE_STORAGE=lmdb,berkeleydb,lmdb:pruned,berkeleydb,rocksdb,berkeleydb,lmdb,berkeleydb,lmdb:pruned SNAPSHOT_SERVE_NODES=3,5,7 JOIN_FETCH=p2p JOIN_PRUNED_AT=2300 JOIN_MIN_SOURCES=3 TARGET_SEQ=3300
  fixed: true
- name: snapshot-p2p-join-full
  env: NODES=8 TRUSTED_NODES=7 NODE_STORAGE=lmdb,berkeleydb,lmdb:pruned,berkeleydb,rocksdb,berkeleydb,lmdb,berkeleydb,rocksdb SNAPSHOT_SERVE_NODES=3,5,7 JOIN_FETCH=p2p JOIN_PRUNED_AT=2300 JOIN_FULL=1 JOIN_MIN_SOURCES=3 TARGET_SEQ=3300
  fixed: true
```

n3 (pruned LMDB), n5 (full RocksDB) and n7 (full LMDB) serve; their `blocks.bin.lz4` fall in two
`sha256` groups (n3's starts at its prune floor), the checkpoint `.dat` files in one group each:
the fetch spreads over the three manifests' declarers as the fetcher does. No `official:` mark: the
row runs with official even nodes as it is (the joiner and the sources are odd; the even nodes
are neighbours that never serve and are never asked). Both rows twice in a row green before the
merge, plus `testnet.yml`, `snapshot-join`, `all-storage`, `storage-switch`, `checkpoint-ahead`,
`contracts-dice-join` unchanged, and one run of each row with `node_alt_ref=official`.

**Docs.** `docs/CHANGES.md` (one line); `docs/differences.md` ("Snapshots" section: a node on
an empty LMDB or RocksDB database with `fetch=auto` looks for snapshots on the network first;
private and genesis networks set `fetch=off`; `snapshot.verified` / `snapshot.mismatch` and the
exit code 4). The operator page `docs/snapshots.md` (not on master yet) comes with P5b.

**Pass.** Unit tests green; the two rows green twice, once with official even nodes; the
unchanged scenarios green; a node of the private network with `fetch=auto` and a database never
logs `SNAPSHOT: empty database`.

### P5b. Harness: faults, forged and corrupt sources, the late server, docs (medium)

**Files.** `ci/testnet/run.sh`, `.github/workflows/robustness.yml`, `csnode/src/snapshot_builder.cpp`
and `csnode/src/snapshot_provider.cpp` (two `CREDITS_TESTNET` hooks), `docs/snapshots.md` (new),
`docs/differences.md`, `docs/CHANGES.md`.

**Test-build hooks** (`#ifdef CREDITS_TESTNET`, read once at the first use, like
`testnet-convert-kill`, `run.sh:787-799`): `./testnet-forge-snapshot` present → `buildSnapshot`
signs a manifest whose `state_digest` has its last hex digit flipped (the files are untouched: the
forged source is consistent with itself except for the declared digest, which is what the
agreement compares); `./testnet-corrupt-pieces` present → `SnapshotProvider::onPieceRequest`
flips one byte of every third piece it serves (the chunk hash in the manifest is right, the
bytes are not: the fetcher's `rejected piece` path and the ban).

**`run.sh` additions.**

- `SNAPSHOT_LATE_SERVE_NODES=7`: the node gets the serving storage settings
  (`checkpoint_every=1000`, `checkpoint_keep=2`) and `serve=off`; `JOIN_LATE_SERVE_AFTER` (default
  90) seconds after the joiner started, the harness stops it, sets `serve=on` in its `config.ini`
  and starts it again (`start_node`): the bit is fixed at start, a restart is what an operator does.
  Its `SnapshotService` builds the snapshot of its latest checkpoint at start (`buildLatest`,
  `snapshot_service.cpp:107+`) and serves within a minute.
- `JOIN_SOURCES=keys`: the joiner's `[snapshot]` gets `sources=` with the keys of
  `SNAPSHOT_SERVE_NODES` and every node stays on 127.0.0.1 (one /24: only the trusted-by-key count
  can reach `min_sources`; the agreement line must say `(trusted)` for the chosen group).
  `JOIN_SOURCES=file` writes them to the joiner's `snapshot_sources.txt` instead (`sources_file`,
  not trusted: with one /24 the join must fall back; used with `-wait` to list the late server).
- `JOIN_KILL_DURING_FETCH=1`: the main loop greps the joiner's log every 2 s; on the first
  `SNAPSHOT: fetching block` line `kill -KILL`, `start_node` again; on the first
  `SNAPSHOT: importing block` line the same, once each; the restart test (`RESTART_NODES`) never
  names the joiner.
- `FORGE_NODE=n`, `CORRUPT_NODE=m`: touch the hook files in those nodes' folders before they start
  (odd nodes only; `refuse` with `BIN_ALT_OFFICIAL=1` otherwise).
- Refusals `[#103]`: `FORGE_NODE`, `CORRUPT_NODE`, `SNAPSHOT_LATE_SERVE_NODES` on an even node.

**Rows** (all `fixed: true`, `NODES=8 TRUSTED_NODES=7`, storage as `snapshot-p2p-join` unless said):

| Row | Env (on top of the join row's) | Must show (on top of the join checks) |
| --- | --- | --- |
| `snapshot-p2p-few` | `SNAPSHOT_SERVE_NODES=3 JOIN_MIN_SOURCES=2 JOIN_WAIT=60 JOIN_FALLBACK=sync` | `1 source declares block S (need 2): waiting`; `no agreement of 2 community sources after 1 min` … `Syncing from block 0`; `SNAPSHOT: opening an empty database`; `SLOW START`; no `snapshot.json`; the target reached |
| `snapshot-p2p-wait` | `SNAPSHOT_SERVE_NODES=3,5 SNAPSHOT_LATE_SERVE_NODES=7 JOIN_MIN_SOURCES=3 JOIN_WAIT=60 JOIN_FALLBACK=wait` | `Waiting for more sources (fetch_fallback=wait)` at least once; n7's `SNAPSHOT: serving block` after its restart; then the join checks with n7 among the `served block` lines |
| `snapshot-p2p-wait-file` | as `-wait` with `JOIN_SOURCES=file` | the same, n7 asked from the start (`asking <n7>` before its restart) |
| `snapshot-p2p-dropped` | `JOIN_KILL_DURING_FETCH=1` | two kills logged by the harness; `removed snapshot-tmp/ left by an interrupted fetch` once; `removed the files of an interrupted import` once (`main.cpp`, in `stdout.log`); the join checks |
| `snapshot-p2p-forged` | `SNAPSHOT_SERVE_NODES=1,3,5,7 FORGE_NODE=7 CORRUPT_NODE=5 JOIN_MIN_SOURCES=3` | `minority:` line with n7's digest; `rejected piece` from n5 at least once and `source <n5> … not asked again`; n7 never in a `served block` line of the agreed group; the join checks with `N sources` ≥ 3 |
| `snapshot-p2p-sources` | `JOIN_SOURCES=keys` | the agreement line's group is `(trusted)`; the join checks |
| `snapshot-p2p-contracts` | `CONTRACTS=1 CONTRACTS_AFTER_AT=2400 TARGET_SEQ=2600`, `contracts: true` | the join checks; the contract states of the joiner equal the others' at the end (the harness's existing comparison); no executor-API error in the joiner's log during the bootstrap |

The design's `-mixed` row is every row above run with `node_alt_ref=official` (the nightly
second run of `testnet.yml` `[#103]` covers the join rows once they are in `robustness.yml`'s
selection; `robustness.yml` is run by hand with `node_alt_ref=official` and `only=` the p2p rows
before the merge), plus one manual run with `node_alt_ref=<the last master before #100>` to show
that a community node of an earlier build drops the messages as the official one does.

Known official behaviours `[#103]` and these rows: an official joiner diverges on a balance-reading
contract (the joiner is community here); an official node killed while saving a checkpoint does
not restart (no row kills an even node). `-dropped` kills the joiner only.

**Docs.** `docs/snapshots.md`: what the operator sees (`fetch=auto`: the waiting lines, the
fallback and `--download-snapshot` after P6; `serve=on`; the keys; `snapshot.verified`,
`snapshot.mismatch`, exit code 4; `snapshot.sources.json`; private networks: `fetch=off`); a
line in `differences.md` pointing to it; `CHANGES.md`.

**Pass.** Every row green twice; the p2p rows green once with `node_alt_ref=official`.

### P5c. p2p fork: the address of a connected peer (small; only if needed) [p2p submodule]

If `peers serve snapshots … N not counted: address unknown` shows `N > 0` on the testnet or on
mainnet: `bool Host::GetConnectionEndpoint(const NodeId&, std::string& address)` reading
`connections_` under `conn_mux_` (`p2p/src/host.cc:328-347`, `Connection::GetEndpoint()` with the
`error_code` overload of `remote_endpoint`), `Transport::peerAddress(key, out)` trying the routing
table first (`RoutingTable::HasNode`) and the connection second, and `addressOf` using it before
`peerAddresses_`. Submodule pointer bump, `docs/differences.md`. Not needed for CI (every peer of a
private network is in every routing table).

### Risks and how each is tested

| Risk | Where | Mitigation | Test |
| --- | --- | --- | --- |
| The flag switch races a message in flight | `finishBootstrap` sets `bootstrapping_ = false` | the switch happens on the processor thread, between two messages; `initRunning()` completes before it | the join rows: no crash, no `Unknown`/`dropped` line after `started from block` for a message the node should process; `-dropped` restarts land on both sides of the switch |
| A signal connected while the processor thread emits | the five connections of `304-309` | moved to the constructor; nothing connects after `Transport::run` (the P3 service connects `checkpointSavedEvent` in `initRunning()`: that signal is emitted by the chain's thread only, which is the same processor thread at that point, so it is a same-thread connect; move it to the constructor too, guarded by `if (snapshotService_)` in the slot, if the reviewer prefers) | TSAN build of the unit tests; a grep in review for `Connector::connect` outside constructors |
| The worker and the processor thread touch the chain together | `initChain()` on the worker while messages are dropped | the gate drops everything but the two replies, which touch only the fetcher; `forEachNeighbour`, `getKnownPeers` take their own mutexes; `getLastSeq`/`uuid` are atomics | the join rows under `CONTRACTS=1` (`-contracts`), `-dropped` (kills during `initChain`) |
| `Node::stop()` from the worker (deadlock) | `failBootstrap` | the worker only queues; `stopOnError` runs on the processor thread | the forced mismatch: a unit test of `startupCheck`; `-few` (fallback path through the worker and `finishBootstrap`) |
| A tick after the fetcher is gone | timer ticks queued before `stop()` | `onSnapshotTick` checks `snapshotFetcher_` first; `finishBootstrap` stops the timer before resetting the fetcher | every row (the hand-over) |
| The importer refuses: hash databases present | `BlockChain`'s constructor | `releaseHashDbs()` before, `reopenHashDbs()` after; `close()` guards null | the join rows (the import runs in process); `snapshot_import_tests` unchanged |
| A kill during the import leaves a partial database the next start opens | `db/`, `caches/hashdb`, `qs/<S>.tmp` | `db/.importing` marker + `cleanupInterruptedImport` before `dbcheck::check` | `-dropped` (second kill), the unit test on a laid-out folder |
| A BerkeleyDB node | the official default backend | `notNeeded` returns the backend reason; the node syncs from 0 as today, with one warning when `fetch=auto` | unit test; `all-storage` rows unchanged (their BerkeleyDB nodes start empty) |
| Every node of a genesis network bootstraps | `fetch=auto` default | `run.sh` writes `fetch=off` for all but the joiner; `differences.md` says so; the Docker entrypoint restores before the start (a failed restore then bootstraps by design) | `testnet.yml` green; a private network with `fetch=auto` on every node is **not** supported, by documentation |
| The config observer swaps the config during a bootstrap | `ConfigHolder::config()` raw pointer (design 5.3a) | the fetcher copies `SnapshotData` at construction; `notNeeded` reads once; `startBootstrap` reads once; no new reader on another thread | `-wait` (the observer runs on every node; nothing of the bootstrap re-reads) |
| API objects touch the chain before `initChain` | `csconnector` constructor starts the executor API thread at construction (`csconnector.cpp:55+`); `api_->run()` only in `initRunning` | as today before the chain opens, now for minutes; nothing calls the executor API without a contract to run; `getKnownPeers` of the diag API reads the transport only | `-contracts`: the joiner's executor starts with the node; the joiner's log has no executor-API error before `started from block` |
| The P3 `SnapshotService` on a bootstrapping node | `serve=on` with an empty database | not constructed until `initRunning()`; requests dropped by the gate; bit 24 announced meanwhile means silence to requesters (treated as not ready) | a join row variant by hand with the joiner's `serve=on`: no `SNAPSHOT: built`/`serving` line before `started from block`, and a `serving block` line after its first checkpoint |
| `onRoundTimeElapsed` on an initial confidant starting empty | an initial confidant with `fetch=auto` | gated; initial confidants of a private network have `fetch=off` anyway | no unit test (a `Node` cannot be built in a test): the gate is one reviewed line; a manual run of a join as an initial confidant (`TRUSTED_NODES` including the joiner) once |
| The permanent set left extended | `setPermanentNeighbours` during the bootstrap | restored in `finishBootstrap`; the serving sources stay ordinary neighbours until evicted | `-forged`: n5 banned by the fetcher is still a neighbour (no harm) and leaves the permanent set at the end: a `showNeighbours` dump is not needed, the join rows pass |
| The routing table lies about an address (keys unauthenticated) | `peerAddresses_` | nothing new: the design's independence by address holds for the routing table's own view; `sources=` is the trusted path | `-sources` |
| The pruned mainnet package waits `fetch_wait` on its first start until servers exist | `fetch=auto` default on LMDB | a release note (P11); the Docker image restores from GitHub first; `fetch=off` or `--download-snapshot` (P6) for an operator in a hurry | the mainnet step of P11 |

**Sizes.** P5a large (about 700 lines with tests, half of it a move), P5b medium (harness and
two hooks), P5c small.

## Step P6. `--download-snapshot`, the workflow on format 2, Docker (medium; client side and YAML, can run in parallel with P3-P5 after P0)

**Files.** `client/src/main.cpp:52-66` (the option, handled after `dbcheck::check` next to
`--import-snapshot` at 203-206: no keys needed), `client/src/snapshot_export.cpp` (`runDownload`),
`lib/include/lib/system/process.hpp` use; `.github/workflows/snapshot.yml:81-155`;
`packaging/docker/entrypoint.sh` (`:17-18`, `35`, `41-45`, `105`, `130-138`, `156-190`),
`packaging/docker/Dockerfile` (`curl` present; `zstd` no longer needed for the restore),
`packaging/docker/docker-compose.yml`, `.env.example`, `README.md`,
`packaging/docker/test/test-entrypoint.sh:81-93`, `packaging/docker/test/test-snapshot.sh`,
`packaging/make-package.sh` (nothing new: the binary has the tool), `ci/testnet/run.sh`
(`snapshot-download` scenario), `docs/snapshots.md`, `docs/specs/2026-10-16-docker-and-pruned-snapshots-design.md`
(a note that format 2 replaces the scripts), `docs/CHANGES.md`.

**Change.**

- `runDownload(config, url)` as the design's section 9: refuse next to a running node
  (`dbcheck::lockedByAnotherProcess`) or with a database present; `curl` found on the `PATH`
  (`boost::process::search_path`), else exit 2 with the message; manifest to
  `snapshot-tmp/snapshot.json` (60 s, `--max-filesize 1048576`), `fromJson` + `verify()` + limits;
  the GitHub asset name `s<seq>-<name>` composed from the manifest's `seq` for every file, each
  with `curl -fL -C - --max-time 3600 --retry 3 -o snapshot-tmp/<name>`; size, chunk and whole
  hashes; `importSnapshot`; `snapshot.json` written by the importer; `snapshot-tmp/` removed on
  success, kept on failure (resumable); progress lines per file. Exit 0 / 2.
- `snapshot.yml`: after "Stop and check the logs", `./node --export-snapshot out2/` in the
  node's directory; the "Check" step verifies the manifest (`fromJson` shape through a small
  Python check as today's: every file's size and `sha256`, `format == 2`); "Publish" uploads
  `out2/<seq>/snapshot.json` with `--clobber` and every file as `s<seq>-<name>`, deletes assets
  of sequences older than the 3 newest; the "skip when the checkpoint is the one the run started
  from" rule (`latest.json`/`snapshot.json` of the release read at the start); format 1 kept for
  one release (both jobs), then removed in a follow-up PR with the scripts.
- Entrypoint: `CREDITS_SNAPSHOT=github|p2p|off` (default `github`); on the first start with an
  empty data directory and `github`: `"$OPT/node" --download-snapshot "$CREDITS_SNAPSHOT_URL"`
  (default the release `snapshot.json`) in `$DATA`, `log` its outcome, continue to the normal
  start whatever it was; `p2p`: nothing; `off`: write `fetch=off`. `CREDITS_SERVE_SNAPSHOT`
  (default `on`) → `serve=on`; `CREDITS_SNAPSHOT_MIN_SOURCES` → `min_sources`. The restore and
  `.restore-incomplete` paths and the digest wait removed (the node verifies in process, P5);
  `snapshot.mismatch` refusal kept. `docker-compose.yml`/`.env.example`/`README.md` follow; the
  README's storage table says the three modes and that serving is on.
- `test-entrypoint.sh`: the stub node records its arguments: `github` → `--download-snapshot`
  seen then a normal start; `p2p` → no download, `fetch=auto` in the written `config.ini`;
  `off` → `fetch=off`; `snapshot.mismatch` still refused; the old restore cases removed.
- `run.sh` `snapshot-download`: a `python3 -m http.server` over a directory laid out as the
  release (`snapshot.json`, `s<seq>-<name>`) from `--export-snapshot` of n6 at `SNAPSHOT_AT`;
  n8 runs `--download-snapshot http://127.0.0.1:<port>/snapshot.json`, then starts and joins; a
  second run with one file truncated must exit 2 and leave no `db/`.

**Tests.** The scenario above; `test-entrypoint.sh`; a manual run of the workflow on the branch
publishes `snapshot.json` + files to a test release (`RELEASE` input) and a container restores
from it.

**Pass.** `docker.yml` green with the mainnet smoke test on the GitHub format-2 download; the
scheduled `snapshot.yml` publishes both formats once; `packaging/docker/test/test-snapshot.sh`
adapted or retired with the scripts.

## Step P7. Load, limits, stress and scale (medium, CI only)

**Files.** `ci/testnet/run.sh` (`JOIN_SEQUENCE=2` for two joins in a row; `JOIN_PARALLEL=4` for
joiners started together; `JOIN_FAULTS` per joiner and `SERVE_FAULTS` per serving node, on top of
the existing `NET_FAULT` cgroup/netem band (`run.sh:411-430`), one netem band per fault value;
`JOIN_KILL=n11:40-90` kills and restarts a joiner at random intervals during the fetch and the
import; the rate measurement from the joining nodes' progress lines; the round-time extraction
from the logs; resident memory sampled per node), `ci/testnet/sampler` (#68) use,
`tests/tools/snapshot_flood` (a small test-only binary built with the tests: connects as a peer,
asks manifests and pieces from every serving node as fast as it can without honouring
`busy`/`retry_ms`, drops connections in the middle of replies, sends truncated and out-of-range
requests), `.github/workflows/robustness.yml` (`snapshot-p2p-trusted`, `-limits`, `-stress`),
`.github/workflows/scale.yml` (documentation of the `scenario` values: `SNAPSHOT_SERVE_NODES=…
JOIN_FETCH=p2p JOIN_AT=…` pass through `inputs.scenario`, `scale.yml:216-226`, whose regex
`^[A-Z_]+=[a-z0-9,:]+$` already admits them; the faults are given as short aliases,
`JOIN_FAULTS=n9:slow,n10:lossy`, mapped to netem values in `run.sh`, so the regex stays as is).

**Change.** The three scenarios of the design's section 13 with their pass numbers:

- `-trusted`: block rate within 10 %, round p95 within 20 %, no fork.
- `-limits`: measured rate within 20 % of the cap, busy answers logged.
- `-stress`, the ad hoc test for the nodes in consensus: every initial confidant serves; four
  joiners start together under contract load, one clean, one on a slow link, one with loss and
  delay, one killed and restarted repeatedly; one serving confidant itself on a slow link and one
  with a low `serve_rate_kbps`; the flood client running throughout. A baseline run with the same
  faults and no joiners or flood client is made in the same job and the two compared. Must show:
  block rate within 10 % and round p95 within 20 % of the baseline; no fork; no stage timeout or
  writer change caused by a serving node (stage logs checked against the serve lines'
  timestamps); consensus messages to the slow confidant within the 10-s deadline. The single FIFO
  queue per peer is the risk here: a piece queued before a round message delays it, so the piece
  size and the outstanding pieces per peer must keep that delay under about 1 s at 4 Mbit/s.
  Also: the flood client throttled and its bucket logged; each serving node at most 64 MiB of
  resident memory above its idle level; the joiners complete, or the slow one falls back to
  `sync` after `fetch_wait` with the logged line; state digests equal at the end.

Scale runs by hand: `scale-t7-n33` and `-n51` with the serving nodes among the low-stake and among
the trusted ones, and `-stress` on 51 nodes with eight joiners; recorded in the design's section 14.

**Pass.** All three scenarios green twice; the scale numbers recorded. If `-trusted` or `-stress`
fails its thresholds, the design's section 8 knobs are tried in this order: 256 KiB pieces, fewer
outstanding pieces per peer while trusted, `serving_trusted` sources last, no serving while
trusted (a question to the user, design 15.4).

## Step P8. Weekly job: serving nodes and groups (small, Python and YAML; after P5)

**Files.** `client/src/main.cpp` (`--survey-snapshots [seconds]`, needs keys: after 208-217),
`csnode/src/node.cpp` (`Node` constructed with `surveyMode_`: bootstrap mode with the fetcher in
`dryRun`, no chain, exits with the report after the time), `.github/workflows/known-hosts.yml`,
`ci/known-hosts/run-weekly.sh`, new `ci/known-hosts/snapshot_sources.py` (from
`known_peers.json` and the survey output to `snapshot_sources.txt` and `snapshots.md`, the /24
helper of `collect.py`), `packaging/make-package.sh:28-29` (ship `snapshot_sources.txt` when
present), `packaging/docker/Dockerfile` (copy it next to `known_hosts.txt`).

**Change.** The survey prints one line per group `(seq, head_hash, state_digest,
settings_digest, created, sources: key@ip …)` and one per serving peer that did not answer;
exit 0. The job runs it for 600 s after `collect.py`, in the same node directory; the publish job
uploads the two files and adds `packaging/mainnet/snapshot_sources.txt` to the pull request.

**Tests.** A `--self-test` of `snapshot_sources.py` on fixture files; the survey on the private
network (`run.sh` runs it from n8's directory at the end of `snapshot-p2p-join`: the three serving
nodes and their group appear).

**Pass.** The workflow publishes `snapshot_sources.txt` and `snapshots.md`; the first
`snapshots.md` reviewed by hand (the number of serving sources decides question 1).

## Step P9. p2p: a cap on the announced frame size (small) [p2p submodule] — flagged, question 3

**Files.** `p2p/src/connection.cc:91-97` (and `connection.h`), the submodule pointer in the
node repository, `docs/differences.md`.

**Also (P2 review).** In `RoutingTable::UpdateKBuckets`, when the sender of a direct datagram is
already in its bucket, update the entry (`bucket.Update(node)`; `CheckEndpoint` already matched
its address and port) so that a changed `host_data` is seen without waiting for an eviction.

**Change.** `kMaxPacketSize = 256 MiB`; a header announcing more drops the connection with
`kProtocolCorrupted` and a counted log line (`LOG(ERROR)` once a minute per peer). Chosen above
every legitimate packet (the inbox discards the second-priority list above 512 MB in total, so no
single legitimate packet approaches 256 MB). Independent of snapshots; protects every message
type; the official node has no cap.

**Tests.** A p2p unit test with a fake socket is heavy; the private-network `net-corrupt` and
`garbage-datagrams` scenarios plus a new `net-huge-frame` (a Python client sends a header with
`data_size = 2^40` to a node: the node drops the connection, keeps running, no allocation).

**Pass.** `net-*` scenarios green; the node's memory flat in `net-huge-frame`. Not merged
without the user's answer to question 3.

## Backward sync steps, interleaved

- **BS1** (`Storage::pool_save_backfill`) and **BS2** (`BlockChain` anchor/store/flush, online
  index, `BlockHashes::repairBottom`): as planned on `origin/docs/backward-sync`; no transport
  code; can run in parallel with P1-P4 by a second worker.
- **BS3** (`cs::BackwardSync`, Node wiring): after P5, so that the two processor-thread state
  machines (`SnapshotFetcher`, `BackwardSync`) are reviewed against the same `Node`; BS3's
  `Context` adds `bootstrapping` (no backfill while bootstrapping: the chain is not open).
- **BS-F. The floor (small, after BS3).** `[storage] backfill_floor` is not a key: the floor is
  computed: a pruned node (`prune_keep_blocks > 0`) backfills down to `F = max(0, C − (checkpoint_every
  + prune_keep_blocks))` where `C` is the checkpoint it started from (`cachesSerializationManager_
  .getLoadedVersion()`), a full node to 0; `BackwardSync::isActive()` becomes `first > floor`;
  `drain()` stops at `floor` with `BACKFILL: stored down to the floor F (pruned node)`; the
  backward-sync design's section 1 ("a pruned node never backfills") and its plan's step 3 text
  are amended. Tests: the unit test (8) of BS3 with a floor; `storage-switch`/`backfill-*`
  unchanged (full nodes); a new case in `backfill-join` with the joining node pruned.
- **BS4-BS6** as planned (BS4's "RocksDB from the LMDB snapshot" conversion at the first start is
  no longer needed: the importer writes the configured backend; that part of BS4 is dropped, the
  entrypoint parts of BS4 merge into P6's entrypoint).

## Step P10. State-only snapshots by default (small, after BS-F and P6)

**Files.** `client/config/config.hpp:81` (`snapshotBlocks = 1`), `packaging/mainnet/config.ini`,
`ci/testnet/run.sh` (`snapshot-p2p-state-only`), `.github/workflows/robustness.yml`,
`docs/snapshots.md`, `docs/specs/2026-10-24-snapshot-serving-design.md` (section 2's table:
`blocks.bin.lz4` holds block `S`), `docs/CHANGES.md`.

**Change.** The default flips; the builder, importer, provider and fetcher need nothing; the
GitHub snapshot shrinks at the next workflow run; a serving node that still runs the old default
serves the long form and both are accepted.

**Tests.** `snapshot-p2p-state-only` as the design's table (pruned join backfills to its floor,
full join to 0, histories equal n1's at the end); `snapshot-download` on a state-only release
layout.

**Pass.** Scenario green twice; a container from the GitHub state-only snapshot reaches the head
and backfills to its floor.

## Step P11. Testnet trial, then mainnet (operations)

1. Every step above runs on the private networks as it is merged; after P7, two scale runs with
   joins; after P8, a 50-node run where the weekly-job survey is executed from a node.
2. Mainnet, in this order: the local pruned node `serve=on` (2 MB/s); a container on this host
   with `CREDITS_SNAPSHOT=p2p`, `min_sources=1`, `sources=<local key>` joins (the first p2p join
   on mainnet; join time, bytes, round effect on the serving node recorded); the weekly job's
   first `snapshots.md`; then the release that turns `serve=on` in the packages and the Docker
   default `CREDITS_SNAPSHOT=github` with `fetch=auto` behind it; then, with the serving count
   known, the `min_sources` decision (question 1).
3. `docs/CHANGES.md` on each merge; the release notes name the new `[snapshot]` keys and the
   removed ones.

## Sizes and order

| Step | Content | Size | Depends on | Marks |
| --- | --- | --- | --- | --- |
| P0 | `[snapshot]` keys for p2p | S | — | — |
| P1 | messages, codecs, dispatch | S | — | protocol |
| P2 | capability byte, `known_peers.json` | S | P0 | protocol (`host_data`) |
| P3 | service from #98, provider, builder chunk 512 KiB | M | P0, P1, P2 | — |
| P4 | agreement, fetcher (unit-tested, not wired) | L | P1 | — |
| P5 | bootstrap mode, hand-over, verification, scenarios (P5a node, P5b harness, P5c p2p if needed) | L | P3, P4 | P5c: p2p submodule |
| P6 | `--download-snapshot`, workflow format 2, Docker | M | P0 (P5 for the smoke test) | — |
| P7 | trusted/limits/stress scenarios, scale runs | M | P5 | — |
| P8 | survey, weekly job | S | P5 | — |
| P9 | p2p frame cap | S | — | p2p submodule, question 3 |
| BS1, BS2 | backward sync storage and index | S, M | — | — |
| BS3 | `BackwardSync` | L | BS2, P5 | — |
| BS-F | the floor for pruned nodes | S | BS3 | — |
| BS4-6 | packaging, scenarios, BerkeleyDB | M, M, S | BS3 | — |
| P10 | `snapshot_blocks = 1` default | S | BS-F, P6 | — |
| P11 | testnet trial, mainnet, release | ops | all | — |

Critical path: P0 → P1 → P2 → P3 → P5 (P4 in parallel with P2-P3 by the same or a second
worker, it only needs P1's structs). P6 and BS1-BS2 are independent of that path and suit a
second worker. P9 any time, after the user's answer. Review focus: P4 (network input handling,
the state machine) and P5 (the start-up order and the two threads), as the 2026-10-24 plan said
of its step 5a.

## Security considerations (summary for the reviewer)

- Every network payload is bounded before it is parsed (P1), hashed before it is used (P4), and
  nothing reaches `db/`, `qs/` or `caches/` before the importer re-verified the whole snapshot
  against a manifest that `min_sources` independent addresses signed (P5).
- A serving node answers from a table it built itself, under token buckets, with no state per
  requester beyond a 64-entry window (P3); it serves only when synced; a bootstrapping node
  never serves and processes nothing else.
- Official nodes receive no new message; a stray one is a warning and a drop.
- Keys are unauthenticated in p2p: independence is by address, and the design says so to the
  operator; `sources=` and `--download-snapshot` are the explicit trust choices.
- The frame-size cap (P9) is the only change that could reject a legitimate packet; it is flagged
  and sized above anything the node sends today.
