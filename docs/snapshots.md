# Snapshots

A snapshot is the state of the chain at one periodic checkpoint: the checkpoint files, the blocks
before it as a hash chain, and a `snapshot.json` manifest with the SHA-256 of every file and every
512 KiB piece, the state digest of the checkpoint, and the signature of the node that built it.
A node can start from a snapshot instead of syncing from block 0 (about 35 hours on mainnet).

The settings are in the `[snapshot]` section of `config.ini`; the sizes are in KB/s
(`2000` = 2 MB/s).

## Serving snapshots to the neighbours

```ini
[snapshot]
serve=on
serve_rate_kbps=2000
serve_max_peers=8
```

With `serve=on` the node builds the snapshot of each periodic checkpoint (`[storage]
checkpoint_every`) into `snapshot/<block>/`, at low priority, and answers the snapshot requests of
its p2p neighbours, only while it is synced with the network. It announces this to the network at
start: a node switched to `serve=on` while running serves, but new nodes find it only after its
next start.

Log lines: `SNAPSHOT: built block S, N files, M MB`, `SNAPSHOT: serving block S`,
`SNAPSHOT: served block S to <key>, M MB in T s`, and an hourly traffic summary.

Limits: `serve_rate_kbps` for all peers together, half of it for each peer, at most
`serve_max_peers` peers within 30 s, and half of everything while the node is a confidant or the
writer of the round. Serving costs disk space for two snapshots (about 200 MB on mainnet with the
default `snapshot_blocks`).

## Fetching a snapshot at a first start

```ini
[snapshot]
fetch=auto
min_sources=2
fetch_wait=600
fetch_fallback=sync
```

A node whose `db/` holds no database and whose `qs/` holds no checkpoint, with `db_backend=lmdb` or
`rocksdb`, does not open its chain at once: it looks for neighbours that serve snapshots
(`SNAPSHOT: empty database: looking for snapshots on the network`), collects their manifests and
takes the snapshot that `min_sources` independent sources agree on. Sources count by network
(one per /24 for IPv4, /48 for IPv6); keys listed in `sources=` (base58 node keys, comma
separated) count once each whatever their address, and win over other groups. `sources_file`
(default `snapshot_sources.txt`, lines `ip:port key`) lists more nodes to ask, without trusting
them.

Every piece is checked against the signed manifest, every file as a whole, and the import checks
everything again (signatures and hash chain of the blocks, the checkpoint's state digest) before
anything is written to `db/`. Meanwhile the node processes no other message. Once imported, it
quick-starts from the checkpoint and syncs from there like any node.

When no agreement is reached within `fetch_wait` seconds, `fetch_fallback=sync` opens an empty
database and syncs from block 0; `fetch_fallback=wait` keeps waiting (a line every 5 minutes). Both
lines name the manual way: stop the node and run `./node --download-snapshot` in its folder (Docker:
`CREDITS_SNAPSHOT=github`), then start it again.

A BerkeleyDB node cannot start above block 0 and never fetches. `fetch=off` turns fetching off;
nodes of a private network that starts from block 0 should set it, or each waits `fetch_wait` at
its first start.

## Files next to the node

| File | Meaning |
| --- | --- |
| `snapshot.json` | the manifest of the snapshot the node started from |
| `snapshot.sources.json` | the sources it was fetched from (keys and addresses) |
| `snapshot.verified` | the state digest at the first start matched the manifest |
| `snapshot.mismatch` | it did not: the node stopped with exit code 4 and refuses to start until the file is removed (and the database with it) |
| `snapshot-tmp/` | a fetch in progress; removed at the next start |
| `db/.importing` | an import in progress; at the next start the partial database is removed and the fetch starts again |
| `snapshot/<block>/` | snapshots this node built and serves (`serve=on`) |

## Downloading the published snapshot

`./node --download-snapshot` (in the node's folder, node stopped, empty database) downloads the
snapshot published on GitHub every 12 hours with `curl`, checks every file against its manifest and
restores it; `./node --download-snapshot <url>/snapshot.json` takes another one laid out the same
way (the files next to the manifest as `s<block>-<name>`, https only). The manifest's signature
proves that the files are those its builder packaged, not who that builder is: the trust is the
https address, so give another address only when you trust whoever publishes there. It exits 0 when restored, 2 otherwise;
a download that stops is resumed by running it again (`snapshot-download/`). The node's normal
start never downloads from GitHub.

## Exporting and importing by hand

`./node --export-snapshot <dir>` on a stopped node builds a snapshot of its latest checkpoint;
`./node --import-snapshot <dir>/snapshot.json` restores one into an empty database. See
[differences.md](differences.md).
