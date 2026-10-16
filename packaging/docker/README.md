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

The container runs as uid 1000: the directory must be writable by it (`sudo chown -R 1000 ~/.credits`), or set `user:` in the compose file to your own uid.

## Changing the storage

| Change | Result |
| --- | --- |
| Full to pruned, same database | Allowed: old blocks are removed at the next checkpoints. |
| Pruned to full | Refused: the pruned blocks are gone. |
| A different database (`db_backend`) | Refused: checkpoints and caches belong to the old one. |

To start over, set `CREDITS_RESET=1` once. The old data is moved to `~/.credits/old-<date>`, not deleted.

## Upgrades

```sh
docker compose pull && docker compose up -d
```

The data, `config.ini` and the keys stay.
