# Docker image and pruned-node snapshots: implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task by task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** a pruned community node that starts near the chain head from `docker compose up`, using snapshots published every 12 hours by a GitHub workflow.

**Architecture:**
- A Docker image built from the release binaries keeps all state in one volume (`/data`, host `~/.credits`).
- `entrypoint.sh` does the first-start setup: configuration, keys and snapshot restore.
- Shell scripts in `packaging/docker/` package and restore snapshots. The image, the workflow and the test network all use the same scripts.

**Tech stack:** Bash, Python 3 (PyNaCl, base58, thriftpy2 for the API check), Docker, GitHub Actions, `mdb_copy` built from `third-party/lmdb`, zstd.

**Spec:** `docs/specs/2026-10-16-docker-and-pruned-snapshots-design.md`

## Global constraints

- No node C++ changes. The node reads its keys through `--public-key-file` and `--private-key-file`.
- The image runs as uid 1000 (`credits`). Everything persistent lives under `/data`.
- Private keys never pass through environment variables, logs or snapshots.
- A snapshot holds:
  - `db/lmdb` (compacted with `mdb_copy -c`);
  - `caches/`, `qs/`, `roundstat.dat`, `smartcontracts.dat`;
  - nothing else.
- Snapshot release: tag `pruned-snapshot`. Assets: `latest.json` plus at most 3 archives `credits-pruned-<seq>.tar.zst`.
- `snapshot.yml` runs every 12 hours at jittered minutes, one runner, a 90-minute catch-up limit. It never publishes after a failed check.
- Matrix workflows keep `max-parallel` at 5 or lower (Actions was disabled once for heavy use).
- All GitHub text is in English. Commits end with the Co-Authored-By line.

## Storage changes (added at the user's request)

The entrypoint records the storage in use in `/data/.storage` after a successful start, as `mode=<pruned|full> backend=<…>`. On every start it compares that record with what `config.ini` asks for:

| Change | What happens |
| --- | --- |
| None | Start. |
| Full to pruned, same backend | Start. The node prunes old blocks at its next checkpoints. A log line says so. |
| Pruned to full, same backend | Refuse to start: the pruned blocks are gone, so the node can never be full. The message says to set `CREDITS_RESET=1` for a fresh sync. |
| Any backend change | Refuse to start: checkpoints and caches belong to the old database. Same message. |
| `CREDITS_RESET=1` | Move `db/ caches/ qs/ *.dat` into `/data/old-<date>/` (never deleted), then first-start rules: a restore for pruned with `auto`, otherwise a sync from block 0. |

- `.env` values that differ from `config.ini` after the first start produce a warning naming both. `config.ini` stays the source of truth.

## Review focus

1. **A volume left half-written by an interrupted snapshot download or extraction.** The next start must not run on partial data. Restore into `/data/.restore-tmp`, move into place only at the end, and wipe `.restore-tmp` on start.
2. **A non-empty volume from an older container.** The entrypoint must never overwrite `config.ini`, the keys or `db/`. Restore only when `db/` does not exist or is empty.
3. **No network or no release.** The `latest.json` download fails or the checksum mismatches: log it, sync from block 0, never exit in a loop.
4. **SIGTERM during a restore.** No node runs yet, so the entrypoint must exit at once and leave `.restore-tmp` to be wiped next time.
5. **The digest check never sees a `STATE DIGEST` line** (old logs, or the node fails early). The entrypoint must time out after 10 minutes with a warning, not hang, and not kill a healthy node.

---

### Task 1: Snapshot packaging and restore scripts

**Files:**
- Create:
  - `packaging/docker/snapshot-make.sh`
  - `packaging/docker/snapshot-restore.sh`
  - `packaging/docker/test/test-snapshot.sh`

**Interfaces:**
- `snapshot-make.sh <node dir> <out dir> <mdb_copy> [run url]` (`<node dir>` is a stopped node's working directory). It writes `<out>/credits-pruned-<seq>.tar.zst` and `<out>/latest.json`. `seq` and `state_digest` come from the last `STATE DIGEST #<seq> <hex> wallets <n>` line of that node's logs, and `head_hash` from `qs/<seq>/head.bin` when present. It exits non-zero if `db/lmdb` or `qs/` is missing or no digest line is found.
- `snapshot-restore.sh <data dir> <manifest url or file>`:
  - downloads into `<data>/.restore-tmp`, checks the SHA-256 and extracts;
  - moves `db/`, `caches/`, `qs/` and the `.dat` files into `<data>`;
  - writes `<data>/snapshot.json` (a copy of the manifest);
  - exit 0 means restored; exit 2 means not restored, with the reason on stderr.

- [ ] Write `test-snapshot.sh`. It builds a fake node dir with:
  - an LMDB environment made by `mdb_load` from the submodule tools;
  - fake `caches/`, `qs/1000/` and `.dat` files;
  - a log with a `STATE DIGEST #1000 abcd wallets 5` line.

  It runs make, then restore into an empty dir, and diffs the trees. Negative cases:
  - a corrupted archive must exit 2 and leave no `db/`;
  - a missing manifest must exit 2.
- [ ] Run it and see it fail (scripts missing).
- [ ] Implement both scripts:
  - `set -euo pipefail` and `zstd -T0 -19`;
  - the restore reads the manifest with `python3 -c 'import json…'` (no jq dependency);
  - `curl -fL --retry 3`.
- [ ] Build `mdb_copy`/`mdb_load` from `third-party/lmdb/libraries/liblmdb` (`make mdb_copy mdb_load`) and run the test until it passes.
- [ ] Commit: `packaging: make and restore pruned-node snapshots`.

### Task 2: Docker image, entrypoint and compose

**Files:**
- Create:
  - `packaging/docker/Dockerfile`
  - `packaging/docker/entrypoint.sh`
  - `packaging/docker/keygen.py`
  - `packaging/docker/docker-compose.yml`
  - `packaging/docker/.env.example`
  - `packaging/docker/README.md`
  - `packaging/docker/test/test-entrypoint.sh`

**Interfaces:**
- **Dockerfile:**
  - build context = an unpacked release package dir (`csnode/`) plus `packaging/docker/`;
  - installs into `/opt/credits`;
  - `ENTRYPOINT ["/opt/credits/entrypoint.sh"]`, `STOPSIGNAL SIGTERM`, `VOLUME /data`, `USER credits`.
- **entrypoint.sh:**
  - reads `CREDITS_STORAGE`, `CREDITS_DB_BACKEND`, `CREDITS_SNAPSHOT`, `CREDITS_SNAPSHOT_URL`, `CREDITS_P2P_PORT`, `CREDITS_API`, `CREDITS_EXECUTOR`, with the spec's defaults;
  - uses `/opt/credits/config.ini.template` (the package's `config.ini`).
  - Steps:
    1. wipe `.restore-tmp`;
    2. write `config.ini` if absent: executor paths absolute under `/opt/credits`, `executor_multi_instance=true`, `[storage]` for pruned, API port 0 unless on;
    3. copy `settings.properties`, `known_hosts.txt` and `default_confidants.txt` into `/data` if absent;
    4. run `keygen.py /data/keys` if the keys are absent (mode 600);
    5. restore the snapshot when pruned, `auto` and `db/` is empty;
    6. delete `log/executor-log-*.txt` older than 7 days;
    7. `exec` the node with `--db-path db --config-file config.ini --public-key-file keys/NodePublic.txt --private-key-file keys/NodePrivate.txt`, after starting a background digest watcher.
- **Digest watcher:** compares the first `STATE DIGEST #<seq>` at or above the snapshot's `seq` with `snapshot.json`.
  - On a mismatch it logs `SNAPSHOT DIGEST MISMATCH` and sends SIGTERM to PID 1.
  - It gives up after 10 minutes with a warning.

- [ ] Write `test-entrypoint.sh`. It builds the image with a stub `node`, a shell script that prints its arguments, writes a `STATE DIGEST` line to `log/` and sleeps, and with a stub executor. Checks:
  1. First start: `config.ini`, `settings.properties` and keys are created, and the args contain the key paths.
  2. Second start: the keys and a hand-edited `config.ini` line are unchanged.
  3. `CREDITS_SNAPSHOT=auto` with a local manifest from the Task 1 fixtures: restored, and the digest matches.
  4. The same with a wrong digest in `snapshot.json`: the container stops with `SNAPSHOT DIGEST MISMATCH`.
  5. An unreachable snapshot URL: the node starts anyway, with the reason in the log.
  6. A leftover `.restore-tmp`: wiped.
- [ ] Add storage-change cases to `test-entrypoint.sh`:
  - **full to pruned** (same backend): starts, with the note;
  - **pruned to full:** refused, exit code 3 and the message;
  - **lmdb to rocksdb:** refused;
  - **`CREDITS_RESET=1`:** old data moved to `old-<date>/` and a fresh setup;
  - **`.env` differing from `config.ini`:** a warning.
- [ ] Run it and see it fail.
- [ ] Implement the Dockerfile, `entrypoint.sh` and `keygen.py` (adapted from `ci/testnet/gen_keys.py`), until the test passes. Docker builds run with `nice -n 19`.
- [ ] Write `docker-compose.yml`, `.env.example` and the README (install in 3 commands, backup of `~/.credits/keys`, upgrades).
- [ ] Commit: `packaging: Docker image with a /data volume, first-start setup and snapshot restore`.

### Task 3: Image build in the release and a mainnet smoke test

**Files:**
- Modify: `.github/workflows/release.yml`, a new job `image` after `Packages`.
- Create: `.github/workflows/docker.yml` (manual and `docker-test-*` tags).

**Interfaces:**
- The `image` job:
  - unpacks the pruned package artifact;
  - builds with `packaging/docker/Dockerfile`;
  - pushes `ghcr.io/enzinobb/credits-node:<version>` and `:latest` with `GITHUB_TOKEN`;
  - needs `permissions: packages: write` for that job only.
- `docker.yml`:
  - builds the image from the latest release package;
  - runs `test-entrypoint.sh`;
  - starts the real image against mainnet with `CREDITS_SNAPSHOT=off` for 5 minutes, and requires the last written block to advance;
  - stops it, starts it again and requires a quick start with the same public key.

- [ ] Write `docker.yml` and run it with a `docker-test-1` tag. It must pass.
- [ ] Add the `image` job to `release.yml`. Check it by dispatching the release workflow with `publish=false` and an image push to a `test-` tag, then delete that tag's image only if the user agrees.
- [ ] Commit: `ci: build and publish the Docker image with each release; mainnet smoke test`.

### Task 4: Snapshot scenario on the private network

**Files:**
- Modify:
  - `ci/testnet/run.sh`: variables `SNAPSHOT_AT` and `SNAPSHOT_CORRUPT`;
  - `.github/workflows/robustness.yml`: scenarios `snapshot-join` and `snapshot-corrupt`; the build job also builds `mdb_copy`.

**Interfaces:**
- With `SNAPSHOT_AT=<seq>` (the snapshot node is `PRUNE_NODE`):
  - when the minimum sequence reaches `<seq>`, stop that node, run `snapshot-make.sh`, and start it again;
  - then start node `NODES+1`, which is outside the trusted set, with `snapshot-restore.sh` from a `file://` manifest;
  - the joining node counts in the final digest comparison.
- With `SNAPSHOT_CORRUPT=1`, the archive is truncated before the restore. The restore must exit 2 and the node must sync from block 0.

- [ ] Add the variables and the logic, reusing the `JOIN_PRUNED_AT` join-node plumbing (`JOIN_NODE`, `ALL_NODES`).
- [ ] Add the two scenarios: `DB_BACKEND=lmdb PRUNE_NODE=6 SNAPSHOT_AT=1500 TARGET_SEQ=2300`, and the same with `SNAPSHOT_CORRUPT=1`.
- [ ] Add `STORAGE_SWITCH_AT=<seq>`. At that sequence, node 5 (full LMDB) is restarted as pruned (`prune_keep_blocks=300`, checkpoints every 1,000), and node 6 (pruned LMDB) is restarted as full (`prune_keep_blocks=0`). Both must keep the network's digests. Node 5 must log a pruning step after the switch. Node 6's oldest stored block must stay where it was, so the old blocks never come back. Scenario `storage-switch`: `DB_BACKEND=lmdb STORAGE_SWITCH_AT=1200 TARGET_SEQ=2600`.
- [ ] Run with `gh workflow run robustness.yml -f only=snapshot-join,snapshot-corrupt,storage-switch`. All must pass with matching digests.
- [ ] Commit: `ci(robustness): a node joining from a snapshot, and from a corrupted one`.

### Task 5: Snapshot publishing workflow

**Files:**
- Create:
  - `.github/workflows/snapshot.yml`
  - `ci/snapshot/check_balances.py`
  - `ci/snapshot/wait_head.sh`

**Interfaces:**
- `wait_head.sh <node dir> <timeout min>`: exit 0 when the `SYNC: Blocks remaining` value has stayed below 100 for 3 minutes and the last written block is advancing; exit 1 on timeout.
- `check_balances.py --api 127.0.0.1:9090 --count 50`:
  - lists wallets through the node's Thrift API `WalletsGet`;
  - keeps those whose last transaction is older than 24 hours;
  - compares `WalletBalanceGet` with `monitor.credits.com` `ExtendedBalance`;
  - exits 1 on any mismatch, or if the monitor cannot be reached.

  The monitor's exact request format is found and written down in the script's docstring before coding, from the monitor site and the block 176,776,210 notes.
- `snapshot.yml`:
  - cron at minute 37 every 12 hours, plus `workflow_dispatch`;
  - `concurrency: snapshot` (one run at a time);
  - `permissions: contents: write`.
  - Steps:
    1. download the latest pruned package and `pruned-snapshot/latest.json` plus its archive;
    2. restore and start with the API on;
    3. `wait_head.sh` with a 90-minute limit;
    4. `check_balances.py`, then SIGTERM and wait;
    5. grep the log for the four error patterns of the spec;
    6. `snapshot-make.sh`;
    7. upload the archive and `latest.json` (`--clobber` for `latest.json`) and delete all but the 3 newest archives.

- [ ] Write `check_balances.py` and test it against `credits-pruned-c3` locally, which has its API on port 9090 inside the container (`docker exec`). It must report a mismatch when a balance is altered in the comparison by a test flag `--self-test-mismatch`.
- [ ] Write `wait_head.sh` and `snapshot.yml`. With no `pruned-snapshot` release yet, the workflow stops with "no snapshot yet".
- [ ] Commit: `ci: publish a pruned-node snapshot every 12 hours`.

### Task 6: First snapshot (needs the user's approval when it is time)

- [ ] When `credits-pruned-c3` is at the head:
  1. `docker stop -t 120 credits-pruned-c3`;
  2. run `snapshot-make.sh` on its directory;
  3. report the size and the digest to the user.
- [ ] With the user's OK:
  1. create the release `pruned-snapshot` and upload the archive and `latest.json`;
  2. start the container again;
  3. dispatch `snapshot.yml` once and check that it publishes a newer snapshot;
  4. restore that one into a fresh container with the published image, and check that the container reaches the head.

### Task 7: Merge

- [ ] Open a PR from this branch with the testnet label. CI must be green, plus the Task 3 and Task 4 runs. Merge it, since the user asked for this work to be merged when everything passes.
