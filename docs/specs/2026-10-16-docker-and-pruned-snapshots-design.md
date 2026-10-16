# Docker image and pruned-node snapshots: design

Status: draft for review, 2026-10-16.

## Goal

A new pruned node should be running near the chain head minutes after `docker compose up`, instead of days. Two pieces:

1. **A Docker image** of the community node, with one host directory (`~/.credits`) for configuration, keys and data, and a `.env` file for the common choices.
2. **Pruned-node snapshots** published automatically. A GitHub workflow advances the last snapshot to the chain head every 12 hours and publishes it. A new pruned container downloads the latest snapshot on its first start.

Out of scope here: full nodes. A pruned snapshot holds the state and only the recent blocks, so a full node still syncs from peers. Downloading history backwards from a snapshot is a later, separate design.

## 1. Docker image

**Image.** `ghcr.io/enzinobb/credits-node:<version>` plus `:latest`, built and pushed by `release.yml` from the same binaries as the release packages. Base `ubuntu:24.04`. It contains:

- in `/opt/credits`:
  - the node;
  - the contract executor and the JDK of the package;
  - the mainnet `known_hosts.txt` and `default_confidants.txt`;
- `entrypoint.sh`, `keygen.py`, `snapshot-restore.sh`;
- Python 3 with PyNaCl and base58, for the key generation;
- `zstd` and `curl`.

It runs as user `credits`, uid 1000.

**Volume.** `/data`, bound to `~/.credits` on the host:

```
~/.credits/
  config.ini      written on the first start, then the operator's file
  keys/           NodePublic.txt, NodePrivate.txt (back them up)
  db/ caches/ qs/ roundstat.dat smartcontracts.dat
  log/
```

The node runs with `/data` as its working directory. `config.ini` points to `/opt/credits` for the executor command and the jars, and to `keys/` for the keys.

**`.env`** (all optional):

| Variable | Default | Meaning |
| --- | --- | --- |
| `CREDITS_STORAGE` | `pruned` | `pruned` or `full` |
| `CREDITS_DB_BACKEND` | `lmdb` for pruned, `berkeleydb` for full | `berkeleydb`, `lmdb`, `rocksdb` |
| `CREDITS_SNAPSHOT` | `auto` | pruned only: `auto` restores the latest snapshot into an empty volume; `off` syncs from block 0 |
| `CREDITS_SNAPSHOT_URL` | the `pruned-snapshot` release of EnzinoBB/node | manifest location, for a mirror |
| `CREDITS_P2P_PORT` | `9510` | p2p port, UDP and TCP |
| `CREDITS_API` | `off` | `on` serves the Thrift API on port 9090 |
| `CREDITS_EXECUTOR` | `on` | run the contract executor |

**First start** (`entrypoint.sh`):

1. Write `config.ini` from the package template and the `.env` values, only if it does not exist. Afterwards `config.ini` is the source of truth, so a manual edit survives restarts and `.env` changes do not silently override it. `executor_multi_instance=true` is set, since the container runs one executor.
2. Generate the node keys into `keys/` if they are missing. Private keys never pass through environment variables.
3. If pruned, `CREDITS_SNAPSHOT=auto` and `db/` is empty, restore the latest snapshot (section 3). On any failure, log it and continue with a sync from block 0.
4. Start the node in the foreground. SIGTERM reaches it, so the node saves a checkpoint and stops cleanly.

**Every start:**

- delete the executor's daily logs older than 7 days, because the executor does not rotate them;
- the node's own logs rotate already (10 files of 50 MB).

**`docker-compose.yml` and `.env.example`**, in `packaging/docker/`:

- the p2p port published, and the API published on 127.0.0.1 only when enabled;
- `stop_grace_period: 120s`, `restart: unless-stopped`;
- CPU and memory limits as comments, with values that worked locally: 1.5 CPU and 4 GB for a pruned node during sync;
- Docker log rotation: 2 files of 50 MB;
- a healthcheck that fails when the last written block has not advanced for 10 minutes.

**Upgrades.** Pull the new tag and recreate the container. The volume, `config.ini` and the keys stay. A release that needs a configuration change says so in its notes.

## 2. Snapshot contents

A snapshot is taken from a pruned LMDB node that has been stopped cleanly. It is `credits-pruned-<seq>.tar.zst` and contains:

- `db/lmdb/`: the block database, copied with `mdb_copy -c`, which writes only live pages;
- `caches/`, `qs/`, `roundstat.dat`, `smartcontracts.dat`.

It contains no keys, configuration, logs or ban list.

A manifest `latest.json` comes with it:

```json
{ "seq": 176801234, "head_hash": "…", "state_digest": "…", "wallets": 123456,
  "created": "2026-10-16T12:00:00Z", "node": "4.2.532-community.3",
  "file": "credits-pruned-176801234.tar.zst", "size": 0, "sha256": "…",
  "run": "https://github.com/EnzinoBB/node/actions/runs/…" }
```

`seq` and `state_digest` come from the `STATE DIGEST` line that the node logs when it starts and every 1,000 blocks. Size estimate: about 2 GB of data today, smaller after `mdb_copy -c` and zstd. The first snapshot will measure it. A GitHub release asset may be at most 2 GB.

## 3. Restoring a snapshot

`snapshot-restore.sh` runs in the container on the first start. It:

1. downloads `latest.json`, then the archive;
2. checks the archive's SHA-256 against the manifest;
3. extracts it into `/data`;
4. logs the snapshot's sequence, digest and producing run.

Then the node starts. It quick-starts from the snapshot's checkpoint, reads the blocks after it from `db/`, and syncs the remaining hours from peers.

**Check after the start.** When the node logs its start-up `STATE DIGEST`, the entrypoint compares it with the manifest. A mismatch stops the container with a clear message and leaves the volume as it is.

## 4. Publishing workflow (`snapshot.yml`)

**Triggers:** every 12 hours (minutes jittered off :00) and manual dispatch. Only one run at a time.

**Steps:**

1. Download the latest pruned release package and the latest snapshot. Verify both.
2. Start the node from the snapshot, with the package configuration.
3. Wait until the node is at the head: `SYNC: Blocks remaining` below 100 for 3 minutes, and the last written block advancing. Timeout: 90 minutes.
4. Cross-check the balances against an independent source while the node runs with its API on. For 50 wallets picked at random from the node's `WalletsGet`, among those with no transaction in the last 24 hours, the balance must equal `ExtendedBalance` from `monitor.credits.com`. If the monitor cannot be reached, the run fails rather than skips the check. Then stop the node with SIGTERM and wait for it to exit.
5. Check it before publishing. Any failure stops the run, publishes nothing and keeps the previous snapshot.
   - The log has none of these:
     - `counted transactions is different`;
     - `Couldn't save block`;
     - `is not on this chain`;
     - a quick start one block behind its caches.
   - A start-up `STATE DIGEST` is present after the restart.
   - The balance cross-check of step 4 passed.
6. Package it (section 2) and publish it as assets of the release `pruned-snapshot`. Replace `latest.json` and delete the archives older than the last 3.

**Cost.** About 20 to 40 minutes every 12 hours, one runner, no matrix.

**Why not keep a node running on GitHub.** Actions is meant for building, testing and publishing the project. A node kept alive by chaining 6-hour jobs is a permanent service, and Actions on this repository was already disabled once for heavy use. Once at the head, 12 hours of mainnet blocks take minutes to catch up, so a short periodic job is enough.

**First snapshot.** Made locally from `credits-pruned-c3` once it reaches the head:

1. stop it cleanly;
2. package it with the same script;
3. upload it, with the user's approval at that time;
4. start it again.

The workflow then takes over.

## 5. Trust

Whoever starts from a snapshot trusts its producer for the state up to its head, as with Cosmos snapshots or Ethereum checkpoint sync. The blocks after the head are checked by the node as usual. Mitigations:

- every snapshot names the public workflow run that made it, and its steps and logs are visible;
- each snapshot comes from the previous one plus blocks the node validated, and is checked against an independent API before publishing;
- several snapshots are kept, so a bad one can be withdrawn and the previous one used;
- later: other community nodes, which also log `STATE DIGEST`, can confirm a snapshot's digest, and the manifest can carry their signatures.

## 6. Tests

**Private network, a new robustness scenario `snapshot-join`:**

1. Stop a pruned LMDB node at block 1,500.
2. Package a snapshot with the real script.
3. Start the node again.
4. Start a new node from the snapshot with `snapshot-restore.sh`.

Both must reach the target with the state digests of the network. A second variant restores a corrupted archive and must fall back to a sync from block 0.

**Image:**

- a CI job builds it;
- with the private network's genesis it starts one container per node and checks:
  - first-start config generation;
  - key generation;
  - restart with the same identity;
  - a clean stop that saves a checkpoint.

**Workflow:** the first runs are manual. Their manifests and snapshots are checked by restoring them in a fresh container that must reach the head.

## Open points

- The image registry: GHCR on EnzinoBB/node needs `packages: write` in `release.yml`. The image is public, like the repository.
- The 2 GB limit for a release asset: if a snapshot ever exceeds it, it is split into parts.
- The balance cross-check relies on `monitor.credits.com`. BK's dashboard API (`wallet.get`) can be a second source.
