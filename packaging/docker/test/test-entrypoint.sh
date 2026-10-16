#!/usr/bin/env bash
# Tests the image's entrypoint with a stub node: first-start setup, kept configuration and keys,
# snapshot restore and digest check, failures, and storage changes.
# Usage: test-entrypoint.sh <dir with mdb_copy and mdb_load>
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
repo=$(cd "$here/../.." && pwd)
tools=$(cd "$1" && pwd)
work=$(mktemp -d)
image=credits-node:entrypoint-test
cleanup() { docker rm -f $(docker ps -aq --filter label=credits-entrypoint-test) >/dev/null 2>&1 || true; rm -rf "$work" 2>/dev/null || sudo_rm "$work"; }
sudo_rm() { docker run --rm -v "$1:/w" ubuntu:24.04 rm -rf /w/* /w/.[!.]* 2>/dev/null; rm -rf "$1"; }
trap cleanup EXIT
fail() { echo "FAIL: $*"; exit 1; }
pass() { echo "ok: $*"; }

# a fake package: the real configuration, a stub node, no executor
pkg="$work/package"
mkdir -p "$pkg/ojdkbuild/java-11-openjdk-11.0.4/bin"
cp "$repo"/packaging/mainnet/{settings.properties,known_hosts.txt,default_confidants.txt} "$pkg/"
cp "$repo/packaging/mainnet/config.ini" "$pkg/config.ini"
printf '\n[storage]\ncheckpoint_every=100000\nblock_db=lmdb\n' >> "$pkg/config.ini"
touch "$pkg/contract-executor.jar" "$pkg/sc-api-support.jar"
cat > "$pkg/node" <<'STUB'
#!/bin/bash
# stub node: records its arguments, logs a start-up digest and a written sequence, stops on SIGTERM
echo "$@" > /data/stub-args
if [ -f /data/stub-digest ]; then read -r seq hex < /data/stub-digest
elif [ -f /data/snapshot.json ]; then read -r seq hex < <(python3 -c 'import json; m=json.load(open("/data/snapshot.json")); print(m["seq"], m["state_digest"])')
else seq=1 hex=00; fi
mkdir -p /data/log
echo "[t] [info] STATE DIGEST #$seq $hex wallets 3" >> /data/log/log_0.txt
echo "[t] [info]  Last written sequence = $seq, neighbour nodes = 1" >> /data/log/log_0.txt
trap 'echo stopped >> /data/stub-events; exit 0' TERM
for _ in $(seq 1 "${STUB_SECONDS:-3}"); do sleep 1; done
STUB
chmod +x "$pkg/node"
"$here/build.sh" "$pkg" "$image" > "$work/build.log" 2>&1 || { tail -20 "$work/build.log"; fail "image build"; }

# a snapshot made by the real script from a fake node
fake="$work/fake"
mkdir -p "$fake/qs/1000" "$fake/log" "$fake/db/lmdb" "$fake/caches/seqdb"
for env in "$fake/db/lmdb" "$fake/caches/seqdb"; do
    printf 'VERSION=3\nformat=print\ntype=btree\nHEADER=END\n k\n v\nDATA=END\n' | "$tools/mdb_load" -f /dev/stdin "$env"
done
echo x > "$fake/qs/1000/head.bin"
echo "[t] [info] STATE DIGEST #1000 abcd wallets 5" > "$fake/log/log_0.txt"
"$here/snapshot-make.sh" "$fake" "$work/snap" "$tools/mdb_copy" >/dev/null

run() {  # run <data dir> [docker args...]: runs the container to its end, prints its output
    local data=$1; shift
    docker run --rm --label credits-entrypoint-test --user "$(id -u)" -v "$data:/data" \
        -v "$work/snap:/snap:ro" "$@" "$image" 2>&1
}
newdata() { local d="$work/data-$1"; mkdir -p "$d"; echo "$d"; }

# 1. first start
d=$(newdata 1)
out=$(run "$d" -e CREDITS_SNAPSHOT=off)
for f in config.ini settings.properties known_hosts.txt default_confidants.txt keys/NodePublic.txt keys/NodePrivate.txt .storage; do
    [ -e "$d/$f" ] || fail "first start: $f missing"
done
[ "$(stat -c %a "$d/keys/NodePrivate.txt")" = 600 ] || fail "private key mode"
grep -q 'executor_command=/opt/credits/ojdkbuild/java-11-openjdk-11.0.4/bin/java .* -jar /opt/credits/contract-executor.jar' "$d/config.ini" || fail "executor path"
grep -q '^executor_multi_instance=true' "$d/config.ini" || fail "multi instance"
grep -q '^prune_keep_blocks=1000' "$d/config.ini" && grep -q '^db_backend=lmdb' "$d/config.ini" || fail "pruned storage"
[ "$(grep -c '^\[storage\]' "$d/config.ini")" = 1 ] || fail "template [storage] not dropped"
grep -q '^block_db=lmdb' "$d/config.ini" && fail "template block_db kept"
grep -q -- '--public-key-file keys/NodePublic.txt --private-key-file keys/NodePrivate.txt' "$d/stub-args" || fail "key args"
grep -q 'mode=pruned backend=lmdb' "$d/.storage" || fail ".storage"
pass "first start"

# 2. second start keeps keys and a hand edit
key=$(cat "$d/keys/NodePublic.txt")
sed -i 's/^min_neighbours=.*/min_neighbours=7/; $a ; operator note' "$d/config.ini"
run "$d" -e CREDITS_SNAPSHOT=off >/dev/null
[ "$(cat "$d/keys/NodePublic.txt")" = "$key" ] || fail "keys changed"
grep -q '; operator note' "$d/config.ini" || fail "hand edit lost"
pass "second start keeps keys and config.ini"

# 3. snapshot restore and digest check
d=$(newdata 3)
out=$(run "$d" -e CREDITS_SNAPSHOT_URL=/snap/latest.json -e STUB_SECONDS=8)
grep -q 'snapshot restored: block 1000' <<<"$out" || { echo "$out"; fail "restore"; }
[ -f "$d/snapshot.verified" ] || { echo "$out"; fail "digest not verified"; }
[ -d "$d/db/lmdb" ] || fail "restored db missing"
pass "snapshot restored and verified"

# 4. digest mismatch stops the node, and the next start is refused
d=$(newdata 4)
echo "1000 ffff" > "$d/stub-digest"
start=$(date +%s)
out=$(run "$d" -e CREDITS_SNAPSHOT_URL=/snap/latest.json -e STUB_SECONDS=60)
grep -q 'SNAPSHOT DIGEST MISMATCH' <<<"$out" || { echo "$out"; fail "mismatch not reported"; }
[ $(( $(date +%s) - start )) -lt 40 ] || fail "mismatch did not stop the node"
set +e; out=$(run "$d" -e CREDITS_SNAPSHOT_URL=/snap/latest.json); rc=$?; set -e
[ "$rc" = 4 ] || { echo "$out"; fail "start after mismatch: exit $rc, expected 4"; }
pass "digest mismatch stops and blocks the node"

# 5. unreachable snapshot: sync from block 0
d=$(newdata 5)
out=$(run "$d" -e CREDITS_SNAPSHOT_URL=http://127.0.0.1:9/latest.json)
grep -q 'syncing from block 0 instead' <<<"$out" || { echo "$out"; fail "fallback"; }
[ -f "$d/stub-args" ] || fail "node not started after a failed restore"
[ -e "$d/db" ] && fail "db created by a failed restore"
pass "unreachable snapshot falls back to a sync"

# 6. a leftover .restore-tmp is wiped
d=$(newdata 6)
mkdir -p "$d/.restore-tmp/x"
run "$d" -e CREDITS_SNAPSHOT=off >/dev/null
[ -e "$d/.restore-tmp" ] && fail ".restore-tmp left"
pass "leftover restore directory wiped"

# storage changes, on a node with data
withdata() { local d; d=$(newdata "$1"); run "$d" -e CREDITS_SNAPSHOT=off "${@:2}" >/dev/null; mkdir -p "$d/db/x"; touch "$d/db/x/data"; echo "$d"; }

# 7. full to pruned, same backend: starts with a note
d=$(withdata 7 -e CREDITS_STORAGE=full -e CREDITS_DB_BACKEND=lmdb)
printf 'prune_keep_blocks=1000\n' >> "$d/config.ini"
out=$(run "$d")
grep -q 'switching from full to pruned storage' <<<"$out" || { echo "$out"; fail "full to pruned"; }
grep -q 'mode=pruned backend=lmdb' "$d/.storage" || fail "full to pruned record"
pass "full to pruned allowed"

# 8. pruned to full: refused
d=$(withdata 8)
sed -i 's/^prune_keep_blocks=.*/prune_keep_blocks=0/' "$d/config.ini"
set +e; out=$(run "$d"); rc=$?; set -e
[ "$rc" = 3 ] && grep -q 'cannot become a full node' <<<"$out" || { echo "$out"; fail "pruned to full: exit $rc"; }
pass "pruned to full refused"

# 9. backend change: refused
d=$(withdata 9)
sed -i 's/^db_backend=.*/db_backend=rocksdb/' "$d/config.ini"
set +e; out=$(run "$d"); rc=$?; set -e
[ "$rc" = 3 ] && grep -q 'written by lmdb, config.ini asks for rocksdb' <<<"$out" || { echo "$out"; fail "backend change: exit $rc"; }

# 10. CREDITS_RESET=1 moves the data aside and starts over
out=$(run "$d" -e CREDITS_RESET=1 -e CREDITS_SNAPSHOT=off)
ls -d "$d"/old-* >/dev/null 2>&1 || { echo "$out"; fail "reset: no old- directory"; }
[ -e "$d/db/x/data" ] && fail "reset: db kept in place"
grep -q 'backend=rocksdb' "$d/.storage" || fail "reset: record"
pass "backend change refused, CREDITS_RESET=1 starts over"

# 11. .env differing from config.ini: a warning
out=$(run "$d" -e CREDITS_STORAGE=full -e CREDITS_SNAPSHOT=off)
grep -q 'warning: CREDITS_STORAGE=full, but config.ini runs a pruned node' <<<"$out" || { echo "$out"; fail ".env warning"; }
pass ".env differing from config.ini reported"

echo "PASS: entrypoint"
