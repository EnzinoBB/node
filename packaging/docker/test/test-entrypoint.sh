#!/usr/bin/env bash
# Tests the image's entrypoint with a stub node: first-start setup, kept configuration and keys,
# the snapshot download on request (--download-snapshot) and the [snapshot] settings, the refusal
# after a snapshot mismatch, and storage changes.
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
# stub node: --download-snapshot is recorded and exits STUB_DOWNLOAD_RC (0); otherwise it records its
# arguments, logs a start-up digest and a written sequence, stops on SIGTERM
if [[ " $* " == *" --download-snapshot "* ]]; then
    echo "$@" > /data/stub-download
    exit "${STUB_DOWNLOAD_RC:-0}"
fi
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

run() {  # run <data dir> [docker args...]: runs the container to its end, prints its output
    local data=$1; shift
    docker run --rm --label credits-entrypoint-test --user "$(id -u)" -v "$data:/data" "$@" "$image" 2>&1
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

# 3. a pruned node's first start downloads the snapshot (github, the default), then starts
d=$(newdata 3)
out=$(run "$d")
grep -q -- '--download-snapshot https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/snapshot.json' "$d/stub-download" \
    || { echo "$out"; fail "no download on the first start"; }
grep -q 'snapshot restored' <<<"$out" || { echo "$out"; fail "download result not logged"; }
[ -f "$d/stub-args" ] || fail "node not started after the download"
grep -q -- '--download-snapshot' "$d/stub-args" && fail "the normal start downloads"
grep -q '^serve=on' "$d/config.ini" && grep -q '^fetch=auto' "$d/config.ini" || fail "[snapshot] serve=on, fetch=auto"
pass "first start downloads the snapshot, then starts"

# 4. a failed download: the node starts anyway (it tries its neighbours, then block 0)
d=$(newdata 4)
out=$(run "$d" -e STUB_DOWNLOAD_RC=2)
grep -q 'no snapshot downloaded (exit 2)' <<<"$out" || { echo "$out"; fail "failed download not reported"; }
[ -f "$d/stub-args" ] || fail "node not started after a failed download"
pass "a failed download still starts the node"

# 5. the next start with data does not download
rm -f "$d/stub-download"
mkdir -p "$d/db/x"; touch "$d/db/x/data"
run "$d" >/dev/null
[ -e "$d/stub-download" ] && fail "downloaded again on a node with data"
pass "no download once the node has data"

# 6. p2p: no download, fetch=auto; off: no download, fetch=off; an old latest.json address and the
# old value auto still work; serving off and min_sources on request
d=$(newdata 6p)
run "$d" -e CREDITS_SNAPSHOT=p2p -e CREDITS_SNAPSHOT_MIN_SOURCES=3 >/dev/null
[ -e "$d/stub-download" ] && fail "p2p downloads"
grep -q '^fetch=auto' "$d/config.ini" && grep -q '^min_sources=3' "$d/config.ini" || fail "p2p settings"
d=$(newdata 6o)
run "$d" -e CREDITS_SNAPSHOT=off -e CREDITS_SERVE_SNAPSHOT=off >/dev/null
[ -e "$d/stub-download" ] && fail "off downloads"
grep -q '^fetch=off' "$d/config.ini" && grep -q '^serve=off' "$d/config.ini" || fail "off settings"
d=$(newdata 6a)
run "$d" -e CREDITS_SNAPSHOT=auto -e CREDITS_SNAPSHOT_URL=https://example.org/rel/latest.json >/dev/null
grep -q -- '--download-snapshot https://example.org/rel/snapshot.json' "$d/stub-download" || fail "old latest.json address"
pass "p2p, off, serve and the old values"

# 6b. a full node does not download and does not fetch (a snapshot holds recent blocks only)
d=$(newdata 6f)
run "$d" -e CREDITS_STORAGE=full >/dev/null
[ -e "$d/stub-download" ] && fail "a full node downloads"
grep -q '^fetch=off' "$d/config.ini" || fail "a full node fetches"
pass "a full node syncs from block 0"

# 6c. a snapshot mismatch reported by the node: the next start is refused
d=$(newdata 6m)
run "$d" -e CREDITS_SNAPSHOT=off >/dev/null
touch "$d/snapshot.mismatch"
set +e; out=$(run "$d"); rc=$?; set -e
[ "$rc" = 4 ] || { echo "$out"; fail "start after a mismatch: exit $rc, expected 4"; }
pass "snapshot.mismatch refused"

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

# 13. backend change without CREDITS_CONVERT_DB: refused, with the hint
d=$(withdata 13)
sed -i 's/^db_backend=.*/db_backend=rocksdb/' "$d/config.ini"
set +e; out=$(run "$d"); rc=$?; set -e
[ "$rc" = 3 ] && grep -q 'CREDITS_CONVERT_DB=yes' <<<"$out" || { echo "$out"; fail "backend change hint: exit $rc"; }
pass "backend change refused with the conversion hint"

# 14. backend change with CREDITS_CONVERT_DB=yes: the node gets --convert-db, .storage follows the config
d=$(withdata 14)
sed -i 's/^db_backend=.*/db_backend=rocksdb/' "$d/config.ini"
out=$(run "$d" -e CREDITS_CONVERT_DB=yes)
grep -q -- '--convert-db' "$d/stub-args" || { echo "$out"; fail "convert: no --convert-db"; }
grep -q 'mode=pruned backend=rocksdb' "$d/.storage" || fail "convert: record"
# a later start (the node refused or paused, .storage already rewritten) still passes the flag
run "$d" -e CREDITS_CONVERT_DB=yes >/dev/null
grep -q -- '--convert-db' "$d/stub-args" || fail "convert: no --convert-db at the next start"
# with CREDITS_CONVERT_DB=yes the node always gets the flag and decides itself; without it never
d=$(withdata 15)
run "$d" -e CREDITS_CONVERT_DB=yes >/dev/null
grep -q -- '--convert-db' "$d/stub-args" || fail "no --convert-db with CREDITS_CONVERT_DB=yes and no backend change"
run "$d" >/dev/null
grep -q -- '--convert-db' "$d/stub-args" && fail "--convert-db passed without CREDITS_CONVERT_DB"
pass "CREDITS_CONVERT_DB=yes always passes --convert-db, without it never"

# 16. db/.lock alone is no data: the download still runs
d=$(newdata 16)
mkdir -p "$d/db"
touch "$d/db/.lock"
run "$d" >/dev/null
[ -e "$d/stub-download" ] || fail "no download over db/.lock alone"
pass "db/.lock alone does not count as data"

echo "PASS: entrypoint"
