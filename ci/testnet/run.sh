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
#        runs its own executor; contracts.py deploys a counter contract and calls it before and after
#        the restart test, which waits for the first calls; all nodes must end with the same state
#        EXECUTOR_JAR_ALT (optional, with CONTRACTS): the even nodes run this executor jar instead, on the
#        same JDK, so any result that differs from the release executor's fails the contract consensus
#        BIN_ALT (optional): the even nodes run this node binary instead, so a block or contract result
#        that differs from BIN's shows up as a diverging state
#        RESTART_NODES (default: the last node): the nodes the restart test stops and restarts together
#        NET_FAULT (default off): loss, corrupt, delay, reorder (with 5% duplicates), duplicate or partition, applied with
#        tc netem to the p2p traffic sent and received by NET_FAULT_NODES (comma separated) from
#        sequence NET_FAULT_AT (default 150) for NET_FAULT_SECONDS (default 0 = until the end), at
#        NET_FAULT_PCT percent (default 10; partition drops everything); those nodes must keep up
#        SLOW_NODES (comma separated, default none): those nodes get SLOW_CPU percent of one CPU (default 20)
#        GARBAGE_AT (default 0 = off): from that sequence garbage.py sends GARBAGE_RATE (default 200)
#        random datagrams a second to the nodes' p2p ports for GARBAGE_SECONDS (default 300)
#        PRUNE_NODE (default 0 = off): that node runs with pruned storage on LMDB (a checkpoint every
#        1'000 blocks, prune_keep_blocks=300, block_db=lmdb); it must remove old blocks, survive the restart test if it is
#        the restarted node, and end with the same state digest
#        NODE_STORAGE (optional): the storage of every node in order, comma separated, each
#        <berkeleydb|lmdb|rocksdb>[:pruned] (pruned: a checkpoint every 1'000 blocks and every minute,
#        prune_keep_blocks=300); the entry after the last initial node is the joining node's. It takes
#        precedence over DB_BACKEND, and every pruned node must prune
#        SNAPSHOT_AT (default 0 = off, needs PRUNE_NODE): at that sequence the pruned node is stopped,
#        started once without peers to log the digest of its head, packaged with
#        packaging/docker/snapshot-make.sh and started again; one more node (NODES + 1) restores the
#        snapshot with snapshot-restore.sh and joins; with SNAPSHOT_CORRUPT=1 the archive is truncated
#        first, so the restore must fail and that node syncs from block 0
#        STORAGE_SWITCH_AT (default 0 = off, needs PRUNE_NODE=6 and DB_BACKEND=lmdb): at that sequence
#        node 5 is restarted as a pruned node and node 6 as a full one
#        CONVERT_AT (default 0 = off, needs DB_BACKEND=berkeleydb): at that sequence n4 is converted to
#        lmdb with --convert-only and restarted, and n6 is converted to rocksdb with --convert-db, killed
#        in the middle of the conversion and started again: it must resume and finish
#        INDEX_LOSS_AT (default 0 = off): at that sequence node INDEX_LOSS_NODE (default PRUNE_NODE) is
#        stopped, its transaction index (caches/indexdb) is removed and it is started again; a pruned
#        node waits until it has pruned. It must quick-start and rebuild the index from the blocks it
#        stores (a pruned node cannot slow-start without its old blocks; a full one no longer has to)
#        HASHDB_DAMAGE_AT (default 0 = off): at that sequence node HASHDB_DAMAGE_NODE (default 3) is
#        stopped and its block hashes database caches/hashdb is damaged, HASHDB_DAMAGE=zero (default:
#        pages in the middle of data.mdb overwritten with zeros, checked with mdb_dump) or remove (the
#        folder removed); it is started again, and once more 150 blocks later if it has not rebuilt the
#        databases yet. It must rebuild them from its blocks and log no database failure afterwards
#        JOIN_PRUNED_AT (default 0 = off): one more node, outside the initial trusted set and without
#        funds or checkpoints, starts when the network reaches that sequence; it syncs from block 0
#        with pruned storage like PRUNE_NODE and must end with the same state digest
#        DB_BACKEND (default empty = the node's default, berkeleydb): db_backend of every node, or
#        "mixed" for berkeleydb, lmdb and rocksdb in turn; pruned nodes use lmdb unless it names one
#        CRASH_NODE (default 0 = off): that node is killed with SIGKILL CRASH_TIMES times (default 6)
#        from sequence CRASH_FROM (default 150) and restarted each time; it saves a checkpoint every
#        minute, and every other kill lands right after one; it must end with the same state digests
#        TERM_KILL_NODE (default 0 = off): that node is stopped as docker stop does when the node is
#        slow to stop: SIGTERM, then SIGKILL while it shuts down, TERM_KILL_TIMES times (default 4)
#        from sequence TERM_KILL_FROM (default 150). Odd kills land while the block storage closes
#        (right after STOP REQUESTED), even ones while the quick-start caches are saved; it must start
#        again each time and end with the same state digests
#        CRASH_ALL_AT (default 0 = off): every node is killed with SIGKILL at that sequence and the whole
#        network restarted, as after a power cut; the nodes must agree again
#        DISK_FULL_NODE (default 0 = off): put that node's block DB on a DISK_TMPFS_MB (default 64) tmpfs,
#        fill it when the chain reaches DISK_FULL_AT (default 400) and free it DISK_FULL_SECONDS
#        (default 60) later, restarting the node if it stopped; it must catch up and end with the same
#        state digest (needs sudo)
#
# Checks: every node writes blocks up to TARGET_SEQ; a node stopped and restarted (quick start
# from its caches) catches up; all nodes report the same STATE DIGEST at every common sequence.
set -euo pipefail

BIN=$(realpath "$1")
# mixed nodes: the even nodes run BIN_ALT, built from another ref
BIN_ALT=${BIN_ALT:+$(realpath "$BIN_ALT")}
[ -z "$BIN_ALT" ] || echo "mixed nodes: even nodes run $BIN_ALT"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NODES=${NODES:-7}
TARGET_SEQ=${TARGET_SEQ:-1100}
TIMEOUT_MIN=${TIMEOUT_MIN:-45}
RESTART_AT=${RESTART_AT:-300}
RESTART_NODES=${RESTART_NODES:-$NODES}
echo "$RESTART_NODES" | grep -qE '^[0-9]+( [0-9]+)*$' || { echo "bad RESTART_NODES '$RESTART_NODES'"; exit 1; }
FUND=${FUND:-0}
FUND_AT=${FUND_AT:-30}
LOAD_SECONDS=${LOAD_SECONDS:-300}
DISK_FULL_NODE=${DISK_FULL_NODE:-0}
DPOS_AT=${DPOS_AT:-0}
REWARD_ROUND=${REWARD_ROUND:-0}
DELEGATE=${DELEGATE:-0}
MIN_STAKE=${MIN_STAKE:-0}
CONTRACTS=${CONTRACTS:-0}
PRUNE_NODE=${PRUNE_NODE:-0}
NODE_STORAGE=${NODE_STORAGE:-}
DB_BACKEND=${DB_BACKEND:-}
CRASH_NODE=${CRASH_NODE:-0}
CRASH_TIMES=${CRASH_TIMES:-6}
CRASH_FROM=${CRASH_FROM:-150}
TERM_KILL_NODE=${TERM_KILL_NODE:-0}
TERM_KILL_TIMES=${TERM_KILL_TIMES:-4}
TERM_KILL_FROM=${TERM_KILL_FROM:-150}
CRASH_ALL_AT=${CRASH_ALL_AT:-0}
JOIN_PRUNED_AT=${JOIN_PRUNED_AT:-0}
# the joining node is node NODES + 1; it is in nobody's hosts or trusted list
SNAPSHOT_AT=${SNAPSHOT_AT:-0}
SNAPSHOT_CORRUPT=${SNAPSHOT_CORRUPT:-0}
STORAGE_SWITCH_AT=${STORAGE_SWITCH_AT:-0}
CONVERT_AT=${CONVERT_AT:-0}
INDEX_LOSS_AT=${INDEX_LOSS_AT:-0}
INDEX_LOSS_NODE=${INDEX_LOSS_NODE:-$PRUNE_NODE}
HASHDB_DAMAGE_AT=${HASHDB_DAMAGE_AT:-0}
HASHDB_DAMAGE_NODE=${HASHDB_DAMAGE_NODE:-3}
HASHDB_DAMAGE=${HASHDB_DAMAGE:-zero}
MDB_COPY=${MDB_COPY:-$(cd "$(dirname "$0")/../.." && pwd)/third-party/lmdb/libraries/liblmdb/mdb_copy}
JOIN_NODE=$({ [ "$JOIN_PRUNED_AT" -gt 0 ] || [ "$SNAPSHOT_AT" -gt 0 ]; } && echo $((NODES + 1)) || echo 0)
ALL_NODES=$([ "$JOIN_NODE" -gt 0 ] && echo "$JOIN_NODE" || echo "$NODES")
declare -a STORE_BACKEND STORE_PRUNED
if [ -n "$NODE_STORAGE" ]; then
    IFS=, read -r -a entries <<<"$NODE_STORAGE"
    [ "${#entries[@]}" -eq "$ALL_NODES" ] || { echo "NODE_STORAGE needs $ALL_NODES entries, has ${#entries[@]}"; exit 1; }
    for i in $(seq 1 "$ALL_NODES"); do
        e=${entries[$((i - 1))]}
        [[ "$e" =~ ^(berkeleydb|lmdb|rocksdb)(:pruned)?$ ]] || { echo "bad NODE_STORAGE entry '$e'"; exit 1; }
        STORE_BACKEND[$i]=${e%%:*}
        STORE_PRUNED[$i]=$([ "$e" != "${e%:pruned}" ] && echo 1 || echo 0)
    done
fi
# every node with pruned storage: the PRUNE_NODE, the joining node, and those of NODE_STORAGE
PRUNED_NODES=$(for i in $(seq 1 "$ALL_NODES"); do
    if [ "$i" -eq "$PRUNE_NODE" ] || [ "$i" -eq "$JOIN_NODE" ] || [ "${STORE_PRUNED[$i]:-0}" = 1 ]; then echo "$i"; fi
done | tr '\n' ' ')
DELEGATION_SECONDS=${DELEGATION_SECONDS:-240}
DISK_FULL_AT=${DISK_FULL_AT:-400}
DISK_FULL_SECONDS=${DISK_FULL_SECONDS:-60}
DISK_TMPFS_MB=${DISK_TMPFS_MB:-64}
NET_FAULT=${NET_FAULT:-}
NET_FAULT_NODES=${NET_FAULT_NODES:-}
NET_FAULT_AT=${NET_FAULT_AT:-150}
NET_FAULT_SECONDS=${NET_FAULT_SECONDS:-0}
NET_FAULT_PCT=${NET_FAULT_PCT:-10}
SLOW_NODES=${SLOW_NODES:-}
SLOW_CPU=${SLOW_CPU:-20}
GARBAGE_AT=${GARBAGE_AT:-0}
GARBAGE_SECONDS=${GARBAGE_SECONDS:-300}
GARBAGE_RATE=${GARBAGE_RATE:-200}
for v in "$NET_FAULT_NODES" "$SLOW_NODES"; do
    echo "$v" | grep -qE '^([0-9]+(,[0-9]+)*)?$' || { echo "bad node list '$v'"; exit 1; }
done
case "$NET_FAULT" in
    "") NETEM="" ;;
    loss) NETEM="loss ${NET_FAULT_PCT}%" ;;
    corrupt) NETEM="corrupt ${NET_FAULT_PCT}%" ;;
    delay) NETEM="delay 250ms 150ms distribution normal" ;;
    reorder) NETEM="delay 40ms reorder ${NET_FAULT_PCT}% 50% duplicate 5%" ;;
    duplicate) NETEM="duplicate ${NET_FAULT_PCT}%" ;;
    partition) NETEM="loss 100%" ;;
    *) echo "bad NET_FAULT '$NET_FAULT'"; exit 1 ;;
esac
[ -z "$NET_FAULT" ] || [ -n "$NET_FAULT_NODES" ] || { echo "NET_FAULT needs NET_FAULT_NODES"; exit 1; }
WORK=$(mkdir -p "${WORK:-testnet-run}" && cd "${WORK:-testnet-run}" && pwd)
BASE_PORT=6000

declare -a PIDS KEYS REPORTED_DOWN

mapfile -t KEYS < <(python3 "$HERE/gen_keys.py" "$ALL_NODES" "$WORK")

if [ "$FUND" = "1" ]; then
    read -r MASTER_SEED MASTER_PUB < <(python3 -c "import base58, nacl.signing; k = nacl.signing.SigningKey.generate(); print(base58.b58encode(bytes(k)).decode(), base58.b58encode(bytes(k.verify_key)).decode())")
    # read by CREDITS_TESTNET builds only: genesis recipient and starter key of this network
    export CS_TESTNET_GENESIS_KEY=$MASTER_PUB CS_TESTNET_STARTER_KEY=$MASTER_PUB
    echo "testnet master key $MASTER_PUB"
fi

# the release ships its executor with the JDK it runs on (ojdkbuild 11.0.4); jdk.path must be a JDK,
# the executor compiles contracts with it
if [ "$CONTRACTS" = "1" ]; then
    EXECUTOR_JDK=$(find "$(dirname "$EXECUTOR_JAR")" -maxdepth 2 -type d -name 'java-11-openjdk-*' | head -1)
    EXECUTOR_JDK=${EXECUTOR_JDK:-${JAVA_HOME:-/usr}}
    echo "contract executor $EXECUTOR_JAR on $EXECUTOR_JDK"
    # the executor compiles contracts in-process with this JDK's javac and its own class path, but a
    # compile error reaches the API only as a dropped connection, so compile them here first
    mkdir -p "$WORK/contract-sources"
    (cd "$HERE" && python3 -c "import contracts, sys; contracts.write_sources(sys.argv[1])" "$WORK/contract-sources")
    for jar in "$EXECUTOR_JAR" ${EXECUTOR_JAR_ALT:+"$EXECUTOR_JAR_ALT"}; do
        if ! "$EXECUTOR_JDK/bin/javac" -parameters -cp "$jar" -d "$WORK/contract-sources" "$WORK"/contract-sources/*.java; then
            echo "FAIL: the test contracts do not compile against the class path of $jar"
            exit 1
        fi
    done
    echo "test contracts compile against the executor's class path"
    [ -z "${EXECUTOR_JAR_ALT:-}" ] || echo "mixed executors: even nodes run $EXECUTOR_JAR_ALT"
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
    # the first contract compile in a fresh JVM takes longer than the 4 s default
    echo "executor_send_timeout=60000"
    echo "executor_receive_timeout=60000"
    cat > "$WORK/n$i/settings.properties" <<PROPS
node.api.host=127.0.0.1
node.api.port=$([ "$i" -eq 1 ] && echo 9090 || echo $((9100 + i)))
contract.executor.port=$((9300 + i))
contract.executor.node.api.port=$((9200 + i))
contract.executor.node.api.host=127.0.0.1
contract.executor.read.client.timeout=0
jdk.path=$EXECUTOR_JDK
PROPS
    local jar=$EXECUTOR_JAR
    if [ -n "${EXECUTOR_JAR_ALT:-}" ] && [ $((i % 2)) -eq 0 ]; then jar=$EXECUTOR_JAR_ALT; fi
    # the marker lets run.sh stop the executor of a stopped node
    printf '#!/bin/sh\ncd "$(dirname "$0")"\nexec "%s" -Xmx384m -XX:MaxMetaspaceSize=256m -Dcs.testnet.node=n%s -jar "%s" >> executor.log 2>&1\n' \
        "$EXECUTOR_JDK/bin/java" "$i" "$jar" > "$WORK/n$i/executor.sh"
    chmod +x "$WORK/n$i/executor.sh"
}

# the [storage] section of node i: pruning, backend and checkpoint frequency
storage_section() {
    local i=$1 backend="" lines=""
    case "$DB_BACKEND" in
        "") ;;
        mixed) backend=$(echo berkeleydb lmdb rocksdb | cut -d' ' -f$(( (i - 1) % 3 + 1 ))) ;;
        *) backend=$DB_BACKEND ;;
    esac
    if [ -n "$NODE_STORAGE" ]; then
        backend=${STORE_BACKEND[$i]}
    fi
    if [[ " $PRUNED_NODES " == *" $i "* ]]; then
        [ -z "$NODE_STORAGE" ] && { [ -z "$backend" ] || [ "$DB_BACKEND" = mixed ]; } && backend=lmdb
        # pruning follows the periodic checkpoints only: it starts at #2000; a checkpoint a minute as
        # well, so that kills of a pruned node land right after one
        lines="checkpoint_every=1000\ncheckpoint_every_minutes=1\ncheckpoint_keep=2\nprune_keep_blocks=300\n"
    elif [ "$i" -eq "$CRASH_NODE" ]; then
        # frequent checkpoints (checkpoint_every is at least 1000 blocks, so one a minute as well), so
        # that the kills hit the moments right after one
        lines="checkpoint_every=1000\ncheckpoint_every_minutes=1\ncheckpoint_keep=3\n"
    fi
    [ -n "$backend" ] && lines="${lines}db_backend=$backend\n"
    [ -n "$lines" ] && printf "\n[storage]\n$lines"
    return 0
}

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
$(api_settings "$i")
ajax_port=0
diag_port=0
$(storage_section "$i")

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
    # core dumps (see core_pattern in the workflow) let a crash be traced afterwards
    local bin=$BIN
    if [ -n "$BIN_ALT" ] && [ $((i % 2)) -eq 0 ]; then bin=$BIN_ALT; fi
    local extra_var="EXTRA_ARGS_$i"
    (cd "$WORK/n$i" && ulimit -c unlimited && exec "$bin" --db-path db --config-file config.ini ${!extra_var:-} </dev/null >> stdout.log 2>&1) &
    PIDS[$i]=$!
    # a slow or faulty node runs in its own cgroup: a CPU quota for a slow one, and its packets are
    # marked for the network fault (its executor, started later, inherits the cgroup)
    if [[ ",$SLOW_NODES,$NET_FAULT_NODES," == *",$i,"* ]]; then
        sudo mkdir -p "/sys/fs/cgroup/cstest-$i"
        if [[ ",$SLOW_NODES," == *",$i,"* ]]; then
            echo "$((SLOW_CPU * 1000)) 100000" | sudo tee "/sys/fs/cgroup/cstest-$i/cpu.max" > /dev/null
        fi
        echo "${PIDS[$i]}" | sudo tee "/sys/fs/cgroup/cstest-$i/cgroup.procs" > /dev/null \
            || echo "could not move n$i to its cgroup"
    fi
}

# tc netem on the loopback: a 4th band of a prio qdisc holds the fault. Everything a faulty node
# sends (marked by its cgroup) goes there, and so does everything sent to its p2p port (UDP and the
# TCP connections other nodes open to it)
net_fault_on() {
    sudo tc qdisc add dev lo root handle 1: prio bands 4 priomap 1 2 2 2 1 2 0 0 1 1 1 1 1 1 1 1
    sudo tc qdisc add dev lo parent 1:4 handle 40: netem $NETEM
    sudo tc filter add dev lo parent 1:0 protocol ip prio 1 handle 0x40 fw flowid 1:4
    for i in ${NET_FAULT_NODES//,/ }; do
        sudo iptables -t mangle -A OUTPUT -m cgroup --path "cstest-$i" -j MARK --set-mark 0x40 \
            || echo "net fault: cannot mark the packets of n$i (no xt_cgroup), only those sent to it"
        sudo tc filter add dev lo parent 1:0 protocol ip prio 2 u32 \
            match ip dport $((BASE_PORT + i)) 0xffff flowid 1:4
    done
}
net_fault_off() {
    sudo tc qdisc del dev lo root 2>/dev/null || true
    sudo iptables -t mangle -F OUTPUT 2>/dev/null || true
}
net_fault_stats() {
    sudo tc -s qdisc show dev lo 2>/dev/null | grep -A1 netem | tail -1 | sed 's/^ *//'
}

stop_all() {
    for i in $(seq 1 "$ALL_NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -TERM "${PIDS[$i]}" 2>/dev/null || true
    done
    sleep 15
    for i in $(seq 1 "$ALL_NODES"); do
        [ -n "${PIDS[$i]:-}" ] && kill -KILL "${PIDS[$i]}" 2>/dev/null || true
    done
    pkill -f "cs.testnet.node=" 2>/dev/null || true
    if [ "$DISK_FULL_NODE" -gt 0 ]; then
        sudo umount "$WORK/n$DISK_FULL_NODE/db" 2>/dev/null || true
    fi
    for i in ${SLOW_NODES//,/ }; do
        echo "slow node n$i: $(tr '\n' ' ' < "/sys/fs/cgroup/cstest-$i/cpu.stat" 2>/dev/null || echo 'no cgroup')"
    done
    if [ "${net_fault_at:-0}" -gt 0 ] && [ "${net_fault_done:-1}" -eq 0 ]; then
        echo "net fault: until the end ($(net_fault_stats))"
    fi
    [ -z "$NET_FAULT" ] || net_fault_off
    [ -z "${GARBAGE_PID:-}" ] || kill "$GARBAGE_PID" 2>/dev/null || true
}
trap stop_all EXIT

# last written sequence a node reported (WithDelimiters prints 1'234)
seq_of() {
    grep -ah 'Last written sequence = ' "$WORK/n$1"/node.log "$WORK/n$1"/stdout.log 2>/dev/null \
        | tail -1 | sed -E "s/.*Last written sequence = ([0-9']+).*/\1/" | tr -d "'" || true
}

# stop node i cleanly, start it once without peers (another port, no hosts) so that its start-up
# STATE DIGEST is the digest of its head, and stop it again
digest_offline() {
    local i=$1 dir="$WORK/n$1" before
    kill -TERM "${PIDS[$i]}"; wait "${PIDS[$i]}" 2>/dev/null || true
    pkill -f "cs.testnet.node=n$i " 2>/dev/null || true
    # an empty hosts file stops the node ("No hosts specified"): one host nobody listens on
    echo "127.0.0.1:9 11111111111111111111111111111111" > "$dir/hosts-none.txt"
    awk -v port=$((BASE_PORT + 500 + i)) '
        /^\[/ { section = $0 }
        /^hosts_filename=/ { print "hosts_filename=hosts-none.txt"; next }
        section == "[host_input]" && /^port=/ { print "port=" port; next }
        { print }' "$dir/config.ini" > "$dir/config-offline.ini"
    before=$(grep -ac 'STATE DIGEST #' "$dir/node.log" || true)
    (cd "$dir" && exec "$BIN" --db-path db --config-file config-offline.ini </dev/null >> stdout.log 2>&1) &
    local pid=$! deadline=$(( $(date +%s) + 300 ))
    while [ "$(grep -ac 'STATE DIGEST #' "$dir/node.log" || true)" -le "${before:-0}" ] && [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 2
    done
    kill -TERM "$pid"; wait "$pid" 2>/dev/null || true
    if [ "$(grep -ac 'STATE DIGEST #' "$dir/node.log" || true)" -le "${before:-0}" ]; then
        echo "FAIL: n$i logged no start-up digest without peers" >&2
        return 1
    fi
    grep -a 'STATE DIGEST #' "$dir/node.log" | tail -1 | sed 's/^.*STATE DIGEST/STATE DIGEST/'
}

# the lowest sequence among the nodes of the restart test
restarted_seq() {
    local low="" s
    for i in $RESTART_NODES; do
        s=$(seq_of "$i"); s=${s:-0}
        if [ -z "$low" ] || [ "$s" -lt "$low" ]; then low=$s; fi
    done
    echo "${low:-0}"
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
crashes=0
term_kills=0
term_kill_next=0
term_kill_during_save=0
crash_wait_qs=""
crash_next=0
crashed_all=0
FUND_PID=
CONTRACTS_PID=
disk_filled_at=0
disk_freed=0
net_fault_at=0
net_fault_done=0
snapshot_done=0
storage_switched=0
GARBAGE_PID=
if [ -n "$SLOW_NODES" ]; then
    echo "+cpu" | sudo tee /sys/fs/cgroup/cgroup.subtree_control > /dev/null 2>&1 || true
fi
while :; do
    sleep 20
    line="t=$(( TIMEOUT_MIN * 60 - (deadline - $(date +%s)) ))s"
    min=-1
    for i in $(seq 1 "$NODES"); do
        s=$(seq_of "$i"); s=${s:-0}
        line+=" n$i=$s"
        if ! kill -0 "${PIDS[$i]}" 2>/dev/null; then
            line+="(down)"
            if [ -z "${REPORTED_DOWN[$i]:-}" ]; then
                REPORTED_DOWN[$i]=1
                status=0; wait "${PIDS[$i]}" 2>/dev/null || status=$?
                echo "n$i exited with status $status$([ "$status" -gt 128 ] && echo " (signal $((status - 128)))")"
                # the run may end on its timeout later, so trace a crash right away
                for core in "$WORK"/core.*."${PIDS[$i]}"; do
                    [ -f "$core" ] || continue
                    echo "n$i core dump $core:"
                    gdb -q -batch -ex "bt 60" -ex "thread apply all bt 6" "$BIN" "$core" 2>/dev/null | grep -E '^(#|Thread)' | head -n 200
                    mv "$core" "$core.traced"
                done
            fi
        fi
        if [ "$min" -lt 0 ] || [ "$s" -lt "$min" ]; then min=$s; fi
    done
    join_seq=-1
    if [ "$JOIN_NODE" -gt 0 ] && [ -n "${PIDS[$JOIN_NODE]:-}" ]; then
        join_seq=$(seq_of "$JOIN_NODE"); join_seq=${join_seq:-0}
        line+=" n$JOIN_NODE=$join_seq(joined)"
    fi
    echo "$line"

    if [ "$JOIN_PRUNED_AT" -gt 0 ] && [ -z "${PIDS[$JOIN_NODE]:-}" ] && [ "$min" -ge "$JOIN_PRUNED_AT" ]; then
        echo "join test: n$JOIN_NODE starts from block 0 with pruned storage at sequence $min"
        start_node "$JOIN_NODE"
    fi

    # repeated SIGKILL of one node: odd kills right after it saves a checkpoint, even ones at any moment
    if [ "$CRASH_NODE" -gt 0 ] && [ "$crashes" -lt "$CRASH_TIMES" ] && [ "$min" -ge "$CRASH_FROM" ] \
        && [ "$(date +%s)" -ge "$crash_next" ]; then
        newest=$(ls -t "$WORK/n$CRASH_NODE/qs" 2>/dev/null | head -1)
        if [ $((crashes % 2)) -eq 1 ] && [ -z "$crash_wait_qs" ]; then
            crash_wait_qs=${newest:-none}
        fi
        if [ $((crashes % 2)) -eq 0 ] || { [ -n "$newest" ] && [ "$newest" != "$crash_wait_qs" ]; }; then
            [ $((crashes % 2)) -eq 1 ] && sleep $((RANDOM % 3))
            crashes=$((crashes + 1))
            echo "crash test: SIGKILL n$CRASH_NODE (kill $crashes of $CRASH_TIMES) at sequence $(seq_of "$CRASH_NODE"), newest checkpoint ${newest:-none}"
            kill -KILL "${PIDS[$CRASH_NODE]}" 2>/dev/null || true
            wait "${PIDS[$CRASH_NODE]}" 2>/dev/null || true
            sleep 5
            start_node "$CRASH_NODE"
            crash_wait_qs=""
            crash_next=$(( $(date +%s) + 40 + RANDOM % 60 ))
        fi
    fi

    # the whole network killed at once, as after a power cut
    if [ "$CRASH_ALL_AT" -gt 0 ] && [ "$crashed_all" -eq 0 ] && [ "$min" -ge "$CRASH_ALL_AT" ]; then
        echo "crash test: SIGKILL every node at sequence $min"
        for i in $(seq 1 "$ALL_NODES"); do
            [ -n "${PIDS[$i]:-}" ] && kill -KILL "${PIDS[$i]}" 2>/dev/null || true
        done
        for i in $(seq 1 "$ALL_NODES"); do
            [ -n "${PIDS[$i]:-}" ] && wait "${PIDS[$i]}" 2>/dev/null || true
        done
        sleep 10
        for i in $(seq 1 "$NODES"); do start_node "$i"; done
        crashed_all=1
        echo "crash test: network restarted"
    fi

    if [ "$RESTART_AT" -gt 0 ] && [ "$restarted" -eq 0 ] && [ "$min" -ge "$RESTART_AT" ] \
        && { [ "$CONTRACTS" != "1" ] || [ -e "$WORK/contracts-ready" ]; }; then
        echo "restart test: stopping $(printf 'n%s ' $RESTART_NODES)at sequence $min"
        stopped_at=$min
        for i in $RESTART_NODES; do kill -TERM "${PIDS[$i]}"; done
        for i in $RESTART_NODES; do
            wait "${PIDS[$i]}" 2>/dev/null || true
            pkill -f "cs.testnet.node=n$i " 2>/dev/null || true
        done
        sleep 30
        for i in $RESTART_NODES; do start_node "$i"; done
        restarted=1
        echo "restart test: $(printf 'n%s ' $RESTART_NODES)started again"
    fi

    if [ -n "$NET_FAULT" ] && [ "$net_fault_at" -eq 0 ] && [ "$min" -ge "$NET_FAULT_AT" ]; then
        net_fault_on
        net_fault_at=$(date +%s)
        echo "net fault: $NETEM on the p2p traffic of n${NET_FAULT_NODES//,/ n} from sequence $min"
    fi
    if [ "$net_fault_at" -gt 0 ] && [ "$net_fault_done" -eq 0 ] && [ "$NET_FAULT_SECONDS" -gt 0 ] \
        && [ "$(date +%s)" -ge $((net_fault_at + NET_FAULT_SECONDS)) ]; then
        echo "net fault: lifted at sequence $min ($(net_fault_stats))"
        net_fault_off
        net_fault_done=1
    fi
    if [ "$GARBAGE_AT" -gt 0 ] && [ -z "$GARBAGE_PID" ] && [ "$min" -ge "$GARBAGE_AT" ]; then
        echo "garbage: $GARBAGE_RATE random datagrams a second to every node for $GARBAGE_SECONDS s, from sequence $min"
        python3 "$HERE/garbage.py" "$BASE_PORT" "$ALL_NODES" "$GARBAGE_SECONDS" "$GARBAGE_RATE" > "$WORK/garbage.log" 2>&1 &
        GARBAGE_PID=$!
    fi

    if [ "$SNAPSHOT_AT" -gt 0 ] && [ "$snapshot_done" -eq 0 ] && [ "$min" -ge "$SNAPSHOT_AT" ]; then
        snapshot_done=1
        head_digest=$(digest_offline "$PRUNE_NODE") || exit 1
        echo "snapshot test: n$PRUNE_NODE stopped, head $head_digest"
        "$REPO/packaging/docker/snapshot-make.sh" "$WORK/n$PRUNE_NODE" "$WORK/snapshot" "$MDB_COPY" local \
            | sed 's/^/snapshot test: /'
        start_node "$PRUNE_NODE"
        if [ "$SNAPSHOT_CORRUPT" = 1 ]; then
            f=$(ls "$WORK"/snapshot/credits-pruned-*.tar.zst)
            head -c $(( $(stat -c %s "$f") / 2 )) "$f" > "$f.cut" && mv "$f.cut" "$f"
            echo "snapshot test: archive truncated"
        fi
        rc=0
        "$REPO/packaging/docker/snapshot-restore.sh" "$WORK/n$JOIN_NODE" "$WORK/snapshot/latest.json" \
            > "$WORK/restore.log" 2>&1 || rc=$?
        sed 's/^/snapshot test: /' "$WORK/restore.log"
        echo "snapshot test: restore exit $rc; n$JOIN_NODE joins at sequence $min"
        if [ "$SNAPSHOT_CORRUPT" = 1 ] && [ "$rc" != 2 ]; then echo "FAIL: a truncated snapshot was restored"; exit 1; fi
        if [ "$SNAPSHOT_CORRUPT" != 1 ] && [ "$rc" != 0 ]; then echo "FAIL: the snapshot was not restored"; exit 1; fi
        start_node "$JOIN_NODE"
    fi

    if [ "$STORAGE_SWITCH_AT" -gt 0 ] && [ "$storage_switched" -eq 0 ] && [ "$min" -ge "$STORAGE_SWITCH_AT" ]; then
        storage_switched=1
        pruned_before=$(grep -a 'pruned storage: blocks before .* removed' "$WORK/n6"/node.log | tail -1 | sed -E "s/.*blocks before ([0-9']+) removed.*/\1/" | tr -d "'")
        for i in 5 6; do kill -TERM "${PIDS[$i]}"; done
        for i in 5 6; do wait "${PIDS[$i]}" 2>/dev/null || true; done
        sed -i '/^db_backend=/a checkpoint_every=1000\ncheckpoint_keep=2\nprune_keep_blocks=300' "$WORK/n5/config.ini"
        sed -i 's/^prune_keep_blocks=.*/prune_keep_blocks=0/' "$WORK/n6/config.ini"
        switch_lines5=$(wc -l < "$WORK/n5/node.log")
        switch_lines6=$(wc -l < "$WORK/n6/node.log")
        for i in 5 6; do start_node "$i"; done
        echo "storage switch: n5 full -> pruned, n6 pruned -> full at sequence $min (n6 had pruned up to ${pruned_before:-none})"
    fi

    if [ "$CONVERT_AT" -gt 0 ] && [ "${converted:-0}" -eq 0 ] && [ "$min" -ge "$CONVERT_AT" ]; then
        converted=1
        for i in 4 6; do kill -TERM "${PIDS[$i]}"; done
        for i in 4 6; do wait "${PIDS[$i]}" 2>/dev/null || true; done
        sed -i 's/^db_backend=.*/db_backend=lmdb/' "$WORK/n4/config.ini"
        sed -i 's/^db_backend=.*/db_backend=rocksdb/' "$WORK/n6/config.ini"
        t0=$(date +%s)
        (cd "$WORK/n4" && "$BIN" --db-path db --config-file config.ini --convert-only --convert-batch 200 </dev/null >> stdout.log 2>&1) \
            || { echo "FAIL: n4 conversion to lmdb failed"; tail -20 "$WORK/n4/stdout.log"; exit 1; }
        echo "convert test: n4 berkeleydb -> lmdb at sequence $min in $(( $(date +%s) - t0 )) s"
        # node.log is appended across restarts: remember where the start after the conversion begins
        convert_lines4=$(wc -l < "$WORK/n4/node.log")
        start_node 4
        # a small batch for n6, so that many batches remain after the second one
        (cd "$WORK/n6" && exec "$BIN" --db-path db --config-file config.ini --convert-db --convert-batch 50 </dev/null >> stdout.log 2>&1) &
        cpid=$!
        until_t=$(( $(date +%s) + 300 ))
        until [ "$(grep -ac 'converted blocks' "$WORK/n6/stdout.log")" -ge 2 ] || ! kill -0 "$cpid" 2>/dev/null; do
            [ "$(date +%s)" -lt "$until_t" ] || { kill -KILL "$cpid" 2>/dev/null || true; echo "FAIL: n6 did not log two converted batches within 300 s"; tail -20 "$WORK/n6/stdout.log"; exit 1; }
            sleep 0.2
        done
        kill -KILL "$cpid" 2>/dev/null || true; wait "$cpid" 2>/dev/null || true
        if [ ! -f "$WORK/n6/db/.convert" ]; then
            if grep -aq 'conversion done' "$WORK/n6/stdout.log"; then
                echo "FAIL: n6 finished its conversion before the kill, lower the batch"
            else
                echo "FAIL: n6 was killed but has no db/.convert to resume"
            fi
            exit 1
        fi
        convert_lines6=$(wc -l < "$WORK/n6/node.log")
        echo "convert test: n6 killed during its conversion to rocksdb, restarting it"
        # the extra arguments are a temporary of this call only: a later restart of n6 does not convert again
        EXTRA_ARGS_6="--convert-db --convert-batch 50" start_node 6
    fi

    if [ "$INDEX_LOSS_AT" -gt 0 ] && [ "${index_lost:-0}" -eq 0 ] && [ "$min" -ge "$INDEX_LOSS_AT" ] \
        && { [ "$INDEX_LOSS_NODE" != "$PRUNE_NODE" ] \
             || grep -aq 'pruned storage: blocks before .* removed' "$WORK/n$INDEX_LOSS_NODE/node.log"; }; then
        index_lost=1
        kill -TERM "${PIDS[$INDEX_LOSS_NODE]}"; wait "${PIDS[$INDEX_LOSS_NODE]}" 2>/dev/null || true
        pkill -f "cs.testnet.node=n$INDEX_LOSS_NODE " 2>/dev/null || true
        rm -rf "$WORK/n$INDEX_LOSS_NODE/caches/indexdb"
        index_lines=$(wc -l < "$WORK/n$INDEX_LOSS_NODE/node.log")
        start_node "$INDEX_LOSS_NODE"
        echo "index loss test: n$INDEX_LOSS_NODE restarted without its transaction index at sequence $min"
    fi

    if [ "$HASHDB_DAMAGE_AT" -gt 0 ] && [ "${hashdb_damaged:-0}" -eq 0 ] && [ "$min" -ge "$HASHDB_DAMAGE_AT" ]; then
        hashdb_damaged=1
        hd=$HASHDB_DAMAGE_NODE
        kill -TERM "${PIDS[$hd]}"; wait "${PIDS[$hd]}" 2>/dev/null || true
        pkill -f "cs.testnet.node=n$hd " 2>/dev/null || true
        if [ "$HASHDB_DAMAGE" = remove ]; then
            rm -rf "$WORK/n$hd/caches/hashdb"
        else
            # zeros over the pages after the two meta pages, up to the end of the data written so far
            pages=$(( $(du -B4096 "$WORK/n$hd/caches/hashdb/data.mdb" | cut -f1) ))
            [ "$pages" -gt 4 ] || { echo "FAIL: n$hd hashdb has only $pages pages"; exit 1; }
            dd if=/dev/zero of="$WORK/n$hd/caches/hashdb/data.mdb" bs=4096 seek=2 count=$(( pages - 2 )) conv=notrunc 2>/dev/null
            if "$(dirname "$MDB_COPY")/mdb_dump" "$WORK/n$hd/caches/hashdb" >/dev/null 2>&1; then
                echo "FAIL: n$hd hashdb still reads after the damage"; exit 1
            fi
        fi
        hashdb_lines=$(wc -l < "$WORK/n$hd/node.log")
        hashdb_restart_at=$((min + 150))
        start_node "$hd"
        echo "hashdb test: n$hd restarted with its block hashes database damaged ($HASHDB_DAMAGE) at sequence $min"
    fi
    if [ "${hashdb_damaged:-0}" -eq 1 ] && [ -n "${hashdb_restart_at:-}" ] && [ "$min" -ge "$hashdb_restart_at" ]; then
        hashdb_restart_at=""
        if ! tail -n +"$((hashdb_lines + 1))" "$WORK/n$HASHDB_DAMAGE_NODE/node.log" | grep -aq 'blockHashes rebuilt from the stored blocks'; then
            kill -TERM "${PIDS[$HASHDB_DAMAGE_NODE]}"; wait "${PIDS[$HASHDB_DAMAGE_NODE]}" 2>/dev/null || true
            pkill -f "cs.testnet.node=n$HASHDB_DAMAGE_NODE " 2>/dev/null || true
            start_node "$HASHDB_DAMAGE_NODE"
            echo "hashdb test: n$HASHDB_DAMAGE_NODE restarted once more at sequence $min, it had not rebuilt yet"
        fi
    fi

    # SIGTERM, then SIGKILL in the middle of the shutdown, as docker stop after its timeout
    if [ "$TERM_KILL_NODE" -gt 0 ] && [ "$term_kills" -lt "$TERM_KILL_TIMES" ] && [ "$min" -ge "$TERM_KILL_FROM" ] \
        && [ "$(date +%s)" -ge "$term_kill_next" ]; then
        term_kills=$((term_kills + 1))
        tk_k=$TERM_KILL_NODE
        tk_log="$WORK/n$tk_k/node.log"
        if [ $((term_kills % 2)) -eq 1 ]; then tk_mark='STOP REQUESTED'; else tk_mark='try to save caches for QUICK START'; fi
        tk_before_mark=$(grep -ac "$tk_mark" "$tk_log" || true)
        tk_before_saved=$(grep -ac 'caches for QUICK START saved' "$tk_log" || true)
        tk_at=$(seq_of "$tk_k")
        kill -TERM "${PIDS[$tk_k]}" 2>/dev/null || true
        tk_deadline=$(( $(date +%s) + 120 ))
        while [ "$(grep -ac "$tk_mark" "$tk_log" || true)" -le "${tk_before_mark:-0}" ] && kill -0 "${PIDS[$tk_k]}" 2>/dev/null \
            && [ "$(date +%s)" -lt "$tk_deadline" ]; do
            sleep 0.02
        done
        # odd kills: a little later, so that the storage is closing; even ones: at once
        [ $((term_kills % 2)) -eq 1 ] && sleep "0.$((RANDOM % 5))"
        if kill -0 "${PIDS[$tk_k]}" 2>/dev/null; then
            kill -KILL "${PIDS[$tk_k]}" 2>/dev/null || true
            if [ "$(grep -ac 'caches for QUICK START saved' "$tk_log" || true)" -le "${tk_before_saved:-0}" ]; then
                term_kill_during_save=$((term_kill_during_save + 1))
                tk_landed="before the caches were saved"
            else
                tk_landed="after the caches were saved"
            fi
        else
            tk_landed="too late: the node had already exited"
        fi
        wait "${PIDS[$tk_k]}" 2>/dev/null || true
        pkill -KILL -f "cs.testnet.node=n$tk_k " 2>/dev/null || true
        echo "term-kill test: SIGTERM then SIGKILL n$tk_k (kill $term_kills of $TERM_KILL_TIMES, after \"$tk_mark\") at sequence $tk_at, $tk_landed"
        sleep 5
        start_node "$tk_k"
        term_kill_next=$(( $(date +%s) + 60 + RANDOM % 40 ))
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
    if [ "$restarted" -eq 1 ] && [ ! -e "$WORK/restart-done" ] && [ "$(restarted_seq)" -ge $((stopped_at + 30)) ] 2>/dev/null; then
        touch "$WORK/restart-done"
    fi

    if [ "$min" -ge "$TARGET_SEQ" ] && { [ "$JOIN_NODE" -eq 0 ] || [ "$join_seq" -ge "$TARGET_SEQ" ]; } \
        && { [ "$CONTRACTS" != "1" ] || ! kill -0 "${CONTRACTS_PID:-0}" 2>/dev/null; }; then break; fi
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
        echo "n$i executor: $(cat "$WORK/n$i"/log/*.txt 2>/dev/null | grep -ac . || true) log lines, $(grep -aci "SmartContractGet\|exception" "$WORK/n$i/node.log" 2>/dev/null || true) node log lines on SmartContractGet/exceptions"
    done
    if [ "$contracts_status" -ne 0 ]; then
        # the executor logs to log/executor-log-<day>.txt in its working directory (its logback.xml)
        tail -n 40 "$WORK"/n1/log/*.txt 2>/dev/null || true
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

if [ "$CRASH_NODE" -gt 0 ]; then
    echo "crash test: n$CRASH_NODE was killed $crashes time(s); it skipped $(grep -ah 'is not on this chain' "$WORK/n$CRASH_NODE"/*.log | wc -l) checkpoint(s) as not on the chain and logged $(grep -ah "Couldn't save block" "$WORK/n$CRASH_NODE"/*.log | wc -l) failed block saves"
    if [ "$crashes" -lt "$CRASH_TIMES" ]; then
        echo "FAIL: only $crashes of $CRASH_TIMES kills happened before the target sequence"
        exit 1
    fi
fi
if [ -n "$DB_BACKEND" ] || [ -n "$NODE_STORAGE" ]; then
    for i in $(seq 1 "$ALL_NODES"); do
        used=$(grep -ah 'block database: ' "$WORK/n$i"/*.log | tail -1 | sed 's/.*block database: //' || true)
        echo "n$i block database: $used"
        if [ -n "$NODE_STORAGE" ] && [ "${used%% *}" != "${STORE_BACKEND[$i]}" ]; then
            echo "FAIL: n$i was to run on ${STORE_BACKEND[$i]}"; exit 1
        fi
    done
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

# a crashed node left a core file: print where it stopped
for core in "$WORK"/core.* "$WORK"/n*/core*; do
    [ -f "$core" ] || continue
    echo "core dump $core:"
    gdb -q -batch -ex "bt 40" "$BIN" "$core" 2>/dev/null | grep -E '^#' | head -n 40
done

for p in $PRUNED_NODES; do
    echo "pruned storage: n$p logged:"
    { grep -ah "pruned storage" "$WORK/n$p"/node.log || true; } | sed 's/^/  /' | tail -n 8
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

# checkpoints of this build hold their last sequence: the correction for earlier builds stays unused
for i in $(seq 1 "$ALL_NODES"); do
    n=$(cat "$WORK/n$i"/*.log | grep -ac "but its caches already hold block" || true)
    if [ "${n:-0}" -gt 0 ]; then echo "n$i loaded $n checkpoint(s) one block behind their caches"; fi
done
if [ "$SNAPSHOT_AT" -gt 0 ]; then
    if [ "$SNAPSHOT_CORRUPT" = 1 ]; then
        grep -aq 'SLOW START' "$WORK/n$JOIN_NODE"/node.log || { echo "FAIL: n$JOIN_NODE did not sync from block 0"; exit 1; }
        echo "snapshot test: n$JOIN_NODE synced from block 0 after the truncated snapshot"
    else
        grep -aq 'Caches for QUICK START loaded successfully' "$WORK/n$JOIN_NODE"/node.log \
            || { echo "FAIL: n$JOIN_NODE did not start from the snapshot"; exit 1; }
        read -r snap_seq snap_digest < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["seq"], m["state_digest"])' "$WORK/n$JOIN_NODE/snapshot.json")
        [[ "$snap_seq" =~ ^[0-9]+$ && "$snap_digest" =~ ^[0-9A-Fa-f]{64}$ ]] || { echo "FAIL: bad snapshot.json"; exit 1; }
        # the start-up digest: a checkpoint saved at shutdown labels it one block higher (S + 1)
        got=$(grep -a -m1 -E "STATE DIGEST #($snap_seq|$((snap_seq + 1))) " "$WORK/n$JOIN_NODE"/node.log | sed -E 's/.*STATE DIGEST #[0-9]+ ([0-9A-Fa-f]+).*/\1/' || true)
        [ "${got,,}" = "${snap_digest,,}" ] || { echo "FAIL: n$JOIN_NODE start-up digest ${got:-none} differs from the snapshot's $snap_digest"; exit 1; }
        echo "snapshot test: n$JOIN_NODE started from the snapshot at block $snap_seq with its digest $got"
    fi
fi
if [ "$STORAGE_SWITCH_AT" -gt 0 ]; then
    # no "tail | grep -q" under pipefail: grep -q stops at the first match and tail fails on SIGPIPE
    after5=$(tail -n +"$((switch_lines5 + 1))" "$WORK/n5/node.log")
    grep -aq 'pruned storage: blocks before .* removed' <<<"$after5" \
        || { echo "FAIL: n5 did not prune after switching to pruned storage"; exit 1; }
    # n6, now full, must still start from the block its last pruning left, and prune no more
    after6=$(tail -n +"$((switch_lines6 + 1))" "$WORK/n6/node.log")
    oldest_after=$(grep -a -m1 'oldest block stored is' <<<"$after6" | sed -E 's/.*stored is //' | tr -d "'" || true)
    [ -n "$pruned_before" ] && [ "$oldest_after" = "$pruned_before" ] \
        || { echo "FAIL: n6 had pruned up to ${pruned_before:-none}, its oldest block after the switch is ${oldest_after:-none}"; exit 1; }
    ! grep -aq 'pruned storage: blocks before .* removed' <<<"$after6" \
        || { echo "FAIL: n6 kept pruning as a full node"; exit 1; }
    echo "storage switch: n5 pruned after the switch; n6, now full, keeps its oldest block $oldest_after and prunes no more"
fi
if [ "$CONVERT_AT" -gt 0 ]; then
    for i in 4 6; do
        want=$([ "$i" = 4 ] && echo lmdb || echo rocksdb)
        [ "$(cat "$WORK/n$i/db/.backend" 2>/dev/null)" = "$want" ] || { echo "FAIL: n$i db/.backend is not $want"; exit 1; }
        [ ! -e "$WORK/n$i/db/.convert" ] || { echo "FAIL: n$i still has db/.convert"; exit 1; }
        [ ! -e "$WORK/n$i/db/blockchain.db" ] || { echo "FAIL: n$i still has its BerkeleyDB files"; exit 1; }
        grep -aq 'conversion done' "$WORK/n$i/stdout.log" || { echo "FAIL: n$i did not finish its conversion"; exit 1; }
    done
    grep -aq 'resuming the conversion' "$WORK/n6/stdout.log" || { echo "FAIL: n6 did not resume its conversion"; exit 1; }
    for i in 4 6; do
        # QUICK START! goes to node.log (the file sink), not stdout.log; only the part since the restart
        lines_var="convert_lines$i"
        after=$(tail -n +"$(( ${!lines_var} + 1 ))" "$WORK/n$i/node.log")
        grep -aq 'QUICK START!' <<<"$after" || { echo "FAIL: n$i did not quick-start after the conversion"; exit 1; }
    done
    echo "convert test: n4 now lmdb, n6 now rocksdb (resumed after a kill), both quick-started"
fi
if [ "$TERM_KILL_NODE" -gt 0 ]; then
    [ "$term_kills" -eq "$TERM_KILL_TIMES" ] || { echo "FAIL: only $term_kills of $TERM_KILL_TIMES term-kills ran"; exit 1; }
    [ "$term_kill_during_save" -gt 0 ] || { echo "FAIL: no SIGKILL landed before n$TERM_KILL_NODE saved its caches"; exit 1; }
    echo "term-kill test: n$TERM_KILL_NODE was killed $term_kills time(s) while stopping, $term_kill_during_save before its caches were saved"
fi
if [ "$HASHDB_DAMAGE_AT" -gt 0 ]; then
    [ "${hashdb_damaged:-0}" = 1 ] || { echo "FAIL: the hashdb test did not run"; exit 1; }
    after=$(tail -n +"$((hashdb_lines + 1))" "$WORK/n$HASHDB_DAMAGE_NODE/node.log")
    grep -aq 'blockHashes rebuilt from the stored blocks' <<<"$after" \
        || { echo "FAIL: n$HASHDB_DAMAGE_NODE did not rebuild its block hashes databases"; exit 1; }
    since=$(awk '/blockHashes rebuilt from the stored blocks/{found=NR} END{print found}' <<<"$after")
    failures=$(tail -n +"$((since + 1))" <<<"$after" | grep -ac 'block hashes database exception' || true)
    [ "${failures:-0}" -eq 0 ] || { echo "FAIL: n$HASHDB_DAMAGE_NODE logged $failures block hashes failures after the rebuild"; exit 1; }
    echo "hashdb test: n$HASHDB_DAMAGE_NODE $(grep -a 'blockHashes rebuilt from the stored blocks' <<<"$after" | tail -1 | sed 's/.*blockHashes/blockHashes/'), no failure since"
fi
if [ "$INDEX_LOSS_AT" -gt 0 ]; then
    [ "${index_lost:-0}" = 1 ] || { echo "FAIL: the index loss test did not run on n$INDEX_LOSS_NODE"; exit 1; }
    after=$(tail -n +"$((index_lines + 1))" "$WORK/n$INDEX_LOSS_NODE/node.log")
    grep -aq 'QUICK START!' <<<"$after" || { echo "FAIL: n$INDEX_LOSS_NODE did not quick-start without its index"; exit 1; }
    grep -aq 'trxIndex rebuilt from the stored blocks' <<<"$after" \
        || { echo "FAIL: n$INDEX_LOSS_NODE did not rebuild its transaction index"; exit 1; }
    echo "index loss test: n$INDEX_LOSS_NODE quick-started and rebuilt its index: $(grep -a -m1 'trxIndex rebuilt from the stored blocks' <<<"$after" | sed 's/.*trxIndex/trxIndex/')"
fi
# every node started from an empty db/: the start-up check must have stayed silent there (it prints
# "block database:" only on a backend mismatch, a resume or --convert-only); n4 and n6 of the
# convert test are checked up to their conversion only
for i in $(seq 1 "$ALL_NODES"); do
    first_start=$(awk '/conversion done|resuming the conversion/{exit} {print}' "$WORK/n$i/stdout.log")
    if [ "$CONVERT_AT" -eq 0 ] || { [ "$i" != 4 ] && [ "$i" != 6 ]; }; then
        ! grep -aq 'block database:' <<<"$first_start" || { echo "FAIL: n$i printed a block database message on a normal start"; exit 1; }
    fi
done

# the round statistics of a node that applied a block twice no longer match its blockchain
for i in $(seq 1 "$ALL_NODES"); do
    n=$(cat "$WORK/n$i"/*.log | grep -ac "counted transactions is different" || true)
    if [ "${n:-0}" -gt 0 ]; then echo "n$i logged \"counted transactions is different\" $n time(s)"; fi
done

# every node logs "STATE DIGEST #<seq> <hex> wallets <n>"; at a sequence logged by several nodes the
# digests must be identical
python3 - "$WORK" "$ALL_NODES" "$FUND" "$NODES" "$SNAPSHOT_AT" <<'PY'
import collections, glob, re, sys
work, nodes, funded = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1"
# a node that joins from a snapshot starts past block 1000: only the initial nodes log it
initial = int(sys.argv[4]) if int(sys.argv[5]) > 0 else nodes
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
if 1000 not in seen or len(seen[1000]) < initial:
    print(f"FAIL: expected a STATE DIGEST #1000 from all {initial} nodes, got {len(seen.get(1000, {}))}")
    ok = False
if funded and 1000 in seen and max(w for _, w in seen[1000].values()) <= 2:
    print("FAIL: funding did not change the wallet state by block 1000")
    ok = False
sys.exit(0 if ok else 1)
PY
echo "PASS: $ALL_NODES nodes reached sequence $TARGET_SEQ with matching state digests"
