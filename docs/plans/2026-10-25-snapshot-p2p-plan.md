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

## Step P5. Bootstrap mode of `Node`, start-up, scenarios (medium-large; the delicate one)

**Files.** `csnode/include/csnode/node.hpp`, `csnode/src/node.cpp` (`init` 141-293 split;
constructor 102-121; `onNeighbourAdded` 384-392; `onPingChecked` 2134-2145; `chooseMessageAction`
2169-2172; `stop` 340-352; `destroy` 354-368), `csnode/src/blockchain.cpp:1350` (`close()` guard),
`client/src/main.cpp` (nothing: the hook is inside `Node`; the `--import-snapshot` path at 203-206
stays), `packaging/docker/entrypoint.sh` (only the `snapshot.mismatch` and digest-wait parts, the
rest in P6), `ci/testnet/run.sh`, `.github/workflows/robustness.yml`, `docs/snapshots.md`,
`docs/differences.md`, `docs/CHANGES.md`; a test-build hook (`CREDITS_TESTNET`) for the forged
scenario in `snapshot_builder.cpp` (alter one balance in the copied `walletscache.dat` before
hashing when `./testnet-forge-snapshot` exists) and in `SnapshotProvider` (flip one byte of a
piece when `./testnet-corrupt-pieces` exists).

**Change.**

- `Node::init` → `initCommon()` (142-166), `initChain()` (168-255, both branches), `initRunning()`
  (257-292); the five connections at 276-281 move to the constructor after 117. `init()` runs
  `initCommon()`, then if `needsSnapshot()` (design 6.2) constructs `fetcher_` with the hooks
  (`sendDirect` of the two requests; `addNeighbours` = `transport_->addToNeighbours` +
  `setPermanentNeighbours(keys ∪ initialConfidants_)`; `addressOf` from `getKnownPeers`), starts
  `fetchTimer_` (500 ms, `CallQueuePolicy`) whose slot builds `Peers` (`getKnownPeers` once per
  second, `forEachNeighbour`) and calls `tick`, sets `bootstrapping_ = true`, logs
  `SNAPSHOT: empty database: looking for snapshots on the network (fetch=auto, min_sources=N)`,
  and returns `true`; otherwise `initChain()` and `initRunning()` as today.
- Gates: `chooseMessageAction` after 2170-2172: `if (bootstrapping_) return (type ==
  SnapshotManifestReply || type == SnapshotPieceReply) ? Process : Drop;`; `onNeighbourAdded`
  and `onPingChecked`: `if (bootstrapping_) return;`.
- Hand-over: when `fetcher_->phase() == Import`, the tick starts `bootstrapThread_`: write the
  `sources` into the manifest copy for `snapshot.json`, `importSnapshot(...)`; on failure
  `fetcher_->onImportFailed(file)` queued back to the processor thread; on success
  `initChain()` on the worker (if it fails: log, `Node::requestStop()`, exit 1 as a failed init
  does today through `Peer`), then `CallsQueue::instance().insert([this] { initRunning();
  verifySnapshotAtStart(); bootstrapping_ = false; fetchTimer_.stop(); fetcher_.reset();
  transport_->setPermanentNeighbours(initialConfidants_ if I am one, else {}); log SNAPSHOT:
  started from block S fetched from N sources in T s })`.
- Fallback: `phase() == Fallback`: on the processor thread, log the 6.4 line, then the same
  hand-over without an import (`initChain()` writes the genesis and the node syncs from 0) — on
  the worker thread as well, so that the processor thread is never blocked for minutes.
- `verifySnapshotAtStart()` (design 6.8): `snapshot.json` present and `snapshot.verified` absent:
  compare `blockChain_.checkpointDigest()` (`blockchain.cpp:124`) with the manifest's; write
  `snapshot.verified`, or `snapshot.mismatch` + `SNAPSHOT DIGEST MISMATCH …` + stop with exit
  code 4 (a new exit path through `Peer`: `Node::exitCode()` read by `Peer::onRun`,
  `peer.cpp:130-147`); `snapshot.mismatch` present at `init` → refuse with the same code. This
  also covers directories restored by `--import-snapshot` and `--download-snapshot`.
- `Node::stop`: `if (fetcher_) fetcher_->stop(); if (bootstrapThread_.joinable()) join;` before
  `poolSynchronizer_->stop()`. `BlockChain::close()`: `if (!storage_.isOpen()) { stop_ = true;
  return; }` first (and the same guard where `destroy()` touches the chain). `snapshot-tmp/`
  removed in `needsSnapshot()` before the fetch and after a fallback.
- `run.sh`: `SNAPSHOT_SERVE_NODES=4,6,7` (each gets `serve=on`, `serve_rate_kbps=0`,
  `checkpoint_every=1000`, `checkpoint_keep=3`); `JOIN_FETCH=p2p` (needs `JOIN_AT`): the joining
  node `NODES + 1` is started at sequence `JOIN_AT` with an empty directory, `fetch=auto`,
  `min_sources=$JOIN_MIN_SOURCES` (3), `fetch_wait=$JOIN_WAIT` (300), `fetch_fallback=$JOIN_FALLBACK`
  (sync), the network's `hosts.txt`, API open; `JOIN_KILL_DURING_FETCH=1` kills it once on the
  first `SNAPSHOT: fetching block` line and once on `SNAPSHOT: importing`; `FORGE_NODE=7` and
  `CORRUPT_NODE=7` create the test-build hook files in that node's directory. End checks: the
  joining node's log has `SNAPSHOT: block S hash … : N sources`, `started from block S fetched
  from`, `Caches for QUICK START loaded successfully`, `snapshot.verified` exists, the digest at
  `S` or `S+1` equals the manifest's (the check of `run.sh:1163-1170` reused), the serving nodes
  logged `served block S to`; for `-few`: the waiting line, the manual-import line, `SLOW START`,
  the target reached; `-dropped`: `snapshot-tmp` absent after the restart and the join completes;
  `-forged`: `minority` logged for n7's digest, `rejected piece from` for n7, completion;
  `-mixed`: no `Unknown message type` in the even nodes' logs and no request sent to them (the
  joining node logs `asking <key>` lines; none is an even node's key).
- `robustness.yml` rows: `snapshot-p2p-join`, `-few`, `-wait` (the serving node switched on by a
  `sed` on its `config.ini` after 90 s; the observer period is 5 min: the scenario waits up to
  7 min once, acceptable, or the test network sets a shorter observer period if the config has
  one), `-dropped`, `-forged`, `-mixed` (`node_alt_ref` = the last master before P1).
- `docs/snapshots.md`: the operator view: what `fetch=auto` does, the waiting line, the manual
  way (`--download-snapshot`, `--import-snapshot`), `serve=on`, the keys, what the network sees.

**Tests.** Unit: `needsSnapshot()` on temporary directories (empty, lmdb present, `qs/0` only,
`qs/1000` present, berkeleydb backend, `fetch=off`); the gates (a `Node` cannot be constructed in
a unit test: the gate logic is a free function `snapshotBootstrapAction(type, bootstrapping)`
tested alone). The rest is the private network.

**Pass.** Unit tests green; the six scenarios green twice in a row; `testnet.yml`, `all-storage`,
`snapshot-join*`, `storage-switch` unchanged (a node with a database never enters bootstrap mode);
the Docker smoke test unchanged (format 1 restore still in place until P6).

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
| P5 | bootstrap mode, hand-over, verification, 6 scenarios | M-L | P3, P4 | — |
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
