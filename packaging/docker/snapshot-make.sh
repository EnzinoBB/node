#!/usr/bin/env bash
# Package the state of a stopped pruned node as a snapshot.
#
# Usage: snapshot-make.sh <node dir> <out dir> <mdb_copy> [run url]
#
# <node dir> is the working directory of a pruned LMDB node that has been stopped cleanly and was
# then started once more, so that its last log line "STATE DIGEST #<seq> <hex> wallets <n>" is the
# digest of its head. Writes <out>/credits-pruned-<seq>.tar.zst and <out>/latest.json. The snapshot
# holds db/lmdb, caches/, qs/, roundstat.dat and smartcontracts.dat; every LMDB environment is
# copied with mdb_copy -c, which writes only the live pages. Keys, configuration and logs stay out.
set -euo pipefail

node=$1 out=$2 mdb_copy=$3 run=${4:-}
[ -d "$node/db/lmdb" ] || { echo "no $node/db/lmdb: not a pruned LMDB node" >&2; exit 1; }
[ -d "$node/qs" ] || { echo "no $node/qs: no checkpoint" >&2; exit 1; }

digest=$(cat "$node"/log/log_*.txt 2>/dev/null | grep -a 'STATE DIGEST #' | tail -1 || true)
[ -n "$digest" ] || { echo "no STATE DIGEST line in $node/log" >&2; exit 1; }
seq=$(sed -E 's/.*STATE DIGEST #([0-9]+) .*/\1/' <<<"$digest")
hex=$(sed -E 's/.*STATE DIGEST #[0-9]+ ([0-9A-Fa-f]+) .*/\1/' <<<"$digest")
wallets=$(sed -E 's/.*wallets ([0-9]+).*/\1/' <<<"$digest")

mkdir -p "$out"
stage=$(mktemp -d "$out/.stage.XXXXXX")
trap 'rm -rf "$stage"' EXIT

# LMDB environments: compact copies; everything else as it is
copy_env() { mkdir -p "$stage/$1" && "$mdb_copy" -c "$node/$1" "$stage/$1"; }
copy_env db/lmdb
for env in "$node"/caches/*/; do
    rel=caches/$(basename "$env")
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

python3 - "$out/latest.json" <<PY
import json, sys
json.dump({"seq": $seq, "state_digest": "$hex", "wallets": $wallets, "checkpoint": "$checkpoint",
           "created": "$(date -u +%Y-%m-%dT%H:%M:%SZ)", "node": "$node_version",
           "file": "$name", "size": $size, "sha256": "$sha", "run": "$run"},
          open(sys.argv[1], "w"), indent=2)
PY
echo "snapshot $name: block $seq, digest $hex, $size bytes"
