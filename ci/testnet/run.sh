#!/usr/bin/env bash
# Run a private Credits network of N nodes on this machine and check that it works.
#
# Usage: run.sh <path-to-node-binary>
# Env:   NODES (default 7, keep it odd: an even set of ready nodes drops one at bootstrap)
#        TARGET_SEQ (default 1100: past the first STATE DIGEST at block 1000)
#        TIMEOUT_MIN (default 45)  RESTART_AT (default 300, 0 disables the restart test)
#        WORK (default ./testnet-run)
#        FUND (default 0; needs a -DCREDITS_TESTNET=ON binary): genesis funds go to a generated master
#        key, which funds every node through node 1's API (port 9090) once the chain reaches FUND_AT
#        (default 30) and then keeps random transfers flowing for LOAD_SECONDS (default 300)
#        DPOS_AT / REWARD_ROUND (default 0 = off, needs FUND=1): the master key, which is also the
#        starter key, moves StartingDPOS and turns block rewards on (special orders 9 and 37)
#        MIN_STAKE (default 0 = off, needs FUND=1): special order 22 sets the minimum stake; with the
#        restart test, the restarted node must apply it again after its quick start
#        DELEGATE (default 0, needs FUND=1): nodes delegate to each other, with and without a time
#        limit (DELEGATION_SECONDS, default 240), and some delegations are withdrawn later
#        PRUNE_NODE (default 0 = off): that node runs with pruned storage (a checkpoint every 1'000
#        blocks, prune_keep_blocks=300); it must remove old blocks, survive the restart test if it is
#        the restarted node, and end with the same state digest
#        JOIN_PRUNED_AT (default 0 = off): one more node, outside the initial trusted set and without
#        funds or checkpoints, starts when the network reaches that sequence; it syncs from block 0
#        with pruned storage like PRUNE_NODE and must end with the same state digest
#        DISK_FULL_NODE (default 0 = off): put that node's block DB on a DISK_TMPFS_MB (default 64) tmpfs,
#        fill it when the chain reaches DISK_FULL_AT (default 400) and free it DISK_FULL_SECONDS
#        (default 60) later, restarting the node if it stopped; it must catch up and end with the same
#        state digest (needs sudo)
#
# Checks: every node writes blocks up to TARGET_SEQ; a node stopped and restarted (quick start
# from its caches) catches up; all nodes report the same STATE DIGEST at every common sequence.
set -euo pipefail

BIN=$(realpath "$1")
HERE=$(cd "$(dirname "$0")" && pwd)
NODES=${NODES:-7}
TARGET_SEQ=${TARGET_SEQ:-1100}
TIMEOUT_MIN=${TIMEOUT_MIN:-45}
RESTART_AT=${RESTART_AT:-300}
FUND=${FUND:-0}
FUND_AT=${FUND_AT:-30}
LOAD_SECONDS=${LOAD_SECONDS:-300}
DISK_FULL_NODE=${DISK_FULL_NODE:-0}
DPOS_AT=${DPOS_AT:-0}
REWARD_ROUND=${REWARD_ROUND:-0}
DELEGATE=${DELEGATE:-0}
MIN_STAKE=${MIN_STAKE:-0}
PRUNE_NODE=${PRUNE_NODE:-0}
JOIN_PRUNED_AT=${JOIN_PRUNED_AT:-0}
# the joining node is node NODES + 1; it is in nobody's hosts or trusted list
JOIN_NODE=$([ "$JOIN_PRUNED_AT" -gt 0 ] && echo $((NODES + 1)) || echo 0)
ALL_NODES=$([ "$JOIN_NODE" -gt 0 ] && echo "$JOIN_NODE" || echo "$NODES")
DELEGATION_SECONDS=${DELEGATION_SECONDS:-240}
DISK_FULL_AT=${DISK_FULL_AT:-400}
DISK_FULL_SECONDS=${DISK_FULL_SECONDS:-60}
DISK_TMPFS_MB=${DISK_TMPFS_MB:-64}
WORK=$(mkdir -p "${WORK:-testnet-run}" && cd "${WORK:-testnet-run}" && pwd)
BASE_PORT=6000

declare -a PIDS KEYS

mapfile -t KEYS < <(python3 "$HERE/gen_keys.py" "$ALL_NODES" "$WORK")

if [ "$FUND" = "1" ]; then
    read -r MASTER_SEED MASTER_PUB < <(python3 -c "import base58, nacl.signing; k = nacl.signing.SigningKey.generate(); print(base58.b58encode(bytes(k)).decode(), base58.b58encode(bytes(k.verify_key)).decode())")
    # read by CREDITS_TESTNET builds only: genesis recipient and starter key of this network
    export CS_TESTNET_GENESIS_KEY=$MASTER_PUB CS_TESTNET_STARTER_KEY=$MASTER_PUB
    echo "testnet master key $MASTER_PUB"
fi

for i in $(seq 1 "$ALL_NODES"); do
    dir="$WORK/n$i"
    : > "$dir/hosts.txt"
    : > "$dir/trusted.txt"
    for j in $(seq 1 "$NODES"); do
        echo "${KEYS[$((j - 1))]}" >> "$dir/trusted.txt"
        [ "$j" -ne "$i" ] && echo "127.0.0.1:$((BASE_PORT + j)) ${KEYS[$((j - 1))]}" >> "$dir/hosts.txt"
    done
    cat > "$dir/config.ini" <<CFG
[params]
hosts_filename=hosts.txt
init_trusted_filename=trusted.txt
ipv6=false
traverse_nat=false
min_neighbours=1
max_neighbours=16
round_elapse_time=10000

[host_input]
ip=127.0.0.1
port=$((BASE_PORT + i))

[api]
port=$([ "$FUND" = "1" ] && [ "$i" -eq 1 ] && echo 9090 || echo 0)
apiexec_port=0
ajax_port=0
diag_port=0
$({ [ "$i" -eq "$PRUNE_NODE" ] || [ "$i" -eq "$JOIN_NODE" ]; } && printf '\n[storage]\ncheckpoint_every=1000\ncheckpoint_keep=2\nprune_keep_blocks=300\n')

[Core]
Filter="%Severity% >= info"

[Sinks.file]
Destination=TextFile
FileName=node.log
AutoFlush=true
Append=true
Format="[%TimeStamp%] %Severity% %Message%"
CFG
done

start_node() {
    local i=$1
    (cd "$WORK/n$i" && exec "$BIN" --db-path db --config-file config.ini </dev/null >> stdout.log 2>&1) &
    PIDS[$i]=$!
}

stop_all() {
    for i in $(seq 1 "$ALL_NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -TERM "${PIDS[$i]}" 2>/dev/null || true
    done
    sleep 15
    for i in $(seq 1 "$ALL_NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -KILL "${PIDS[$i]}" 2>/dev/null || true
    done
    if [ "$DISK_FULL_NODE" -gt 0 ]; then
        sudo umount "$WORK/n$DISK_FULL_NODE/db" 2>/dev/null || true
    fi
}
trap stop_all EXIT

# last written sequence a node reported (WithDelimiters prints 1'234)
seq_of() {
    grep -ah 'Last written sequence = ' "$WORK/n$1"/node.log "$WORK/n$1"/stdout.log 2>/dev/null \
        | tail -1 | sed -E "s/.*Last written sequence = ([0-9']+).*/\1/" | tr -d "'" || true
}

if [ "$DISK_FULL_NODE" -gt 0 ]; then
    mkdir -p "$WORK/n$DISK_FULL_NODE/db"
    sudo mount -t tmpfs -o "size=${DISK_TMPFS_MB}m" tmpfs "$WORK/n$DISK_FULL_NODE/db"
    sudo chown "$(id -u):$(id -g)" "$WORK/n$DISK_FULL_NODE/db"
    echo "disk-full test: n$DISK_FULL_NODE block DB on a ${DISK_TMPFS_MB} MB tmpfs"
fi

for i in $(seq 1 "$NODES"); do start_node "$i"; done
echo "started $NODES nodes in $WORK"

deadline=$(( $(date +%s) + TIMEOUT_MIN * 60 ))
restarted=0
FUND_PID=
disk_filled_at=0
disk_freed=0
while :; do
    sleep 20
    line="t=$(( TIMEOUT_MIN * 60 - (deadline - $(date +%s)) ))s"
    min=-1
    for i in $(seq 1 "$NODES"); do
        s=$(seq_of "$i"); s=${s:-0}
        line+=" n$i=$s"
        if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then line+="(down)"; fi
        if [ "$min" -lt 0 ] || [ "$s" -lt "$min" ]; then min=$s; fi
    done
    join_seq=-1
    if [ "$JOIN_NODE" -gt 0 ] && [ -n "${PIDS[$JOIN_NODE]:-}" ]; then
        join_seq=$(seq_of "$JOIN_NODE"); join_seq=${join_seq:-0}
        line+=" n$JOIN_NODE=$join_seq(joined)"
    fi
    echo "$line"

    if [ "$JOIN_NODE" -gt 0 ] && [ -z "${PIDS[$JOIN_NODE]:-}" ] && [ "$min" -ge "$JOIN_PRUNED_AT" ]; then
        echo "join test: n$JOIN_NODE starts from block 0 with pruned storage at sequence $min"
        start_node "$JOIN_NODE"
    fi

    if [ "$RESTART_AT" -gt 0 ] && [ "$restarted" -eq 0 ] && [ "$min" -ge "$RESTART_AT" ]; then
        echo "restart test: stopping n$NODES at sequence $min"
        kill -TERM "${PIDS[$NODES]}"
        wait "${PIDS[$NODES]}" 2>/dev/null || true
        sleep 30
        start_node "$NODES"
        restarted=1
        echo "restart test: n$NODES started again"
    fi

    if [ "$DISK_FULL_NODE" -gt 0 ] && [ "$disk_filled_at" -eq 0 ] && [ "$min" -ge "$DISK_FULL_AT" ]; then
        dd if=/dev/zero of="$WORK/n$DISK_FULL_NODE/db/filler" bs=1M 2>/dev/null || true
        disk_filled_at=$(date +%s)
        echo "disk-full test: n$DISK_FULL_NODE block DB filled at sequence $min ($(df -h "$WORK/n$DISK_FULL_NODE/db" | tail -1 | awk '{print $4}') left)"
    fi
    if [ "$disk_filled_at" -gt 0 ] && [ "$disk_freed" -eq 0 ] && [ "$(date +%s)" -ge $((disk_filled_at + DISK_FULL_SECONDS)) ]; then
        rm -f "$WORK/n$DISK_FULL_NODE/db/filler"
        disk_freed=1
        echo "disk-full test: n$DISK_FULL_NODE block DB freed"
        # BerkeleyDB throws on ENOSPC and the node terminates: an operator would free space and
        # restart it, so do the same and require it to catch up with the same state
        if ! kill -0 "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null; then
            echo "disk-full test: n$DISK_FULL_NODE had stopped ($(grep -ah 'terminate called\|what():' "$WORK/n$DISK_FULL_NODE"/stdout.log | tail -1)), restarting it"
            start_node "$DISK_FULL_NODE"
        fi
    fi

    if [ "$FUND" = "1" ] && [ -z "$FUND_PID" ] && [ "$min" -ge "$FUND_AT" ]; then
        echo "funding nodes from the genesis key, then $LOAD_SECONDS s of transfers"
        python3 "$HERE/fund.py" "$WORK" "$NODES" "$MASTER_SEED" --load-seconds "$LOAD_SECONDS" \
            --dpos-at "$DPOS_AT" --reward-round "$REWARD_ROUND" --delegation-seconds "$DELEGATION_SECONDS" --min-stake "$MIN_STAKE" \
            $([ "$DELEGATE" = "1" ] && echo --delegations) > "$WORK/fund.log" 2>&1 &
        FUND_PID=$!
    fi

    if [ "$min" -ge "$TARGET_SEQ" ] && { [ "$JOIN_NODE" -eq 0 ] || [ "$join_seq" -ge "$TARGET_SEQ" ]; }; then break; fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "FAIL: not every node reached sequence $TARGET_SEQ within $TIMEOUT_MIN min"
        exit 1
    fi
done

if [ -n "$FUND_PID" ]; then
    fund_status=0
    wait "$FUND_PID" || fund_status=$?
    cat "$WORK/fund.log"
    if [ "$fund_status" -ne 0 ]; then
        echo "FAIL: funding or transfers failed"
        exit 1
    fi
fi

if [ "$DISK_FULL_NODE" -gt 0 ]; then
    echo "disk-full test: n$DISK_FULL_NODE logged $(grep -ah "Couldn't save block" "$WORK/n$DISK_FULL_NODE"/*.log | wc -l) failed block saves"
fi

if [ "$MIN_STAKE" -gt 0 ] && [ "$RESTART_AT" -gt 0 ]; then
    # order 22 lands well before the restart: once applied live, once more after the quick start
    applied=$(grep -ah "MinStakeValue changed to" "$WORK/n$NODES"/*.log | wc -l)
    echo "special orders: n$NODES applied order 22 $applied time(s)"
    if [ "$applied" -lt 2 ]; then
        echo "FAIL: n$NODES lost special order 22 across its quick start"
        exit 1
    fi
fi

for p in "$PRUNE_NODE" "$JOIN_NODE"; do
    [ "$p" -gt 0 ] || continue
    echo "pruned storage: n$p logged:"
    grep -ah "pruned storage" "$WORK/n$p"/node.log | sed 's/^/  /' | tail -n 8
    # file by file, for the pruned node and a full one: block records, hash index, logs, environment
    for i in 1 "$p"; do
        echo "  n$i block DB $(du -sh "$WORK/n$i/db" 2>/dev/null | cut -f1): $(cd "$WORK/n$i/db" && du -k * 2>/dev/null | sort -k2 | awk '{printf "%s=%sK ", $2, $1}')"
        # LMDB caches: block hashes (seqdb, hashdb) and the transaction index (indexdb)
        echo "  n$i caches $(du -sh "$WORK/n$i/caches" 2>/dev/null | cut -f1): $(cd "$WORK/n$i/caches" 2>/dev/null && du -sk * 2>/dev/null | sort -k2 | awk '{printf "%s=%sK ", $2, $1}')"
    done
    if ! grep -aq "pruned storage: blocks before .* removed" "$WORK/n$p"/node.log; then
        echo "FAIL: n$p never pruned its storage"
        exit 1
    fi
done

# every node logs "STATE DIGEST #<seq> <hex> wallets <n>"; at a sequence logged by several nodes the
# digests must be identical
python3 - "$WORK" "$ALL_NODES" "$FUND" <<'PY'
import collections, glob, re, sys
work, nodes, funded = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1"
seen = collections.defaultdict(dict)
for i in range(1, nodes + 1):
    for path in glob.glob(f"{work}/n{i}/*.log"):
        for m in re.finditer(r"STATE DIGEST #(\d+) ([0-9a-fA-F]+) wallets (\d+)", open(path, errors="replace").read()):
            seen[int(m.group(1))][i] = (m.group(2).lower(), int(m.group(3)))
ok = True
for seq in sorted(seen):
    values = set(seen[seq].values())
    status = "OK" if len(values) == 1 else "MISMATCH"
    if len(values) != 1:
        ok = False
    print(f"STATE DIGEST #{seq}: {status} ({len(seen[seq])} nodes) {sorted(values)[:3]}")
if 1000 not in seen or len(seen[1000]) < nodes:
    print(f"FAIL: expected a STATE DIGEST #1000 from all {nodes} nodes, got {len(seen.get(1000, {}))}")
    ok = False
if funded and 1000 in seen and max(w for _, w in seen[1000].values()) <= 2:
    print("FAIL: funding did not change the wallet state by block 1000")
    ok = False
sys.exit(0 if ok else 1)
PY
echo "PASS: $ALL_NODES nodes reached sequence $TARGET_SEQ with matching state digests"
