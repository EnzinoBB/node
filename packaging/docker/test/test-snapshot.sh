#!/usr/bin/env bash
# Tests snapshot-make.sh and snapshot-restore.sh on a fake pruned node directory.
# Usage: test-snapshot.sh <dir with mdb_copy and mdb_load>
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
tools=$(cd "$1" && pwd)
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*"; exit 1; }

# an LMDB environment with a few records
mkenv() {
    mkdir -p "$1"
    printf 'VERSION=3\nformat=print\ntype=btree\nHEADER=END\n key1\n value1\n key2\n value2\nDATA=END\n' \
        | "$tools/mdb_load" -n -f /dev/stdin "$1/data.mdb" 2>/dev/null \
        || printf 'VERSION=3\nformat=print\ntype=btree\nHEADER=END\n key1\n value1\n key2\n value2\nDATA=END\n' \
        | "$tools/mdb_load" -f /dev/stdin "$1"
}

node="$work/node"
mkdir -p "$node/qs/1000" "$node/log" "$node/keys"
mkenv "$node/db/lmdb"
for c in hashdb indexdb poolcachedb seqdb; do mkenv "$node/caches/$c"; done
echo head > "$node/qs/1000/head.bin"
echo state > "$node/qs/1000/walletscache.dat"
echo rs > "$node/roundstat.dat"
echo sc > "$node/smartcontracts.dat"
echo secret > "$node/keys/NodePrivate.txt"
echo "[1] [info] STATE DIGEST #900 aaaa wallets 4" > "$node/log/log_0.txt"
echo "[2] [info] STATE DIGEST #1000 abababababababababababababababababababababababababababababababab wallets 5" >> "$node/log/log_0.txt"

out="$work/out"
"$here/snapshot-make.sh" "$node" "$out" "$tools/mdb_copy" "https://example/run/1"
archive="$out/credits-pruned-1000.tar.zst"
[ -f "$archive" ] || fail "archive missing"
[ -f "$out/latest.json" ] || fail "manifest missing"
python3 - "$out/latest.json" <<'PY' || fail "manifest content"
import json, sys
m = json.load(open(sys.argv[1]))
assert m["seq"] == 1000 and m["state_digest"] == "abababababababababababababababababababababababababababababababab" and m["wallets"] == 5, m
assert m["file"] == "credits-pruned-1000.tar.zst" and len(m["sha256"]) == 64 and m["size"] > 0, m
assert m["run"] == "https://example/run/1", m
PY
tar -I zstd -tf "$archive" | grep -q 'keys/' && fail "keys in the snapshot"
tar -I zstd -tf "$archive" | grep -q 'log/' && fail "logs in the snapshot"

# restore into an empty data dir from the manifest file
data="$work/data"
mkdir -p "$data"
"$here/snapshot-restore.sh" "$data" "$out/latest.json" || fail "restore exit $?"
for p in db/lmdb/data.mdb caches/hashdb/data.mdb qs/1000/walletscache.dat roundstat.dat smartcontracts.dat snapshot.json; do
    [ -e "$data/$p" ] || fail "restored $p missing"
done
[ -e "$data/.restore-tmp" ] && fail "restore temp left behind"
"$tools/mdb_dump" -p "$data/db/lmdb" | grep -q value2 || fail "restored LMDB unreadable"

# a corrupted archive: exit 2, nothing put in place
data2="$work/data2"; mkdir -p "$data2"
cp "$out/latest.json" "$work/latest-bad.json"
head -c 100 "$archive" > "$work/credits-pruned-1000.tar.zst"
cp "$work/latest-bad.json" "$work/latest.json"
set +e; "$here/snapshot-restore.sh" "$data2" "$work/latest.json"; rc=$?; set -e
[ "$rc" -eq 2 ] || fail "corrupted archive: exit $rc, expected 2"
[ -e "$data2/db" ] && fail "corrupted archive: db put in place"

# a manifest with a block number bash would evaluate: refused, nothing run
python3 - "$out/latest.json" "$work/evil.json" "$work/pwned" <<'PY2'
import json, sys
m = json.load(open(sys.argv[1]))
m["seq"] = "a[$(touch " + sys.argv[3] + ")]"
json.dump(m, open(sys.argv[2], "w"))
PY2
cp "$archive" "$work/"
set +e; "$here/snapshot-restore.sh" "$data2" "$work/evil.json"; rc=$?; set -e
[ "$rc" -eq 2 ] || fail "malicious manifest: exit $rc, expected 2"
[ -e "$work/pwned" ] && fail "malicious manifest ran a command"

# a missing manifest: exit 2
set +e; "$here/snapshot-restore.sh" "$data2" "$work/nope.json"; rc=$?; set -e
[ "$rc" -eq 2 ] || fail "missing manifest: exit $rc, expected 2"

echo "PASS: snapshot make and restore"
