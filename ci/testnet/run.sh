#!/usr/bin/env bash
# Run a private Credits network of N nodes on this machine and check that it works.
#
# Usage: run.sh <path-to-node-binary>
# Env:   NODES (default 7, keep it odd: an even set of ready nodes drops one at bootstrap)
#        TARGET_SEQ (default 1100: past the first STATE DIGEST at block 1000)
#        TIMEOUT_MIN (default 45)  RESTART_AT (default 300, 0 disables the restart test)
#        WORK (default ./testnet-run)
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
WORK=$(mkdir -p "${WORK:-testnet-run}" && cd "${WORK:-testnet-run}" && pwd)
BASE_PORT=6000

declare -a PIDS KEYS

mapfile -t KEYS < <(python3 "$HERE/gen_keys.py" "$NODES" "$WORK")

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
port=0
apiexec_port=0
ajax_port=0
diag_port=0

[Core]
Filter="%Severity% >= info"

[Sinks.file]
Destination=TextFile
FileName=node.log
AutoFlush=true
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

    if [ "$min" -ge "$TARGET_SEQ" ]; then break; fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "FAIL: not every node reached sequence $TARGET_SEQ within $TIMEOUT_MIN min"
        exit 1
    fi
done

# every node logs "STATE DIGEST #<seq> <hex> wallets <n>"; at a sequence logged by several nodes the
# digests must be identical
python3 - "$WORK" "$NODES" <<'PY'
import collections, glob, re, sys
work, nodes = sys.argv[1], int(sys.argv[2])
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
sys.exit(0 if ok else 1)
PY
echo "PASS: $NODES nodes reached sequence $TARGET_SEQ with matching state digests"
