# Credits node in Docker

```sh
mkdir credits && cd credits
curl -fsSLO https://raw.githubusercontent.com/EnzinoBB/node/master/packaging/docker/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/EnzinoBB/node/master/packaging/docker/.env.example
docker compose up -d && docker compose logs -f
```

A pruned node (the default) starts from a snapshot instead of syncing from block 0 for days. On its first start, with `CREDITS_SNAPSHOT=github` (the default), the container runs `node --download-snapshot`: it downloads the latest snapshot published on GitHub, checks every file against its signed manifest and restores it. When that fails, the node looks for a snapshot among its p2p neighbours (the community nodes that serve one) and, after `fetch_wait` (10 minutes) without one, syncs from block 0. `CREDITS_SNAPSHOT=p2p` skips the GitHub download, `off` syncs from block 0. The node checks a restored snapshot's state digest at its first start and stops if it differs. A full node syncs from block 0.

The node also serves its own snapshots to its neighbours (`CREDITS_SERVE_SNAPSHOT=on`, the default; at most 2 MB/s, half while it is a confidant). See `docs/snapshots.md`.

## Data: `~/.credits`

| Path | What |
| --- | --- |
| `config.ini` | Written from `.env` on the first start, then yours to edit. It wins over `.env` afterwards, which logs a warning if the two differ. |
| `keys/` | The node's identity. **Back it up.** The keys never go into `.env`, the logs or a snapshot. |
| `db/`, `caches/`, `qs/`, `*.dat` | Blocks, caches and checkpoints. |
| `log/` | Node logs (10 × 50 MB) and executor logs (kept 7 days). |
| `snapshot.json`, `snapshot.verified` | The snapshot this node started from, and its digest check at the first start. |
| `snapshot.mismatch` | The restored state did not match the snapshot: the node does not start until it is removed (set `CREDITS_RESET=1` once to start over). |
| `snapshot/` | The snapshots this node builds and serves to its neighbours. |
| `snapshot-download/` | A download in progress; the next first start resumes it. |

The container runs as uid 1000: the directory must be writable by it (`sudo chown -R 1000 ~/.credits`), or set `user:` in the compose file to your own uid.

## Changing the storage

| Change | Result |
| --- | --- |
| Full to pruned, same database | Allowed: old blocks are removed at the next checkpoints. |
| Pruned to full | Refused: the pruned blocks are gone. |
| A different database (`db_backend`) | Refused, unless `CREDITS_CONVERT_DB=yes` is set (see below). |

To start over, set `CREDITS_RESET=1` once. The old data is moved to `~/.credits/old-<date>`, not deleted.

### Changing the block database

To move an existing database to another backend, set `db_backend` in `config.ini` to the new one and set `CREDITS_CONVERT_DB=yes` in `.env`. Stop the node before converting. The node converts the database in place at the next start; it needs about 1 GB free beyond the database when converting from berkeleydb; about the size of the database again when converting from lmdb (its files do not shrink until the conversion ends); up to 4 times the size of the database when converting from rocksdb, which compresses its blocks. The node checks the free space first and names what it needs. It resumes if interrupted. Before converting from berkeleydb, keep a copy of `~/.credits/db`, taken with the node stopped right before the conversion (it takes as much space as `db/`; another disk is fine): a crash during a BerkeleyDB compaction can leave the old database unopenable while blocks are still to convert, and the copy is then the way back (see `docs/storage.md`, Interruptions). Remove `CREDITS_CONVERT_DB` afterwards.

## Upgrades

```sh
docker compose pull && docker compose up -d
```

The data, `config.ini` and the keys stay.
