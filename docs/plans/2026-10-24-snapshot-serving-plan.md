# Snapshots served by community nodes: implementation plan

For `snapshot-serving-design.md` (same folder), against master b5f74ecd (4.2.532-community.11).
Each step is one pull request, in this order; every step leaves master releasable with the feature
off or absent. File:line references are on b5f74ecd. No step is consensus-relevant: no block, hash,
signature rule or round behaviour changes; a snapshot is a checkpoint and stored blocks, which a
node has anyway.

Conventions: log prefix `SNAPSHOT: `; new files `csnode/include/csnode/snapshot_*.hpp` and
`csnode/src/snapshot_*.cpp`; unit tests in `tests/csnode/` (GoogleTest, `tests/csnode/CMakeLists.txt`
globs `*.cpp`); plain log texts, no exclamation marks; `WithDelimiters` for sequences; sizes
below are rough (small: up to about 300 lines with tests; medium: up to about 800; large: more).

Order of the backward-sync branch: step 3b (the block verifier) is shared with backward sync's
step 3; whichever lands first creates `csnode/include/csnode/blockverifier.hpp`, the other reuses
it.

## Step 0. Measure the digest families (no code, half a day)

Before any step, with the user's permission for the local node's logs (design, open question 8):

- `STATE DIGEST #186400000` and `#186500000` from the local pruned node's `log/log_*.txt`, from
  the workflow run 40069688210 logs (`gh run view --log`) and from the next scheduled run; the
  fresh container of the mainnet smoke test if it reached those blocks.
- Record the result in the design's section 7.1 (number of families seen, which wallet differs
  if any; `WalletsGet` through the API of each node at the same block gives the wallets).

Pass: a table of digests per node per block in the design; the default of `fallback` confirmed
or changed accordingly.

## Step 1. Checkpoint digest (small)

**Files.** `csnode/src/blockchain.cpp` (`saveLiveCheckpoint` 1012-1043, slow-start checkpoint
409-432, quick start 165-171), `csnode/include/csnode/blockchain.hpp`,
`csnode/include/csnode/caches_serialization_manager.hpp`, `csnode/src/caches_serialization_manager.cpp`
(`save`, `loadVersion` 392-460, file names 84-92), `csnode/include/csnode/statedigest.hpp`,
`csnode/src/node.cpp` (`logStateDigest` 4470-4481, `onSuccessQS`),
`tests/csnode/checkpoint_tests.cpp`, `tests/csnode/statedigest_tests.cpp`.

**Change.**

- `struct cs::CheckpointDigest { std::array<uint8_t, 32> digest; uint64_t wallets; }` in
  `statedigest.hpp`, with `write(path)` / `read(path)` of `state_digest.bin` (`u8 version = 1`,
  32 bytes, `u64 wallets`, little-endian).
- `CachesSerializationManager::save(version, head, const std::optional<CheckpointDigest>&)`:
  writes `state_digest.bin` into the version's directory before the hashes file, as
  `saveHead` does (`caches_serialization_manager.cpp:232-244`); `load` exposes the digest of the
  loaded version (`std::optional<CheckpointDigest> loadedDigest() const`).
- `BlockChain::saveLiveCheckpoint` and the slow-start path pass `multiWallets().stateDigest()`
  and `multiWallets().size()`; a new signal `checkpointSavedEvent(cs::Sequence)` is emitted
  after a successful periodic save (used by step 4). Log
  `STATE DIGEST #<S> <hex> wallets <n> checkpoint` from `BlockChain` (the same text as
  `Node::logStateDigest`, moved to a shared helper in `statedigest.hpp`).
- After `tryQuickStart` succeeds (`blockchain.cpp:165`): compute the digest of the loaded
  caches, log `STATE DIGEST #<S> <hex> wallets <n> checkpoint loaded`, and when the loaded
  version has a `state_digest.bin` that differs, log an error, quarantine the version
  (`quarantineVersion`, as `caches_serialization_manager.cpp:424-429`) and treat the quick start
  as failed so that the next version is tried; `BlockChain` gets
  `std::optional<CheckpointDigest> loadedCheckpointDigest() const` for step 6.
- `qs/0` (the shutdown checkpoint, `blockchain.cpp:1300-1352`) gets the file too: it costs
  nothing and `digest_offline.sh` can later read it instead of starting the node.

**Tests.** `checkpoint_tests.cpp`: a saved version has `state_digest.bin` with the digest of the
wallets at that moment; a version without the file loads; a version whose file is altered is
quarantined and the previous version loads; `statedigest_tests.cpp`: write/read round trip,
truncated file rejected. Private network: `all-storage` and `snapshot-join` unchanged
(`run.sh:669-690`, `entrypoint.sh:152-190` patterns still match: the added words follow
`wallets <n>`).

**Pass.** Unit tests green; `robustness.yml` `snapshot-join` and `all-storage` green; the local
node's next periodic checkpoint has the file and the log line (after the release that ships it).

## Step 2. `[snapshot]` configuration (small)

**Files.** `client/config/config.hpp` (next to `StorageData`, 61-71), `client/config/config.cpp`
(`readStorageData` 952, `readApiData` 1002, call sites 884-885, `operator==`),
`docs/storage.md` or a new `docs/snapshots.md`, `docs/differences.md`.

**Change.** `struct SnapshotData` with the keys and defaults of the design's section 8;
`Config::readSnapshotData` with range checks (ports 0-65535, `min_sources` 1-20,
`fetch_connections` 1-16, `snapshot_blocks` 1000-10,000,000, a warning and the default for a bad
value, as `readStorageData` does at 963 and 987); the fields in the configuration comparison so
that the observer emits `configChanged` for them; `getSnapshotSettings()`. `docs/snapshots.md`:
the keys, the format, the trust model in operator terms (what "3 sources" means), the ports to
open.

**Tests.** A config test next to the existing ones (`tests/csnode/` has `clientconfigmock.hpp`;
if `Config::readFromFile` is not unit-tested today, a small test on a temporary `config.ini`):
defaults without the section, every key, bad values clamped, two configs differing only in
`serve_port` compare unequal.

**Pass.** Tests green; a node with and without `[snapshot]` starts as before (no behaviour yet).

## Step 3a. Snapshot format 2: builder and `--export-snapshot` (medium)

**Files.** New `csnode/include/csnode/snapshot_manifest.hpp`, `csnode/src/snapshot_manifest.cpp`,
`csnode/include/csnode/snapshot_builder.hpp`, `csnode/src/snapshot_builder.cpp`;
`csdb/include/csdb/storage.hpp`/`storage.cpp` (a raw read `bool Storage::pool_bytes(cs::Sequence,
cs::Bytes&) const` under `dbLock_`'s caller, and `forEachContractData` passthrough);
`client/src/main.cpp` (the option, after `dbcheck::check` at 192); `tests/csnode/snapshot_manifest_tests.cpp`,
`tests/csnode/snapshot_builder_tests.cpp`.

**Change.**

- `snapshot_manifest`: `struct SnapshotManifest` with the fields of the design's section 2,
  `toJson()` (hand-written, numbers unquoted), `fromJson(string)` (`boost::property_tree`
  `read_json`, every field checked for shape: hex lengths, `format == 2`, name regex
  `[a-z0-9.-]{1,64}`, counts and sizes within the limits of section 6.4), `filesHash()`,
  `signedLine()`, `sign(privateKey)`, `verify(publicKey)` with `cscrypto`. SHA-256 with
  libsodium's `crypto_hash_sha256`.
- `snapshot_builder`: `SnapshotBuilder::build(const BlockChain&, const Storage&, Sequence S,
  const std::filesystem::path& qsDir, const std::filesystem::path& outDir, const SnapshotSettings&,
  const KeyPair&, Progress)` as in section 4: copy `qs/<S>`, require `state_digest.bin`, verify
  `quick_start_hashes.dat` with the manager's check, read blocks `F..S` in batches of 256 with the
  hash-chain check, lz4 frame compression (the `lz4frame` API of `third-party/lz4`, not the
  block API of `compressor.hpp`, so that files are standard `.lz4`), contract data, per-chunk
  hashes, manifest, signature, `<S>.tmp` renamed to `<S>`, older directories removed. Low
  priority: the thread calls `setpriority`/`nice 10` on Linux. A stop request aborts between
  batches and removes `<S>.tmp`.
- `--export-snapshot <dir>`: on a stopped node directory, opens the storage read-only, loads
  nothing into the caches, picks the highest periodic version in `qs/` (not `0`) that has
  `state_digest.bin`, builds into `<dir>`, prints the manifest's block and digest, exits 0; exit 1
  with the reason otherwise. Signs with the node's key files as a normal start would.
- Measure and record in the design's section 12: lz4 ratio per file on the extracted published
  snapshot (`/tmp/claude-1001/…/scratchpad/snap-x` has an extracted copy today), export time.

**Tests.** `snapshot_manifest_tests`: JSON round trip, signature verify and tamper (every field),
a name outside the regex rejected, limits. `snapshot_builder_tests`: a temporary LMDB storage with
generated blocks 0..300 (as `pool_tests.cpp` builds them) and a fake `qs/200` directory with the
checkpoint files and `state_digest.bin`: the build gives the files of section 2, `blocks.bin`
decompresses to blocks `F..200` in order with the right bytes, `contracts.bin` holds the
records, every chunk hash and file hash matches, the manifest verifies; a storage with a broken
chain builds nothing; a `qs` directory without `state_digest.bin` builds nothing; a stop
request leaves no `.tmp`. With RocksDB built: the same on RocksDB.

**Pass.** Unit tests green; `./node --export-snapshot out` on the extracted published snapshot
(after a local run that wrote `state_digest.bin` into its `qs/186300000`, or on a test-network
node) produces a manifest that verifies with the node's key; the ratio numbers recorded.

## Step 3b. Block verifier shared with backward sync (small)

**Files.** New `csnode/include/csnode/blockverifier.hpp`, `csnode/src/blockverifier.cpp`,
`tests/csnode/blockverifier_tests.cpp`. Skip if backward sync's step 3 has landed: use its class.

**Change.** `cs::BlockVerifier::verify(const csdb::Pool& block, const csdb::PoolHash& expectedHash,
std::string& reason)`: `is_valid()`, hash computed from the bytes equals `expectedHash`, for
`sequence() ≥ 1` the pool signatures verify against the block's own confidants with
`mask = bitsToMask(numberTrusted, realTrusted)` exactly as `tryFlushDeferredBlock` does
(`blockchain.cpp:1277-1285` on b5f74ecd per the backward-sync design), block 0 needs none. Round
confirmations against the previous block's confidants as a separate `verifyConfirmations(block,
previousConfidants)` for callers that have them.

**Tests.** Generated signed blocks: valid; altered transaction (hash differs); altered signature;
missing signature; wrong sequence; genesis.

**Pass.** Tests green; the same source file works on both branches.

## Step 3c. Importer and `--import-snapshot` (medium)

**Files.** New `csnode/include/csnode/snapshot_importer.hpp`, `csnode/src/snapshot_importer.cpp`;
`csdb` (open a fresh storage of a backend on an empty `db/` and `put` blocks: reuse what
`storage_convert.cpp` does to open the target backend, and `Database::put`, `updateContractData`);
`csnode/src/blockhashes.cpp` (`update` usable before `BlockChain` exists: a static helper that
opens `caches/hashdb` and `caches/seqdb` and inserts `(seq, hash)`); `client/src/main.cpp`;
`tests/csnode/snapshot_importer_tests.cpp`.

**Change.** `SnapshotImporter::import(const std::filesystem::path& downloadDir, const SnapshotManifest&,
const std::string& dbPath, const std::string& backend, Progress)` as in the design's section 6.4:
the fixed name table (`blocks.bin.lz4`, `contracts.bin.lz4`, and `qs-<name>.lz4` for the twelve
names), refusal of anything else; decompression with a size cap (`raw_size` from the manifest,
at most 4 GB per file); `qs/<S>/` written from `qs-*`; `head.bin` must carry `S` and the
manifest's `head_hash`, `state_digest.bin` its digest, `quick_start_hashes.dat` must check;
blocks ascending with the verifier (`previous_hash` chain, the hash of `S` equals `head_hash`,
signatures), written with `put(hash, seq, bytes)` on the configured backend (lmdb or rocksdb;
berkeleydb refused with the conversion's reason), `hashdb`/`seqdb` updated per block,
`caches/indexdb` left absent, contract data written, `flush()`, `db/.backend` written. Any
failure removes `db/`, `qs/<S>` and `caches/` created by the import and returns the reason and
the file at fault (so the fetcher can mark sources). `--import-snapshot <manifest file>` runs
it on a local directory (the CI and the test network use it in place of `snapshot-restore.sh`)
and exits 0 or 2 with the reason, as the script does.

**Tests.** Round trip with step 3a's builder on LMDB and RocksDB: import into an empty directory,
then `BlockChain::init` on it quick-starts from `qs/<S>` (the `blockchain_tests.cpp` style with a
mock node, or a process-level test in `run.sh`), `first_sequence()` is `F`, `pool_load(S)` is the
exported block, contract records present; tampered inputs: a block with an altered signature
(rejected, nothing left behind), a `head.bin` with another hash, a `state_digest.bin` with another
digest, a file name outside the table, a `raw_size` above the cap, a truncated `.lz4`.

**Pass.** Tests green; `run.sh` `snapshot-join` switched to `--export-snapshot` and
`--import-snapshot` (format 2) and green, including `snapshot-corrupt` with a truncated file (exit 2,
sync from 0).

## Step 4. HTTP server (medium)

**Files.** New `csnode/include/csnode/snapshot_server.hpp`, `csnode/src/snapshot_server.cpp`;
`csnode/src/node.cpp` (owned next to `api_`; started in `Node::init` after `blockChain_.init`,
before `api_->run()` at 258; stopped in `Node::destroy` 355-358); `csnode/src/blockchain.cpp`
(connect `checkpointSavedEvent` to the builder thread); `csnode/CMakeLists.txt` (Beast is
header-only; check the CI Boost install has `boost/beast` since `build.yml:56-68` builds with
`--with-libraries`; `b2 install` copies all headers, verify once);
`tests/csnode/snapshot_server_tests.cpp`.

**Change.** `SnapshotServer` with a control thread that reads `getSnapshotSettings()` every 10 s
(`csconnector.cpp:99-150` pattern): starts an `asio::io_context` with 2 threads and an acceptor
on `serve_bind:serve_port` when the port is above 0 and no server runs; stops it when the port
is 0 or changed (then starts on the new one); updates the limits in place. Builder: on
`checkpointSavedEvent(S)` and at server start (for the highest periodic version without a built
snapshot), a builder thread runs step 3a's `build` into `snapshot/`; the server loads the manifest
of the newest complete `snapshot/<S>` and serves it. Routes, limits, timeouts, `Range`
handling, `503`, `416`, `404`, `405`, the token bucket, logging as in the design's section 5.
`Node::stop` requests the builder to abort.

**Tests.** `snapshot_server_tests`: the server on an ephemeral port over a temporary snapshot
directory, a Beast client in the test: manifest; whole file equals the file; ranges (first, last,
middle, beyond the end `416`, two ranges `416`, 16 MiB + 1 `416`); `HEAD`; unknown path `404`;
`POST` `405`; a 9 KB header closed; `serve_max_connections=1` and a second connection gets
`503`; a 200 KB/s rate on a 1 MB file takes about 5 s; an idle connection closed after the
timeout; port change at run time (settings object swapped, server moves). The dynamic start is
tested in the private network (step 7).

**Pass.** Tests green; on the test network a node with `serve_port` set serves `curl
http://…/snapshot/latest.json` and `curl -r 0-1048575` of a file.

**Only synced nodes serve (user's rule, 2026-10-24).** Any node may serve, whatever its stake, but
only while it is synced with the network and only a snapshot it has finished building: the routes
answer `503 Retry-After` while the node's last block is more than a few rounds behind the current
round (the `wait-head` rule of `ci/snapshot/check_node.py`: within 3 rounds) or while no snapshot is
built; `latest.json` carries the node's last block at the time of the request (`synced_to`, outside
the signed line, informative), and the fetcher (5a) drops sources whose `synced_to` is far behind
the others'. Tests: a node in slow start answers 503; it serves once at the head.

## Step 5a. Fetcher: sources, agreement, download (large)

**Files.** New `csnode/include/csnode/snapshot_fetcher.hpp`, `csnode/src/snapshot_fetcher.cpp`,
`csnode/include/csnode/snapshot_agreement.hpp`, `csnode/src/snapshot_agreement.cpp`;
`lib/include/lib/system/process.hpp` use for `curl`; `tests/csnode/snapshot_agreement_tests.cpp`,
`tests/csnode/snapshot_fetcher_tests.cpp`.

**Change.**

- `snapshot_agreement`: pure functions over `std::vector<SourceManifest>` (`{source id, key,
  address, manifest}`): collapse by key and by /24 or /48, group by `(seq, head_hash)` and by
  digest, choose per the design's section 6.3, return the chosen group, every group for logging,
  and the reason when nothing is chosen.
- `snapshot_fetcher`: source discovery (config list, `sources_file`, `known_hosts.txt` at the
  default port, GitHub through `curl` if found on the `PATH`; a `cs::Process` with a 30 s
  timeout, output to a file in `snapshot-tmp/`, never to memory beyond 64 KB for a manifest);
  manifest fetch in parallel with the Beast client (connect 5 s, total 10 s, 64 KB), signature
  checks against the expected key; the agreement call; the chunked download of section 6.4 with
  `fetch_connections` workers, per-file source groups by `sha256`, per-chunk verification,
  source marking, `fetch_rate_kbps` token bucket, 30 s per chunk, resumable within a run (a
  chunk already verified on disk is skipped; across runs `snapshot-tmp/` is discarded, which
  keeps the logic simple); progress log every 10 % and every 30 s; then step 3c's importer.
  Returns the result (restored block, sources used, or the reason).

**Tests.** `snapshot_agreement_tests`: threshold, same-/24 collapsing, GitHub tie-break, highest
block first, two blocks where only the lower reaches the threshold, `fallback=none` with nothing
chosen, `min_sources=1`. `snapshot_fetcher_tests`: three in-process servers (step 4) with
snapshots built by step 3a from the same generated chain (same digest; `qs-*` bytes identical
or not: one server serves a `walletscache.dat` with an extra trailing byte so that its `sha256`
differs and its file group is separate), a fourth with a different digest: the fetcher picks the
three, downloads each file from a source of its `sha256` group, imports, the directory is a
valid node directory; a server that corrupts one chunk (test hook): the chunk is refetched from
another source of the group, the server marked; a server that stalls: timeout, the rest
completes; a manifest with a bad signature is dropped; `curl` absent: GitHub skipped with a log
line (test by an empty `PATH`).

**Pass.** Unit tests green with 4 servers in process; a manual run of `--import-snapshot
http://host:9511/snapshot/latest.json` against a test-network node restores a directory the node
starts from.

## Step 5b. Fetch at start, verification, Docker (medium)

**Files.** `client/src/main.cpp` (before `dbcheck::check` at 192), `csnode/src/node.cpp`
(verification after the quick start, `snapshot.verified` / `snapshot.mismatch`, exit code 4),
`packaging/docker/entrypoint.sh` (write `[snapshot]` from `CREDITS_SERVE_SNAPSHOT`,
`CREDITS_SNAPSHOT`, `CREDITS_SNAPSHOT_URL`, `CREDITS_SNAPSHOT_MIN_SOURCES`; remove the restore
call 128-139 and the digest wait 152-190 for format 2; keep `snapshot.mismatch` handling at 117),
`packaging/docker/docker-compose.yml` (publish `9511/tcp` when serving), `packaging/docker/.env.example`,
`packaging/docker/README.md`, `packaging/docker/test/test-entrypoint.sh`, `packaging/mainnet/config.ini`
(a commented `[snapshot]` section), `docs/snapshots.md`, `docs/differences.md`, `docs/CHANGES.md`.

**Change.** The start-up hook of the design's section 6.1 (conditions: `fetch=auto`, no
database, no checkpoint, lmdb or rocksdb; `snapshot-tmp/` removed first); the fetcher's result
logged; on failure the node continues with a sync from block 0 and the log line names every
group seen. After `tryQuickStart`, when `snapshot.json` exists and `snapshot.verified` does not:
the loaded checkpoint's digest (step 1) against the manifest's; `snapshot.verified` or
`snapshot.mismatch` and a stop with exit 4; `snapshot.mismatch` present at start refuses with the
same message. The entrypoint maps the variables and no longer restores by script when the
configuration has `[snapshot]`; `snapshot-restore.sh` stays one release.

**Tests.** `test-entrypoint.sh`: the `[snapshot]` section written from each variable; a volume with
`snapshot.mismatch` refused. Private network: `snapshot-join` restores through the node's own fetch
from the pruned node's server with `min_sources=1` (the three-source scenario is step 7).
Docker mainnet smoke test with `CREDITS_SNAPSHOT_MIN_SOURCES=1` against GitHub format 2 (needs
step 8 first; until then the smoke test stays on format 1).

**Pass.** A fresh test-network node with `fetch=auto` and one source restores, logs the agreement,
verifies at the quick start and reaches the target; a node with `fetch=off` syncs from 0 as today;
`docker.yml` green.

## Step 6. Discovery in the weekly job (small)

**Files.** New `ci/known-hosts/probe_snapshots.py`; `.github/workflows/known-hosts.yml`
(after `collect.py`, in the read-only job; the publish job uploads the two new files and adds
`packaging/mainnet/snapshot_sources.txt` to the pull request), `ci/known-hosts/run-weekly.sh`
(same), `packaging/make-package.sh:28-29` (ship `snapshot_sources.txt` when present),
`packaging/docker/Dockerfile` and `entrypoint.sh` (copy it next to `known_hosts.txt`).

**Change.** `probe_snapshots.py <known_hosts.txt> <out dir> [--port 9511]`: for each line, GET
the manifest with a 5 s timeout and a 64 KB cap (`urllib`, plain HTTP), parse, verify
`signature` over `signed` with PyNaCl and the line's key, check `node_key` equals it; write
`snapshot_sources.txt` (`ip:port key`, at most 3 per /24, same helper as `collect.py`) and
`snapshots.md` (a table of `(seq, head_hash, state_digest)` groups with their sources and
`created`). The workflow's node runs with `fetch=off` in its `config.ini`.

**Tests.** A pytest-free self-test as `collect.py` has none: a `--self-test` that runs against a
local `python3 -m http.server` directory with one valid and one tampered manifest, used in the
workflow's `Collect` step. Review of the first `snapshots.md` by hand.

**Pass.** The workflow publishes `snapshot_sources.txt` and `snapshots.md`; the pull request
carries both lists; the packages ship the file.

## Step 7. Robustness scenarios (medium, `run.sh` and `robustness.yml`)

**Files.** `ci/testnet/run.sh` (`SERVE_NODES=4,6,7`, `JOIN_FETCH=1`, `JOIN_MIN_SOURCES`,
`JOIN_FALLBACK`, `FORGE_NODE`, `SERVE_DYNAMIC_AT`, `SERVE_LIMITS`, `JOIN_KILL_DURING_FETCH`),
`.github/workflows/robustness.yml` (the rows of the design's section 13), a test-build hook in
`snapshot_builder.cpp` under `CREDITS_TESTNET` (`csnode/CMakeLists.txt` already defines it) to
alter one balance of the copied `walletscache.dat` before hashing, and one signature in the
block stream, when an environment variable names the node.

**Change.** Scenarios `snapshot-serve-join`, `-few`, `-forged`, `-dynamic`, `-limits`, `-dropped`,
`-backend` as specified; `snapshot-join` and `snapshot-corrupt` on format 2 (step 3c, 5b). The
`-dynamic` scenario edits `config.ini` of a running node with `sed` and waits up to the observer
period (the test network can set a shorter `observer_wait_time` if the configuration has such a
key; otherwise the scenario waits 6 minutes, acceptable once).

**Pass.** Every scenario green twice in a row on the branch; `max-parallel` unchanged.

## Step 8. GitHub workflow on format 2 (medium)

**Files.** `.github/workflows/snapshot.yml` (after "Stop and check the logs": `./node
--export-snapshot out2/`; the assets of one release share a namespace, so each file is published
as `s<seq>-<name>` and the manifest as `snapshot.json`; the manifest's `name` stays the plain
name and the client composes `s<seq>-<name>` for the GitHub source only; keep the 3 newest
sequences; skip when the exported sequence equals the one the run started from),
`ci/snapshot/check_node.py` (unchanged), `snapshot_fetcher.cpp` (the GitHub asset naming),
`docs/specs/2026-10-16-docker-and-pruned-snapshots-design.md` (a note on format 2),
`packaging/docker/test/test-snapshot.sh`.

**Change.** Both formats published for one release; the publish job's check extended to the
format 2 manifest (shape, every file's size and hash); the `pruned-snapshot` release notes name
both. The step after one release: remove format 1 (`snapshot-make.sh`, `snapshot-restore.sh`,
the entrypoint's legacy path, `digest_offline.sh` replaced by reading `qs/0/state_digest.bin`),
its own small PR.

**Pass.** A manual run publishes `snapshot.json` and files; a fresh container with
`CREDITS_SNAPSHOT_MIN_SOURCES=1` restores from GitHub format 2 and verifies; the next scheduled
run skips when no checkpoint boundary was crossed and publishes when one was.

## Step 9. Community testnet trial, then mainnet (user's rule, 2026-10-24)

Nothing of this feature reaches mainnet before it has run on a testnet: the official testnet has
no nodes (monitor: 0 online), so the trial runs on a persistent private network of our own.

- **Testnet.** Private networks on GitHub Actions, as the scale runs (`scale.yml`, up to 51 nodes
  on one runner): about 50 nodes, 7 to 9 trusted per round, nodes below the minimum stake, some
  even nodes on the official binary (`node_alt_ref`), checkpoints every 1,000 to 10,000 blocks. A
  job lasts at most 6 hours, so "days" means repeated runs of a `snapshot-testnet` scenario, not
  one network kept alive (Actions is not for permanent services, and this repository's Actions was
  disabled once for heavy use: keep the runs to what each step needs).
- **What runs there, step by step as it is merged:** checkpoint digests (1), format-2 export and
  import (3a-3c), serving with limits switched at run time (4), fresh nodes joining from the
  network only (5a-5b, GitHub source off and on), a forged source and a dropped source (7), a
  full node starting from a network snapshot. Pass: no node stops or forks, every join agrees on
  the digest, the official nodes see no difference.
- **Only then mainnet:** the local mainnet node serves first, alone, with a low rate; the
  release that turns serving on by default in the packages comes after the testnet runs of every
  step pass and a few days without problems on the local mainnet node.

## Sizes and order

| Step | Size | Depends on |
| --- | --- | --- |
| 0 measurement | half a day | user's permission |
| 1 checkpoint digest | small | — |
| 2 configuration | small | — |
| 3a builder, export | medium | 1 |
| 3b block verifier | small | — (shared with backward sync) |
| 3c importer, import | medium | 3a, 3b |
| 4 HTTP server | medium | 2, 3a |
| 5a fetcher | large | 3c, 4 |
| 5b start-up, Docker | medium | 5a, 1 |
| 6 discovery job | small | 4 (a node to probe), 5b (the file is read) |
| 7 robustness scenarios | medium | 5b |
| 8 GitHub format 2 | medium | 3a, 5a |
| 9 mainnet trial, release | small | all |

Steps 1, 2 and 3b can go in parallel; 3a and 4 after 1 and 2; 5a is the longest and the one to
review most closely (network input handling); 6 and 8 are Python and YAML and can be done by a
second worker once 4 and 3a exist.
