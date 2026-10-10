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
#        CONTRACTS (default 0, needs FUND=1 and EXECUTOR_JAR, the contract-executor jar): every node
#        runs its own executor; contracts.py deploys two contracts, one calling the other, and calls
#        it before and after the restart test, which waits for the first calls
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
CONTRACTS=${CONTRACTS:-0}
DELEGATION_SECONDS=${DELEGATION_SECONDS:-240}
DISK_FULL_AT=${DISK_FULL_AT:-400}
DISK_FULL_SECONDS=${DISK_FULL_SECONDS:-60}
DISK_TMPFS_MB=${DISK_TMPFS_MB:-64}
WORK=$(mkdir -p "${WORK:-testnet-run}" && cd "${WORK:-testnet-run}" && pwd)
BASE_PORT=6000

declare -a PIDS KEYS

mapfile -t KEYS < <(python3 "$HERE/gen_keys.py" "$NODES" "$WORK")

if [ "$FUND" = "1" ]; then
    read -r MASTER_SEED MASTER_PUB < <(python3 -c "import base58, nacl.signing; k = nacl.signing.SigningKey.generate(); print(base58.b58encode(bytes(k)).decode(), base58.b58encode(bytes(k.verify_key)).decode())")
    # read by CREDITS_TESTNET builds only: genesis recipient and starter key of this network
    export CS_TESTNET_GENESIS_KEY=$MASTER_PUB CS_TESTNET_STARTER_KEY=$MASTER_PUB
    echo "testnet master key $MASTER_PUB"
fi

# public API on node 1 for fund.py; with CONTRACTS every node gets one (contracts.py compares their
# contract states) and its own executor, started by the node through executor.sh
api_settings() {
    local i=$1
    if [ "$CONTRACTS" != "1" ]; then
        echo "port=$([ "$FUND" = "1" ] && [ "$i" -eq 1 ] && echo 9090 || echo 0)"
        echo "apiexec_port=0"
        return
    fi
    echo "port=$([ "$i" -eq 1 ] && echo 9090 || echo $((9100 + i)))"
    echo "apiexec_port=$((9200 + i))"
    echo "executor_port=$((9300 + i))"
    echo "executor_command=$WORK/n$i/executor.sh"
    echo "executor_multi_instance=true"
    cat > "$WORK/n$i/settings.properties" <<PROPS
node.api.host=127.0.0.1
node.api.port=$([ "$i" -eq 1 ] && echo 9090 || echo $((9100 + i)))
contract.executor.port=$((9300 + i))
contract.executor.node.api.port=$((9200 + i))
contract.executor.node.api.host=127.0.0.1
contract.executor.read.client.timeout=10000
jdk.path=${JAVA_HOME:-/usr}
PROPS
    # the marker lets run.sh stop the executor of a stopped node
    printf '#!/bin/sh\ncd "$(dirname "$0")"\nexec java -Xmx256m -Dcs.testnet.node=n%s -jar "%s" >> executor.log 2>&1\n' \
        "$i" "$EXECUTOR_JAR" > "$WORK/n$i/executor.sh"
    chmod +x "$WORK/n$i/executor.sh"
}

for i in $(seq 1 "$NODES"); do
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
$(api_settings "$i")
ajax_port=0
diag_port=0

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
    for i in $(seq 1 "$NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -TERM "${PIDS[$i]}" 2>/dev/null || true
    done
    sleep 15
    for i in $(seq 1 "$NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -KILL "${PIDS[$i]}" 2>/dev/null || true
    done
    pkill -f "cs.testnet.node=" 2>/dev/null || true
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
stopped_at=0
FUND_PID=
CONTRACTS_PID=
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
    echo "$line"

    if [ "$RESTART_AT" -gt 0 ] && [ "$restarted" -eq 0 ] && [ "$min" -ge "$RESTART_AT" ] \
        && { [ "$CONTRACTS" != "1" ] || [ -e "$WORK/contracts-ready" ]; }; then
        echo "restart test: stopping n$NODES at sequence $min"
        stopped_at=$min
        kill -TERM "${PIDS[$NODES]}"
        wait "${PIDS[$NODES]}" 2>/dev/null || true
        pkill -f "cs.testnet.node=n$NODES " 2>/dev/null || true
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

    if [ "$CONTRACTS" = "1" ] && [ -z "$CONTRACTS_PID" ] && grep -q "master transactions done" "$WORK/fund.log" 2>/dev/null; then
        echo "contracts: deploying and calling them through node 1"
        (cd "$HERE" && python3 contracts.py "$WORK" "$NODES" "$MASTER_SEED" \
            --ready-file "$WORK/contracts-ready" --wait-file "$WORK/restart-done") > "$WORK/contracts.log" 2>&1 &
        CONTRACTS_PID=$!
    fi
    # the calls after the restart must run on the restarted node too, so let it catch up first
    if [ "$restarted" -eq 1 ] && [ ! -e "$WORK/restart-done" ] && [ "$(seq_of "$NODES")" -ge $((stopped_at + 30)) ] 2>/dev/null; then
        touch "$WORK/restart-done"
    fi

    if [ "$min" -ge "$TARGET_SEQ" ] && { [ "$CONTRACTS" != "1" ] || ! kill -0 "${CONTRACTS_PID:-0}" 2>/dev/null; }; then break; fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "FAIL: not every node reached sequence $TARGET_SEQ within $TIMEOUT_MIN min"
        exit 1
    fi
done

if [ -n "$CONTRACTS_PID" ]; then
    contracts_status=0
    wait "$CONTRACTS_PID" || contracts_status=$?
    cat "$WORK/contracts.log"
    for i in $(seq 1 "$NODES"); do
        echo "n$i executor: $(grep -ac . "$WORK/n$i/executor.log" 2>/dev/null || echo 0) log lines, $(grep -aci "SmartContractGet\|exception" "$WORK/n$i/node.log" 2>/dev/null || echo 0) node log lines on SmartContractGet/exceptions"
    done
    if [ "$contracts_status" -ne 0 ]; then
        echo "FAIL: contract deploys or calls failed"
        exit 1
    fi
elif [ "$CONTRACTS" = "1" ]; then
    echo "FAIL: contracts.py never started"
    exit 1
fi

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

# every node logs "STATE DIGEST #<seq> <hex> wallets <n>"; at a sequence logged by several nodes the
# digests must be identical
python3 - "$WORK" "$NODES" "$FUND" <<'PY'
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
echo "PASS: $NODES nodes reached sequence $TARGET_SEQ with matching state digests"
