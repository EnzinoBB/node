#!/usr/bin/env bash
# Restore a pruned-node snapshot into a node's data directory.
#
# Usage: snapshot-restore.sh <data dir> <manifest: URL or file>
#
# Reads the manifest (latest.json), downloads the archive it names from the same location, checks
# its SHA-256, extracts it into <data>/.restore-tmp and only then moves db/, caches/, qs/ and the
# .dat files into <data>, with the manifest as <data>/snapshot.json. Exit 0: restored. Exit 2: not
# restored, with the reason on stderr; <data> is left as it was. Any other failure happens while the
# files are moved into <data>: <data>/.restore-incomplete then stays, and what was moved is the
# snapshot's, to be removed before the next attempt (the entrypoint does so).
set -euo pipefail

data=$1 manifest=$2
tmp=$data/.restore-tmp
give_up() { echo "snapshot not restored: $*" >&2; rm -rf "$tmp"; exit 2; }

rm -rf "$tmp"
mkdir -p "$tmp"
case "$manifest" in
    http://*|https://*)
        curl -fsSL --retry 3 -o "$tmp/latest.json" "$manifest" || give_up "cannot download $manifest"
        base=${manifest%/*} ;;
    *)
        [ -f "$manifest" ] || give_up "no manifest $manifest"
        cp "$manifest" "$tmp/latest.json"
        base=$(cd "$(dirname "$manifest")" && pwd) ;;
esac

read -r file sha seq < <(python3 -c '
import json, sys
m = json.load(open(sys.argv[1]))
print(m["file"], m["sha256"], m["seq"])' "$tmp/latest.json") || give_up "unreadable manifest"
# the manifest comes from the network: accept only the exact shapes this script uses
[[ "$file" =~ ^credits-pruned-[0-9]{1,12}\.tar\.zst$ ]] || give_up "bad file name in the manifest"
[[ "$sha" =~ ^[0-9a-f]{64}$ ]] || give_up "bad checksum in the manifest"
[[ "$seq" =~ ^[0-9]{1,12}$ ]] || give_up "bad block number in the manifest"
python3 -c 'import json,sys,re; m=json.load(open(sys.argv[1])); sys.exit(0 if re.fullmatch(r"[0-9A-Fa-f]{64}", str(m.get("state_digest",""))) else 1)' \
    "$tmp/latest.json" || give_up "bad state digest in the manifest"

case "$base" in
    http://*|https://*) curl -fsSL --retry 3 -o "$tmp/$file" "$base/$file" || give_up "cannot download $file" ;;
    *) cp "$base/$file" "$tmp/$file" 2>/dev/null || give_up "no archive $base/$file" ;;
esac
echo "$sha  $tmp/$file" | sha256sum -c --quiet - >/dev/null 2>&1 || give_up "checksum mismatch for $file"

mkdir -p "$tmp/x"
zstd -q -dc "$tmp/$file" | tar -C "$tmp/x" -xf - || give_up "cannot extract $file"
[ -d "$tmp/x/db/lmdb" ] && [ -d "$tmp/x/qs" ] || give_up "$file has no db/lmdb or qs"

# from here on <data> changes: the marker stays until the last move, so that an interrupted restore is
# redone and never taken for the node's own data
touch "$data/.restore-incomplete"
for p in db caches qs roundstat.dat smartcontracts.dat; do
    [ ! -e "$tmp/x/$p" ] || { rm -rf "${data:?}/$p"; mv "$tmp/x/$p" "$data/$p"; }
done
mv "$tmp/latest.json" "$data/snapshot.json"
rm -f "$data/.restore-incomplete"
rm -rf "$tmp"
echo "snapshot restored: block $seq from $file"
