# Credits node in Docker

```sh
mkdir credits && cd credits
curl -fsSLO https://raw.githubusercontent.com/EnzinoBB/node/master/packaging/docker/docker-compose.yml
curl -fsSL -o .env https://raw.githubusercontent.com/EnzinoBB/node/master/packaging/docker/.env.example
docker compose up -d && docker compose logs -f
```

A pruned node (the default) first downloads the latest snapshot, published every 12 hours, and starts a few hours behind the chain head instead of syncing from block 0 for days. It checks the snapshot's checksum, and stops if its start-up state digest differs from the snapshot's. A full node syncs from block 0.

## Data: `~/.credits`

| Path | What |
| --- | --- |
| `config.ini` | Written from `.env` on the first start, then yours to edit. It wins over `.env` afterwards, which logs a warning if the two differ. |
| `keys/` | The node's identity. **Back it up.** The keys never go into `.env`, the logs or a snapshot. |
| `db/`, `caches/`, `qs/`, `*.dat` | Blocks, caches and checkpoints. |
| `log/` | Node logs (10 × 50 MB) and executor logs (kept 7 days). |
| `snapshot.json` | The snapshot this node started from. |
| `.restore-incomplete` | Present only while a snapshot restore moves its files into place. If the container stops then, the next start removes what was moved and restores again (a restore runs only without a database). |

The container runs as uid 1000: the directory must be writable by it (`sudo chown -R 1000 ~/.credits`), or set `user:` in the compose file to your own uid.

## Changing the storage

| Change | Result |
| --- | --- |
| Full to pruned, same database | Allowed: old blocks are removed at the next checkpoints. |
| Pruned to full | Refused: the pruned blocks are gone. |
| A different database (`db_backend`) | Refused, unless `CREDITS_CONVERT_DB=yes` is set (see below). |

To start over, set `CREDITS_RESET=1` once. The old data is moved to `~/.credits/old-<date>`, not deleted.

### Changing the block database

To move an existing database to another backend, set `db_backend` in `config.ini` to the new one and set `CREDITS_CONVERT_DB=yes` in `.env`. Stop the node before converting. The node converts the database in place at the next start; it needs about 1 GB free beyond the database when converting from berkeleydb; about the size of the database again when converting from lmdb (its files do not shrink until the conversion ends); up to 4 times the size of the database when converting from rocksdb, which compresses its blocks. The node checks the free space first and names what it needs. It resumes if interrupted. Before converting from berkeleydb, keep a copy of `~/.credits/db` (with the node stopped): a crash during a BerkeleyDB compaction can leave the old database unopenable while blocks are still to convert, and the copy is then the way back (see `docs/storage.md`, Interruptions). Remove `CREDITS_CONVERT_DB` afterwards.

## Upgrades

```sh
docker compose pull && docker compose up -d
```

The data, `config.ini` and the keys stay.
