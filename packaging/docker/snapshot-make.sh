#!/usr/bin/env bash
# Package the state of a stopped pruned node as a snapshot.
#
# Usage: snapshot-make.sh <node dir> <out dir> <mdb_copy> [run url]
#
# <node dir> is the working directory of a pruned LMDB node that has been stopped cleanly and was
# then started once more, so that its last log line "STATE DIGEST #<seq> <hex> wallets <n>" is the
# digest of its head. Writes <out>/credits-pruned-<seq>.tar.zst and <out>/latest.json. The snapshot
# holds db/lmdb, caches/ (but poolcachedb), qs/, roundstat.dat and smartcontracts.dat; every LMDB environment is
# copied with mdb_copy -c, which writes only the live pages, and each copy must pass lmdb-check.sh
# (with mdb_dump and mdb_stat next to mdb_copy): a corrupt environment stops the script, so that it
# never reaches the nodes that restore the snapshot. Keys, configuration and logs stay out.
set -euo pipefail

node=$1 out=$2 mdb_copy=$3 run=${4:-}
tools=$(dirname "$mdb_copy")
here=$(cd "$(dirname "$0")" && pwd)
[ -x "$tools/mdb_dump" ] && [ -x "$tools/mdb_stat" ] || { echo "no mdb_dump and mdb_stat next to $mdb_copy" >&2; exit 1; }
[ -d "$node/db/lmdb" ] || { echo "no $node/db/lmdb: not a pruned LMDB node" >&2; exit 1; }
[ -d "$node/qs" ] || { echo "no $node/qs: no checkpoint" >&2; exit 1; }
# a running node holds db/.lock: its database, caches and checkpoints would not match each other
flock -n "$node/db/.lock" true 2>/dev/null || { echo "the node in $node is running: stop it first" >&2; exit 1; }

# the logs oldest first by modification time: by name log_10 would come before log_9
digest=$(cat $(ls -tr "$node"/log/log_*.txt 2>/dev/null) "$node"/node.log /dev/null 2>/dev/null | grep -a 'STATE DIGEST #' | tail -1 || true)
[ -n "$digest" ] || { echo "no STATE DIGEST line in $node/log" >&2; exit 1; }
seq=$(sed -E 's/.*STATE DIGEST #([0-9]+) .*/\1/' <<<"$digest")
hex=$(sed -E 's/.*STATE DIGEST #[0-9]+ ([0-9A-Fa-f]+) .*/\1/' <<<"$digest")
wallets=$(sed -E 's/.*wallets ([0-9]+).*/\1/' <<<"$digest")
# the log is written by a node that talks to untrusted peers: use only the exact shapes expected
[[ "$seq" =~ ^[0-9]{1,12}$ && "$hex" =~ ^[0-9A-Fa-f]{64}$ && "$wallets" =~ ^[0-9]{1,12}$ ]] \
    || { echo "malformed STATE DIGEST line: $digest" >&2; exit 1; }

mkdir -p "$out"
stage=$(mktemp -d "$out/.stage.XXXXXX")
trap 'rm -rf "$stage"' EXIT

# LMDB environments: compact copies; everything else as it is
copy_env() {
    mkdir -p "$stage/$1"
    "$mdb_copy" -c "$node/$1" "$stage/$1" 2>/dev/null \
        || { echo "cannot copy the LMDB environment $1: it is corrupt or in use" >&2; exit 1; }
    "$here/lmdb-check.sh" "$tools" "$stage/$1" 2>/dev/null \
        || { echo "the LMDB environment $1 is corrupt: $("$here/lmdb-check.sh" "$tools" "$stage/$1" 2>&1 | sed "s|$stage/||")" >&2; exit 1; }
}
copy_env db/lmdb
for env in "$node"/caches/*/; do
    rel=caches/$(basename "$env")
    # the node wipes poolcachedb at every start: nothing in it is worth shipping
    [ "$rel" != caches/poolcachedb ] || continue
    if [ -e "$env/data.mdb" ]; then copy_env "$rel"; else mkdir -p "$stage/caches" && cp -a "$env" "$stage/$rel"; fi
done
cp -a "$node/qs" "$stage/qs"
for f in roundstat.dat smartcontracts.dat; do
    [ ! -e "$node/$f" ] || cp -a "$node/$f" "$stage/$f"
done

name=credits-pruned-$seq.tar.zst
tar -C "$stage" -cf - . | zstd -q -T0 -19 -o "$out/$name.part"
mv "$out/$name.part" "$out/$name"
size=$(stat -c %s "$out/$name")
sha=$(sha256sum "$out/$name" | cut -d' ' -f1)
checkpoint=$(ls "$node/qs" | sort -n | tail -1)
node_version=$(cat "$node/VERSION" 2>/dev/null || echo unknown)

# values as arguments, never interpolated into the program
python3 - "$out/latest.json" "$seq" "$hex" "$wallets" "$checkpoint" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
    "$node_version" "$name" "$size" "$sha" "$run" <<'PY'
import json, sys
path, seq, hex_, wallets, checkpoint, created, node, name, size, sha, run = sys.argv[1:]
json.dump({"seq": int(seq), "state_digest": hex_, "wallets": int(wallets), "checkpoint": checkpoint,
           "created": created, "node": node, "file": name, "size": int(size), "sha256": sha, "run": run},
          open(path, "w"), indent=2)
PY
echo "snapshot $name: block $seq, digest $hex, $size bytes"
