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
port=$([ "$FUND" = "1" ] && [ "$i" -eq 1 ] && echo 9090 || echo 0)
apiexec_port=0
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
}
trap stop_all EXIT

# last written sequence a node reported (WithDelimiters prints 1'234)
seq_of() {
    grep -ah 'Last written sequence = ' "$WORK/n$1"/node.log "$WORK/n$1"/stdout.log 2>/dev/null \
        | tail -1 | sed -E "s/.*Last written sequence = ([0-9']+).*/\1/" | tr -d "'" || true
}

for i in $(seq 1 "$NODES"); do start_node "$i"; done
echo "started $NODES nodes in $WORK"

deadline=$(( $(date +%s) + TIMEOUT_MIN * 60 ))
restarted=0
FUND_PID=
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

    if [ "$RESTART_AT" -gt 0 ] && [ "$restarted" -eq 0 ] && [ "$min" -ge "$RESTART_AT" ]; then
        echo "restart test: stopping n$NODES at sequence $min"
        kill -TERM "${PIDS[$NODES]}"
        wait "${PIDS[$NODES]}" 2>/dev/null || true
        sleep 30
        start_node "$NODES"
        restarted=1
        echo "restart test: n$NODES started again"
    fi

    if [ "$FUND" = "1" ] && [ -z "$FUND_PID" ] && [ "$min" -ge "$FUND_AT" ]; then
        echo "funding nodes from the genesis key, then $LOAD_SECONDS s of transfers"
        python3 "$HERE/fund.py" "$WORK" "$NODES" "$MASTER_SEED" --load-seconds "$LOAD_SECONDS" > "$WORK/fund.log" 2>&1 &
        FUND_PID=$!
    fi

    if [ "$min" -ge "$TARGET_SEQ" ]; then break; fi
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
