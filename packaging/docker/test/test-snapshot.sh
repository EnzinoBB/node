#!/usr/bin/env bash
# Tests snapshot-make.sh and snapshot-restore.sh on a fake pruned node directory.
# Usage: test-snapshot.sh <dir with mdb_copy, mdb_load, mdb_dump and mdb_stat>
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
tar -I zstd -tf "$archive" | grep -q 'poolcachedb' && fail "poolcachedb in the snapshot"

# a corrupt LMDB environment in caches/ stops the script, and no archive is written
bad="$work/bad-node"
cp -a "$node" "$bad"
rm -rf "$bad/caches/hashdb"
mkdir -p "$bad/caches/hashdb"
python3 -c 'import sys
for i in range(5000): sys.stdout.write(" %064x\n %s\n" % (i * 7919, str(i).encode().hex()))' \
    | { printf 'VERSION=3\nformat=bytevalue\ntype=btree\nHEADER=END\n'; cat; printf 'DATA=END\n'; } \
    | "$tools/mdb_load" -f /dev/stdin "$bad/caches/hashdb"
"$tools/mdb_dump" "$bad/caches/hashdb" >/dev/null || fail "the test environment does not read before it is damaged"
# zero one page in the middle of the tree, as a page damaged on disk
dd if=/dev/zero of="$bad/caches/hashdb/data.mdb" bs=4096 seek=6 count=1 conv=notrunc 2>/dev/null
bad_out="$work/bad-out"
if "$here/snapshot-make.sh" "$bad" "$bad_out" "$tools/mdb_copy" 2>"$work/bad.err"; then
    fail "a snapshot was made from a corrupt LMDB environment"
fi
grep -q 'caches/hashdb' "$work/bad.err" || fail "the refusal does not name the environment: $(cat "$work/bad.err")"
! ls "$bad_out"/*.tar.zst >/dev/null 2>&1 || fail "an archive was left behind for a corrupt environment"

# every page after the meta pages zeroed: mdb_dump alone reads that as an empty tree, with no error
"$here/lmdb-check.sh" "$tools" "$node/caches/seqdb" || fail "lmdb-check refuses a whole environment"
cp -a "$node" "$work/zero-node"
python3 -c 'import sys
for i in range(5000): sys.stdout.write(" %064x\n %s\n" % (i * 7919, str(i).encode().hex()))' \
    | { printf 'VERSION=3\nformat=bytevalue\ntype=btree\nHEADER=END\n'; cat; printf 'DATA=END\n'; } \
    > "$work/hashdb.dump"
rm -rf "$work/zero-node/caches/hashdb"; mkdir -p "$work/zero-node/caches/hashdb"
"$tools/mdb_load" -f "$work/hashdb.dump" "$work/zero-node/caches/hashdb"
used=$("$tools/mdb_stat" -e "$work/zero-node/caches/hashdb" | sed -nE 's/.*Number of pages used: ([0-9]+).*/\1/p')
dd if=/dev/zero of="$work/zero-node/caches/hashdb/data.mdb" bs=4096 seek=2 count=$((used - 2)) conv=notrunc 2>/dev/null
"$tools/mdb_dump" "$work/zero-node/caches/hashdb" >/dev/null 2>&1 || fail "the zeroed environment was expected to dump without error"
! "$here/lmdb-check.sh" "$tools" "$work/zero-node/caches/hashdb" 2>/dev/null || fail "lmdb-check accepts a zeroed environment"

# the digest comes from the newest log by modification time, not by name (log_10 sorts before log_9)
rot="$work/rotated-node"
cp -a "$node" "$rot"
rm -f "$rot"/log/log_*.txt
echo "[1] [info] STATE DIGEST #1000 abababababababababababababababababababababababababababababababab wallets 5" > "$rot/log/log_9.txt"
touch -d '2 minutes ago' "$rot/log/log_9.txt"
echo "[2] [info] STATE DIGEST #1200 cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd wallets 6" > "$rot/log/log_10.txt"
"$here/snapshot-make.sh" "$rot" "$work/rot-out" "$tools/mdb_copy" >/dev/null
python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); sys.exit(0 if m["seq"] == 1200 else 1)' "$work/rot-out/latest.json" \
    || fail "the manifest took an older digest: $(cat "$work/rot-out/latest.json")"

# a running node (it holds db/.lock) is refused
flock "$node/db/.lock" sleep 30 &
locker=$!
sleep 1
if "$here/snapshot-make.sh" "$node" "$work/run-out" "$tools/mdb_copy" 2>"$work/run.err"; then
    kill "$locker"; fail "a snapshot was made from a running node"
fi
kill "$locker" 2>/dev/null || true; wait "$locker" 2>/dev/null || true
grep -q 'is running' "$work/run.err" || fail "the refusal does not say the node is running: $(cat "$work/run.err")"

# restore into an empty data dir from the manifest file
data="$work/data"
mkdir -p "$data"
"$here/snapshot-restore.sh" "$data" "$out/latest.json" || fail "restore exit $?"
for p in db/lmdb/data.mdb caches/hashdb/data.mdb caches/seqdb/data.mdb caches/indexdb/data.mdb qs/1000/walletscache.dat roundstat.dat smartcontracts.dat snapshot.json; do
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
