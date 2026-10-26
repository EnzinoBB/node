#!/usr/bin/env bash
# Run a private Credits network of N nodes on this machine and check that it works.
#
# Usage: run.sh <path-to-node-binary>
# Env:   NODES (default 7; keep the initial confidants, TRUSTED_NODES, odd: an even set of ready ones
#        drops one at bootstrap)
#        TARGET_SEQ (default 1100: past the first STATE DIGEST at block 1000)
#        TIMEOUT_MIN (default 45)  RESTART_AT (default 300, 0 disables the restart test)
#        WORK (default ./testnet-run)
#        TRUSTED_NODES (default NODES): only the first TRUSTED_NODES nodes are initial confidants
#        (odd, 5 to 25); the others, up to 61 nodes in all, are ordinary nodes
#        ROUND_ELAPSE_MS (default 20000): round_elapse_time of every node
#        SAMPLE_SECONDS (default 60, 0 = off): sampler.py records host and per-process memory and CPU
#        of the nodes and executors in resources.tsv, and prints a summary at the end
#        FUND (default 0; needs a -DCREDITS_TESTNET=ON binary): genesis funds go to a generated master
#        key, which funds every node through node 1's API (port 9090) once the chain reaches FUND_AT
#        (default 30) and then keeps random transfers flowing for LOAD_SECONDS (default 300)
#        DPOS_AT / REWARD_ROUND (default 0 = off, needs FUND=1): the master key, which is also the
#        starter key, moves StartingDPOS and turns block rewards on (special orders 9 and 37)
#        SYNCRO_ROUND (default 0 = off, needs FUND=1): special order 35 makes the sync answers count
#        from that round (Consensus::syncroChangeRound), as on a network that issued it
#        JOIN_FULL=1: the node of JOIN_PRUNED_AT syncs from block 0 as a full node instead
#        LOW_STAKE_NODES (default 0, needs FUND=1): the last that many nodes get 1'000 CS, below the
#        minimum stake; after StartingDPOS (DPOS_AT) none of them may be trusted
#        MIN_STAKE (default 0 = off, needs FUND=1): special order 22 sets the minimum stake; with the
#        restart test, the restarted node must apply it again after its quick start
#        DELEGATE (default 0, needs FUND=1): nodes delegate to each other, with and without a time
#        limit (DELEGATION_SECONDS, default 240), and some delegations are withdrawn later
#        CONTRACTS (default 0, needs FUND=1 and EXECUTOR_JAR, the contract-executor jar): every node
#        runs its own executor; contracts.py deploys a counter contract and calls it before and after
#        the restart test, which waits for the first calls; all nodes must end with the same state.
#        A power cut (CRASH_ALL_AT) also waits for the first calls. The calls after go once the
#        restarted nodes, and the network after a power cut, are 30 blocks past it, and the lowest
#        node reached CONTRACTS_AFTER_AT (default 0); the states are compared once every node,
#        a joining one too, reached TARGET_SEQ. contracts.py sends everything through n1: a
#        contract scenario never kills, partitions or slows n1
#        SYNC_CONTRACT_EXECUTION=0: every node runs with sync_contract_execution=false (no contract
#        re-execution while reading the blockchain; states fetched from the confidants once live)
#        CONTRACT_DICE=1 (with CONTRACTS): also a Dice contract that reads its own balance, played three
#        times before the restart test; a node that syncs through the plays re-executes them
#        CONTRACT_LOAD_SECONDS (default 0, with CONTRACTS): after the calls, a load phase of that many
#        seconds: CONTRACT_WRITE_RATE (default 1) Counter.next writes a second from every node's
#        account through its own API, and CONTRACT_READ_RATE (default 5) Counter.get reads (getters,
#        no transaction) a second over every node; latencies in contracts-stats.json
#        EXECUTOR_JAR_ALT (optional, with CONTRACTS): the even nodes run this executor jar instead, on the
#        same JDK, so any result that differs from the release executor's fails the contract consensus
#        BIN_ALT (optional): the even nodes run this node binary instead, so a block or contract result
#        that differs from BIN's shows up as a diverging state
#        BIN_ALT_OFFICIAL=1: BIN_ALT is the official node (ci/official/testnet-hooks.patch), which logs
#        no state digest: every node opens its API and the even nodes must hold the same blocks as n1
#        (hash at every 100th sequence up to the lowest head); the digests are compared among the others.
#        The official node runs BerkeleyDB whatever DB_BACKEND says, and a scenario that needs on an
#        even node a feature only this fork has (pruning, snapshots, conversion, damage and failure
#        injection, NODE_STORAGE other than berkeleydb, SYNC_CONTRACT_EXECUTION=0) is refused at once.
#        A fork of an official node shows only as a block hash that differs (it logs no fork line), and
#        an official node restarted after DPOS_AT goes back to StartingDPOS 10000 (it does not keep
#        order 9 across a quick start): official behaviour, so restarts after DPOS_AT are refused for
#        even nodes
#        RESTART_NODES (default: the last initial confidant): the nodes the restart test stops and restarts together
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
#        --export-snapshot (format 2) and started again; one more node (NODES + 1) restores the
#        snapshot with --import-snapshot into its own backend and joins; with SNAPSHOT_CORRUPT=1 the
#        blocks file is truncated first, so the restore must fail and that node syncs from block 0.
#        SNAPSHOT_FORMAT=1: packaging/docker/snapshot-make.sh and snapshot-restore.sh instead
#        SNAPSHOT_SERVE_NODES (optional, comma separated): those nodes run with [snapshot] serve=on
#        and a checkpoint every 1'000 blocks; each must build the snapshot of a checkpoint and serve
#        it; at the end one other node of this build is stopped and its known_peers.json must list
#        each of them with the snapshot capability, and no other node with it. Every node gets
#        [snapshot] fetch=off (a network that starts from block 0 has no snapshot to fetch), except
#        the joining node with JOIN_FETCH=p2p
#        JOIN_FETCH=p2p (needs JOIN_PRUNED_AT): the joining node starts on an empty database with
#        [snapshot] fetch=auto and gets its snapshot from the serving nodes over p2p: min_sources
#        JOIN_MIN_SOURCES (default 3), fetch_wait JOIN_WAIT (default 300), fetch_fallback
#        JOIN_FALLBACK (default sync). Every node then listens on its own loopback /24 (127.0.<i>.1,
#        NODE_NETS=1), so that the sources count as independent. It must agree, fetch, import, verify
#        the digest of the snapshot at its quick start and reach the target
#        JOIN_EXPECT=fallback: too few sources on purpose; the joiner gives up after fetch_wait and
#        syncs from block 0. JOIN_FETCH_RATE: the joiner's fetch_rate_kbps (KB/s), to slow a fetch down
#        SNAPSHOT_LATE_SERVE_NODES (comma separated, odd): checkpoints as a serving node but serve=off;
#        JOIN_LATE_SERVE_AFTER (default 90) s after the joiner started they are stopped for 40 s (so
#        that the peers' routing tables forget them) and started again with serve=on
#        JOIN_SOURCES=keys: the joiner trusts the serving nodes by key ([snapshot] sources=) and every
#        node stays on 127.0.0.1 (one /24: only keys can make up min_sources); JOIN_SOURCES=file: the
#        serving and late nodes are listed in the joiner's snapshot_sources.txt instead (not trusted)
#        JOIN_KILL_DURING_FETCH=1: the joiner is killed once when it logs "fetching block" and once
#        when it logs "importing block", and started again each time; it must clean up and join
#        FORGE_NODE / CORRUPT_NODE (odd, a CREDITS_TESTNET build): that serving node declares a forged
#        state digest / flips a byte of every third piece it serves
#        STORAGE_SWITCH_AT (default 0 = off, needs PRUNE_NODE=6 and DB_BACKEND=lmdb): at that sequence
#        node 5 is restarted as a pruned node and node 6 as a full one
#        CONVERT_AT (default 0 = off, needs DB_BACKEND=berkeleydb): at that sequence n4 is converted to
#        lmdb with --convert-only and restarted, and n6 is converted to rocksdb with --convert-db, killed
#        after its second batch and again with every block converted (testnet-convert-kill), its
#        BerkeleyDB files damaged, and started again: it must resume midway, then finish without them
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
#        with pruned storage like PRUNE_NODE and must end with the same state digest (JOIN_FETCH=p2p:
#        it starts from a snapshot fetched from its neighbours instead)
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
#        CHECKPOINT_AHEAD_AT (default 0 = off): at that sequence node CHECKPOINT_AHEAD_NODE (default 3,
#        a checkpoint a minute, 3 kept) is killed, started once offline with --set-bc-top a few blocks
#        before its second newest checkpoint, and killed again: its two newest checkpoints are then
#        ahead of the blocks on disk. It is restarted on every exit, at most 5 times: it must set
#        those checkpoints aside, quick-start from the older one and end with the same state digests
#        APPLY_FAIL_AT (default 0 = off, a CREDITS_TESTNET build): at that sequence node
#        APPLY_FAIL_NODE (default 3) is told to fail applying a block 20 blocks later to its caches
#        (./testnet-fail-apply); it must stop within 120 s, saving no checkpoint, and after a restart
#        catch up with the same state digests, never logging "Pool already present"
#        API_LOAD_NODE (default 0 = off): that node opens its public API (port 9100 + node) and, from
#        sequence API_LOAD_FROM (default 1500), api_load.py reads random blocks among the last 1,500
#        through it with 4 threads until the end, so that API threads read blocks while the node
#        prunes; the node must keep up, and the load must have made at least 1,000 calls
#        DISK_FULL_NODE (default 0 = off): put that node's block DB on a DISK_TMPFS_MB (default 64) tmpfs,
#        fill it when the chain reaches DISK_FULL_AT (default 400) and free it DISK_FULL_SECONDS
#        (default 60) later, restarting the node if it stopped; it must catch up and end with the same
#        state digest (needs sudo). With DISK_FULL_STOP=1 the node gets SIGTERM halfway through the
#        full-disk period and must exit within 120 s, as an operator would stop it
#
# Checks: every node writes blocks up to TARGET_SEQ; a node stopped and restarted (quick start
# from its caches) catches up; all nodes report the same STATE DIGEST at every common sequence.
set -euo pipefail

BIN=$(realpath "$1")
# mixed nodes: the even nodes run BIN_ALT, built from another ref
BIN_ALT=${BIN_ALT:+$(realpath "$BIN_ALT")}
[ -z "$BIN_ALT" ] || echo "mixed nodes: even nodes run $BIN_ALT"
BIN_ALT_OFFICIAL=${BIN_ALT_OFFICIAL:-0}
[ -n "$BIN_ALT" ] || BIN_ALT_OFFICIAL=0
[ "$BIN_ALT_OFFICIAL" != 1 ] || echo "mixed nodes: the even nodes are official nodes"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
NODES=${NODES:-7}
# the initial confidants (trusted.txt): the first TRUSTED_NODES nodes, by default all of them; the
# others are ordinary nodes, as most nodes on mainnet. MinTrustedNodes is 4, MaxTrustedNodes 25, and
# an even set of ready confidants drops one at bootstrap
TRUSTED_NODES=${TRUSTED_NODES:-$NODES}
case "$TRUSTED_NODES" in
    5|7|9|11|13|15|17|19|21|23|25) ;;
    *) echo "TRUSTED_NODES must be odd, 5 to 25, got '$TRUSTED_NODES'"; exit 1 ;;
esac
# ports: p2p 6000 + i (offline copies 6500 + i), API 9100 + i, apiexec 9200 + i, executor 9300 + i
[ "$NODES" -ge "$TRUSTED_NODES" ] && [ "$NODES" -le 61 ] \
    || { echo "NODES must be TRUSTED_NODES ($TRUSTED_NODES) to 61, got '$NODES'"; exit 1; }
# 25 is Consensus::MaxTrustedNodes: no larger set of initial confidants can bootstrap. Above 15 the neighbour map (16) is not a full mesh, but initial confidants are permanent
# neighbours of each other and consensus messages are routed, so that does not stop the network
CONTRACT_LOAD_SECONDS=${CONTRACT_LOAD_SECONDS:-0}
CONTRACT_WRITE_RATE=${CONTRACT_WRITE_RATE:-1}
CONTRACT_READ_RATE=${CONTRACT_READ_RATE:-5}
SAMPLE_SECONDS=${SAMPLE_SECONDS:-60}
# a node starts a bootstrap when its round lasts this long. The consensus's own fallback for a
# silent trusted node takes about 12.5 s, so below that a slow round is pre-empted by a bootstrap,
# and a bootstrap of a full table re-sent every 10 s never completes (official stage-1 hash race):
# 20 s by default (mainnet default 60 s); bootstrap-livelock keeps 10 s to reproduce the race
ROUND_ELAPSE_MS=${ROUND_ELAPSE_MS:-20000}
TARGET_SEQ=${TARGET_SEQ:-1100}
TIMEOUT_MIN=${TIMEOUT_MIN:-45}
RESTART_AT=${RESTART_AT:-300}
# by default the last initial confidant (the last node when every node is one)
RESTART_NODES=${RESTART_NODES:-$TRUSTED_NODES}
echo "$RESTART_NODES" | grep -qE '^[0-9]+( [0-9]+)*$' || { echo "bad RESTART_NODES '$RESTART_NODES'"; exit 1; }
FUND=${FUND:-0}
FUND_AT=${FUND_AT:-30}
LOAD_SECONDS=${LOAD_SECONDS:-300}
DISK_FULL_NODE=${DISK_FULL_NODE:-0}
CHECKPOINT_AHEAD_AT=${CHECKPOINT_AHEAD_AT:-0}
CHECKPOINT_AHEAD_NODE=${CHECKPOINT_AHEAD_NODE:-3}
APPLY_FAIL_AT=${APPLY_FAIL_AT:-0}
APPLY_FAIL_NODE=${APPLY_FAIL_NODE:-3}
API_LOAD_NODE=${API_LOAD_NODE:-0}
API_LOAD_FROM=${API_LOAD_FROM:-1500}
DPOS_AT=${DPOS_AT:-0}
REWARD_ROUND=${REWARD_ROUND:-0}
DELEGATE=${DELEGATE:-0}
MIN_STAKE=${MIN_STAKE:-0}
# the last LOW_STAKE_NODES nodes (never initial confidants) get 1'000 CS, below the minimum stake
LOW_STAKE_NODES=${LOW_STAKE_NODES:-0}
[ "$LOW_STAKE_NODES" -le $((NODES - TRUSTED_NODES)) ] \
    || { echo "LOW_STAKE_NODES must be at most NODES - TRUSTED_NODES ($((NODES - TRUSTED_NODES)))"; exit 1; }
# stake counts only from StartingDPOS: the check needs it well before the target
[ "$LOW_STAKE_NODES" -eq 0 ] || { [ "$DPOS_AT" -gt 0 ] && [ $((DPOS_AT + 300)) -le "$TARGET_SEQ" ]; } \
    || { echo "LOW_STAKE_NODES needs DPOS_AT, at most TARGET_SEQ - 300"; exit 1; }
SYNCRO_ROUND=${SYNCRO_ROUND:-0}
JOIN_FULL=${JOIN_FULL:-0}
CONTRACTS=${CONTRACTS:-0}
CONTRACTS_AFTER_AT=${CONTRACTS_AFTER_AT:-0}
CONTRACT_DICE=${CONTRACT_DICE:-0}
SYNC_CONTRACT_EXECUTION=${SYNC_CONTRACT_EXECUTION:-1}
[ "$CONTRACTS_AFTER_AT" -eq 0 ] || [ "$CONTRACTS_AFTER_AT" -le $(( ${TARGET_SEQ:-1100} - 100 )) ] \
    || { echo "CONTRACTS_AFTER_AT must be at most TARGET_SEQ - 100"; exit 1; }
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
SNAPSHOT_FORMAT=${SNAPSHOT_FORMAT:-2}
SNAPSHOT_CORRUPT=${SNAPSHOT_CORRUPT:-0}
# SNAPSHOT_VIA=download (format 2): the joining node runs --download-snapshot against a local web
# server laid out as the GitHub release (snapshot.json, s<seq>-<name>) instead of --import-snapshot
SNAPSHOT_VIA=${SNAPSHOT_VIA:-import}
case "$SNAPSHOT_VIA" in import|download) ;; *) echo "SNAPSHOT_VIA must be import or download, got '$SNAPSHOT_VIA'"; exit 1 ;; esac
SNAPSHOT_SERVE_NODES=${SNAPSHOT_SERVE_NODES:-}
if [ -n "$BIN_ALT" ]; then
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do
        # the even nodes run BIN_ALT, which may not serve snapshots
        [ $((i % 2)) -eq 1 ] || { echo "FAIL: SNAPSHOT_SERVE_NODES names n$i, an even node, which runs BIN_ALT"; exit 1; }
    done
fi
JOIN_FETCH=${JOIN_FETCH:-}
JOIN_MIN_SOURCES=${JOIN_MIN_SOURCES:-3}
JOIN_WAIT=${JOIN_WAIT:-300}
JOIN_FALLBACK=${JOIN_FALLBACK:-sync}
# JOIN_EXPECT=fallback: too few sources on purpose; the joiner must give up after fetch_wait and sync from block 0
JOIN_EXPECT=${JOIN_EXPECT:-snapshot}
JOIN_FETCH_RATE=${JOIN_FETCH_RATE:-0}
SNAPSHOT_LATE_SERVE_NODES=${SNAPSHOT_LATE_SERVE_NODES:-}
JOIN_LATE_SERVE_AFTER=${JOIN_LATE_SERVE_AFTER:-90}
JOIN_SOURCES=${JOIN_SOURCES:-}
case "$JOIN_SOURCES" in ""|keys|file) ;; *) echo "JOIN_SOURCES must be keys, file or empty, got '$JOIN_SOURCES'"; exit 1 ;; esac
JOIN_KILL_DURING_FETCH=${JOIN_KILL_DURING_FETCH:-0}
FORGE_NODE=${FORGE_NODE:-0}
CORRUPT_NODE=${CORRUPT_NODE:-0}
for i in ${SNAPSHOT_LATE_SERVE_NODES//,/ } $FORGE_NODE $CORRUPT_NODE; do
    [ "$i" -eq 0 ] || [ $((i % 2)) -eq 1 ] || [ -z "$BIN_ALT" ] || { echo "FAIL: n$i is even and runs BIN_ALT: it cannot serve, forge or corrupt snapshots"; exit 1; }
done
case "$JOIN_EXPECT" in snapshot|fallback) ;; *) echo "JOIN_EXPECT must be snapshot or fallback, got '$JOIN_EXPECT'"; exit 1 ;; esac
case "$JOIN_FETCH" in ""|p2p) ;; *) echo "JOIN_FETCH must be p2p or empty, got '$JOIN_FETCH'"; exit 1 ;; esac
case "$JOIN_FALLBACK" in sync|wait) ;; *) echo "JOIN_FALLBACK must be sync or wait, got '$JOIN_FALLBACK'"; exit 1 ;; esac
if [ "$JOIN_FETCH" = p2p ]; then
    [ "$JOIN_PRUNED_AT" -gt 0 ] && [ "${SNAPSHOT_AT:-0}" -eq 0 ] \
        || { echo "JOIN_FETCH=p2p needs JOIN_PRUNED_AT and no SNAPSHOT_AT"; exit 1; }
fi
# one loopback /24 per node (127.0.<i>.1): the agreement counts sources by /24. The p2p library
# binds its UDP socket to every interface whatever [host_input] ip says, so a datagram to another
# node leaves from 127.0.0.1: node_nets_on rewrites its source to the sender's address (SNAT by the
# sender's p2p port, needs sudo), which is the address the receiver's routing table records
NODE_NETS=${NODE_NETS:-$([ "$JOIN_FETCH" = p2p ] && [ "$JOIN_SOURCES" != keys ] && echo 1 || echo 0)}
node_ip() { if [ "$NODE_NETS" = 1 ]; then echo "127.0.$1.1"; else echo 127.0.0.1; fi; }
STORAGE_SWITCH_AT=${STORAGE_SWITCH_AT:-0}
CONVERT_AT=${CONVERT_AT:-0}
INDEX_LOSS_AT=${INDEX_LOSS_AT:-0}
INDEX_LOSS_NODE=${INDEX_LOSS_NODE:-$PRUNE_NODE}
HASHDB_DAMAGE_AT=${HASHDB_DAMAGE_AT:-0}
HASHDB_DAMAGE_NODE=${HASHDB_DAMAGE_NODE:-3}
HASHDB_DAMAGE=${HASHDB_DAMAGE:-zero}
MDB_COPY=${MDB_COPY:-$REPO/third-party/lmdb/libraries/liblmdb/mdb_copy}
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
    if [ "$i" -eq "$PRUNE_NODE" ] || { [ "$i" -eq "$JOIN_NODE" ] && [ "$JOIN_FULL" != 1 ]; } || [ "${STORE_PRUNED[$i]:-0}" = 1 ]; then echo "$i"; fi
done | tr '\n' ' ')
DELEGATION_SECONDS=${DELEGATION_SECONDS:-240}
DISK_FULL_AT=${DISK_FULL_AT:-400}
DISK_FULL_SECONDS=${DISK_FULL_SECONDS:-60}
DISK_FULL_STOP=${DISK_FULL_STOP:-0}
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

# official even nodes: refuse what they cannot take part in (see BIN_ALT_OFFICIAL above)
official_node() { [ "$BIN_ALT_OFFICIAL" = 1 ] && [ $(( $1 % 2 )) -eq 0 ]; }
if [ "$BIN_ALT_OFFICIAL" = 1 ]; then
    refuse() { echo "FAIL: $1 needs a feature the official node lacks (BIN_ALT_OFFICIAL=1: the even nodes are official)"; exit 1; }
    for p in $PRUNED_NODES; do ! official_node "$p" || refuse "pruned storage on n$p"; done
    [ "$JOIN_NODE" -eq 0 ] || [ "$SNAPSHOT_AT" -eq 0 ] || ! official_node "$JOIN_NODE" || refuse "a snapshot import into n$JOIN_NODE"
    [ "$JOIN_FETCH" != p2p ] || ! official_node "$JOIN_NODE" || refuse "a p2p snapshot join into n$JOIN_NODE"
    [ "$SNAPSHOT_AT" -eq 0 ] || [ "$PRUNE_NODE" -eq 0 ] || ! official_node "$PRUNE_NODE" || refuse "a snapshot export from n$PRUNE_NODE"
    [ "$STORAGE_SWITCH_AT" -eq 0 ] || refuse "STORAGE_SWITCH_AT"
    [ "$CONVERT_AT" -eq 0 ] || refuse "CONVERT_AT"
    [ "$INDEX_LOSS_AT" -eq 0 ] || ! official_node "$INDEX_LOSS_NODE" || refuse "INDEX_LOSS on n$INDEX_LOSS_NODE"
    [ "$HASHDB_DAMAGE_AT" -eq 0 ] || ! official_node "$HASHDB_DAMAGE_NODE" || refuse "HASHDB_DAMAGE on n$HASHDB_DAMAGE_NODE"
    [ "$CHECKPOINT_AHEAD_AT" -eq 0 ] || ! official_node "$CHECKPOINT_AHEAD_NODE" || refuse "CHECKPOINT_AHEAD on n$CHECKPOINT_AHEAD_NODE"
    [ "$APPLY_FAIL_AT" -eq 0 ] || ! official_node "$APPLY_FAIL_NODE" || refuse "APPLY_FAIL on n$APPLY_FAIL_NODE"
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do ! official_node "$i" || refuse "snapshot serving on n$i"; done
    [ "$SYNC_CONTRACT_EXECUTION" != 0 ] || refuse "SYNC_CONTRACT_EXECUTION=0"
    if [ -n "$NODE_STORAGE" ]; then
        for i in $(seq 1 "$ALL_NODES"); do
            ! official_node "$i" || [ "${STORE_BACKEND[$i]}" = berkeleydb ] || refuse "NODE_STORAGE ${STORE_BACKEND[$i]} on n$i"
        done
    fi
    if [ "$DPOS_AT" -gt 0 ]; then
        restarted="$RESTART_NODES $CRASH_NODE $TERM_KILL_NODE"
        [ "$CRASH_ALL_AT" -eq 0 ] || refuse "CRASH_ALL_AT with DPOS_AT (order 9 is not kept across a restart)"
        for i in $restarted; do [ "$i" -eq 0 ] || ! official_node "$i" || refuse "a restart of n$i with DPOS_AT (order 9 is not kept across a restart)"; done
    fi
    [ -z "$DB_BACKEND" ] || echo "mixed nodes: the official nodes run BerkeleyDB whatever DB_BACKEND ($DB_BACKEND) says"
fi

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
# the public API port of node i when it has one: 9090 for n1 with FUND or CONTRACTS, else 9100 + i
api_port() {
    if [ "$1" -eq 1 ] && { [ "$FUND" = "1" ] || [ "$CONTRACTS" = "1" ]; }; then echo 9090; else echo $((9100 + $1)); fi
}

api_settings() {
    local i=$1
    if [ "$CONTRACTS" != "1" ]; then
        if [ "$i" -eq "$API_LOAD_NODE" ]; then
            echo "port=$((9100 + i))"
            echo "apiexec_port=0"
            return
        fi
        if [ "$BIN_ALT_OFFICIAL" = 1 ]; then
            # every node answers block queries: the official ones are compared with n1 by block hash
            echo "port=$(api_port "$i")"
        else
            echo "port=$([ "$FUND" = "1" ] && [ "$i" -eq 1 ] && echo 9090 || echo 0)"
        fi
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
    # the node asks its new executor for its version every second for this long, and disables it for
    # good if it never answers: dozens of JVMs starting at once on one machine (a power cut, a big
    # network) can take longer than the 7 s default
    echo "executor_check_version_delay=60000"
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
    elif [ "$i" -eq "$CRASH_NODE" ] || { [ "$CHECKPOINT_AHEAD_AT" -gt 0 ] && [ "$i" -eq "$CHECKPOINT_AHEAD_NODE" ]; }; then
        # frequent checkpoints (checkpoint_every is at least 1000 blocks, so one a minute as well), so
        # that the kills hit the moments right after one
        lines="checkpoint_every=1000\ncheckpoint_every_minutes=1\ncheckpoint_keep=3\n"
    fi
    if [ "$JOIN_FETCH" = p2p ] && [[ ",$SNAPSHOT_SERVE_NODES,$SNAPSHOT_LATE_SERVE_NODES," == *",$i,"* ]]; then
        # a joiner needs JOIN_MIN_SOURCES sources on one block: a serving node builds its snapshots
        # from the checkpoints every 1'000 blocks only, not from the minute ones of a pruned node
        lines=${lines//checkpoint_every_minutes=1\\n/}
    fi
    if [[ ",$SNAPSHOT_SERVE_NODES,$SNAPSHOT_LATE_SERVE_NODES," == *",$i,"* ]] && [[ "$lines" != *checkpoint_every=* ]]; then
        # a serving node builds its snapshots from the periodic checkpoints
        lines="${lines}checkpoint_every=1000\ncheckpoint_keep=2\n"
    fi
    [ -n "$backend" ] && lines="${lines}db_backend=$backend\n"
    [ -n "$lines" ] && printf "\n[storage]\n$lines"
    return 0
}

# the [snapshot] section of node i: no fetch at a start on an empty database (every node of this
# network starts on one), but the joining node's with JOIN_FETCH=p2p; serve=on on the serving nodes.
# The official nodes have no such section
snapshot_section() {
    local i=$1
    official_node "$i" && return 0
    printf "\n[snapshot]\n"
    if [ "$JOIN_FETCH" = p2p ] && [ "$i" -eq "$JOIN_NODE" ]; then
        printf "fetch=auto\nmin_sources=%s\nfetch_wait=%s\nfetch_fallback=%s\n" "$JOIN_MIN_SOURCES" "$JOIN_WAIT" "$JOIN_FALLBACK"
        [ "$JOIN_FETCH_RATE" -eq 0 ] || printf "fetch_rate_kbps=%s\n" "$JOIN_FETCH_RATE"
        if [ "$JOIN_SOURCES" = keys ]; then
            local keys=""
            for j in ${SNAPSHOT_SERVE_NODES//,/ } ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do keys="$keys${keys:+,}${KEYS[$((j - 1))]}"; done
            printf "sources=%s\n" "$keys"
        fi
    else
        printf "fetch=off\n"
    fi
    if [[ ",$SNAPSHOT_SERVE_NODES," == *",$i,"* ]]; then printf "serve=on\n"; fi
    return 0
}

for i in $(seq 1 "$ALL_NODES"); do
    dir="$WORK/n$i"
    : > "$dir/hosts.txt"
    : > "$dir/trusted.txt"
    for j in $(seq 1 "$NODES"); do
        [ "$j" -gt "$TRUSTED_NODES" ] || echo "${KEYS[$((j - 1))]}" >> "$dir/trusted.txt"
        [ "$j" -ne "$i" ] && echo "$(node_ip "$j"):$((BASE_PORT + j)) ${KEYS[$((j - 1))]}" >> "$dir/hosts.txt"
    done
    cat > "$dir/config.ini" <<CFG
[params]
hosts_filename=hosts.txt
init_trusted_filename=trusted.txt
ipv6=false
traverse_nat=false
min_neighbours=1
max_neighbours=16
sync_contract_execution=$([ "$SYNC_CONTRACT_EXECUTION" = 0 ] && echo false || echo true)
round_elapse_time=$ROUND_ELAPSE_MS

[host_input]
ip=$(node_ip "$i")
port=$((BASE_PORT + i))

[api]
$(api_settings "$i")
ajax_port=0
diag_port=0
$(storage_section "$i")
$(snapshot_section "$i")

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

# test-build hooks of a forged or corrupt snapshot source, and the joiner's list of serving nodes
[ "$FORGE_NODE" -eq 0 ] || : > "$WORK/n$FORGE_NODE/testnet-forge-snapshot"
[ "$CORRUPT_NODE" -eq 0 ] || : > "$WORK/n$CORRUPT_NODE/testnet-corrupt-pieces"
if [ "$JOIN_SOURCES" = file ]; then
    for j in ${SNAPSHOT_SERVE_NODES//,/ } ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do
        echo "$(node_ip "$j"):$((BASE_PORT + j)) ${KEYS[$((j - 1))]}" >> "$WORK/n$JOIN_NODE/snapshot_sources.txt"
    done
fi

# the binary node i runs
bin_of() {
    if [ -n "$BIN_ALT" ] && [ $(($1 % 2)) -eq 0 ]; then echo "$BIN_ALT"; else echo "$BIN"; fi
}

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
node_nets_on() {
    for i in $(seq 1 "$ALL_NODES"); do
        sudo iptables -t nat -A POSTROUTING -o lo -p udp --sport $((BASE_PORT + i)) -j SNAT --to-source "$(node_ip "$i")" \
            || { echo "FAIL: cannot give n$i the source address $(node_ip "$i")"; exit 1; }
    done
    echo "node nets: n1..n$ALL_NODES send their p2p datagrams from 127.0.<i>.1"
}
node_nets_off() {
    sudo iptables -t nat -F POSTROUTING 2>/dev/null || true
}
net_fault_stats() {
    sudo tc -s qdisc show dev lo 2>/dev/null | grep -A1 netem | tail -1 | sed 's/^ *//'
}

stop_all() {
    # the sampler's last sample still sees the nodes running
    if [ -n "${SAMPLER_PID:-}" ]; then
        kill -TERM "$SAMPLER_PID" 2>/dev/null || true
        for _ in $(seq 1 20); do kill -0 "$SAMPLER_PID" 2>/dev/null || break; sleep 1; done
        kill -KILL "$SAMPLER_PID" 2>/dev/null || true
        wait "$SAMPLER_PID" 2>/dev/null || true
    fi
    [ -z "${API_LOAD_PID:-}" ] || kill -TERM "$API_LOAD_PID" 2>/dev/null || true
    [ -z "${JOIN_KILL_PID:-}" ] || kill "$JOIN_KILL_PID" 2>/dev/null || true
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
    [ "$NODE_NETS" != 1 ] || node_nets_off
    [ -z "${GARBAGE_PID:-}" ] || kill "$GARBAGE_PID" 2>/dev/null || true
    [ ! -f "$WORK/resources-summary.txt" ] || cat "$WORK/resources-summary.txt"
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
    # this runs in a subshell ($(...)): wait cannot wait for the node, which is not its child
    kill -TERM "${PIDS[$i]}"
    for _ in $(seq 1 180); do kill -0 "${PIDS[$i]}" 2>/dev/null || break; sleep 1; done
    pkill -f "cs.testnet.node=n$i " 2>/dev/null || true
    # an empty hosts file stops the node ("No hosts specified"): one host nobody listens on
    echo "127.0.0.1:9 11111111111111111111111111111111" > "$dir/hosts-none.txt"
    awk -v port=$((BASE_PORT + 500 + i)) '
        /^\[/ { section = $0 }
        /^hosts_filename=/ { print "hosts_filename=hosts-none.txt"; next }
        section == "[host_input]" && /^port=/ { print "port=" port; next }
        { print }' "$dir/config.ini" > "$dir/config-offline.ini"
    # the start-up line only ("... wallets <n>" at the end), not the checkpoint lines
    local startup='STATE DIGEST #[0-9]+ [0-9A-Fa-f]+ wallets [0-9]+ *$'
    before=$(grep -acE "$startup" "$dir/node.log" || true)
    (cd "$dir" && exec "$BIN" --db-path db --config-file config-offline.ini </dev/null >> stdout.log 2>&1) &
    local pid=$! deadline=$(( $(date +%s) + 300 ))
    while [ "$(grep -acE "$startup" "$dir/node.log" || true)" -le "${before:-0}" ] && [ "$(date +%s)" -lt "$deadline" ]; do
        sleep 2
    done
    kill -TERM "$pid"; wait "$pid" 2>/dev/null || true
    for _ in $(seq 1 180); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
    pkill -f "[c]ontract-executor.*n$i/" 2>/dev/null || true
    if [ "$(grep -acE "$startup" "$dir/node.log" || true)" -le "${before:-0}" ]; then
        echo "FAIL: n$i logged no start-up digest without peers" >&2
        return 1
    fi
    grep -aE "$startup" "$dir/node.log" | tail -1 | sed 's/^.*STATE DIGEST/STATE DIGEST/'
}

# the lowest sequence among the nodes of the restart test
# a SIGKILLed node leaves its executor JVM running (it has no death signal), still bound to its port
kill_executor() {
    pkill -KILL -f "cs.testnet.node=n$1 " 2>/dev/null || true
}

# the calls after the restart (contracts.py --wait-file) go once the nodes stopped by the restart
# test and by a power cut are back and 30 blocks past it, and the lowest node is at CONTRACTS_AFTER_AT
contracts_go_ready() {
    { [ "$RESTART_AT" -eq 0 ] || { [ "$restarted" -eq 1 ] && [ "$(restarted_seq)" -ge $((stopped_at + 30)) ]; }; } \
        && { [ "$CRASH_ALL_AT" -eq 0 ] || { [ "$crashed_all" -eq 1 ] && [ "$min" -ge $((CRASH_ALL_AT + 30)) ]; }; } \
        && [ "$min" -ge "$CONTRACTS_AFTER_AT" ]
}

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

if [ "$NODE_NETS" = 1 ]; then
    node_nets_on
fi
for i in $(seq 1 "$NODES"); do start_node "$i"; done
echo "started $NODES nodes in $WORK"
SAMPLER_PID=
if [ "$SAMPLE_SECONDS" -gt 0 ]; then
    python3 "$HERE/sampler.py" "$WORK" "$SAMPLE_SECONDS" "$WORK/resources.tsv" --nodes "$NODES" \
        --summary-file "$WORK/resources-summary.txt" > "$WORK/sampler.log" 2>&1 &
    SAMPLER_PID=$!
fi

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
    max=0
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
                    gdb -q -batch -ex "bt 60" -ex "thread apply all bt 6" "$(bin_of "$i")" "$core" 2>/dev/null | grep -E '^(#|Thread)' | head -n 200
                    mv "$core" "$core.traced"
                done
            fi
        fi
        if [ "$min" -lt 0 ] || [ "$s" -lt "$min" ]; then min=$s; fi
        if [ "$s" -gt "$max" ]; then max=$s; fi
    done
    join_seq=-1
    if [ "$JOIN_NODE" -gt 0 ] && [ -n "${PIDS[$JOIN_NODE]:-}" ]; then
        join_seq=$(seq_of "$JOIN_NODE"); join_seq=${join_seq:-0}
        line+=" n$JOIN_NODE=$join_seq(joined)"
    fi
    echo "$line"
    echo "t=$(( TIMEOUT_MIN * 60 - (deadline - $(date +%s)) )) min=$min max=$max" > "$WORK/progress"

    if [ "$JOIN_PRUNED_AT" -gt 0 ] && [ -z "${PIDS[$JOIN_NODE]:-}" ] && [ "$min" -ge "$JOIN_PRUNED_AT" ]; then
        if [ "$JOIN_FETCH" = p2p ]; then
            echo "join test: n$JOIN_NODE starts on an empty database at sequence $min and fetches a snapshot from its neighbours"
        else
            echo "join test: n$JOIN_NODE starts from block 0 with pruned storage at sequence $min"
        fi
        start_node "$JOIN_NODE"
        join_started=$(date +%s)
        if [ "$JOIN_KILL_DURING_FETCH" = 1 ]; then
            # the joiner's fetch and import take seconds: a watcher kills it at those lines (node.log
            # is appended across starts: the first "importing block" comes after the restart)
            (
                jl="$WORK/n$JOIN_NODE/node.log"
                for phase in "fetching block" "importing block"; do
                    until grep -aq "SNAPSHOT: $phase" "$jl" 2>/dev/null; do
                        grep -aq 'SNAPSHOT: started from block' "$jl" 2>/dev/null && exit 0
                        sleep 0.1
                    done
                    echo "$phase" > "$WORK/join-killed"
                    kill -KILL "$(cat "$WORK/join-pid" 2>/dev/null)" 2>/dev/null || true
                    # the main loop starts it again and removes the marker
                    while [ -e "$WORK/join-killed" ]; do sleep 0.2; done
                done
            ) &
            JOIN_KILL_PID=$!
            echo "${PIDS[$JOIN_NODE]}" > "$WORK/join-pid"
        fi
    fi
    # the harness killed the joiner on purpose: start it again
    if [ "$JOIN_KILL_DURING_FETCH" = 1 ] && [ -e "$WORK/join-killed" ] && [ -n "${PIDS[$JOIN_NODE]:-}" ] && ! kill -0 "${PIDS[$JOIN_NODE]}" 2>/dev/null; then
        wait "${PIDS[$JOIN_NODE]}" 2>/dev/null || true
        echo "join test: n$JOIN_NODE killed when it logged '$(cat "$WORK/join-killed")', started again"
        join_kills=$(( ${join_kills:-0} + 1 ))
        start_node "$JOIN_NODE"
        echo "${PIDS[$JOIN_NODE]}" > "$WORK/join-pid"
        rm -f "$WORK/join-killed"
    fi
    # late serving nodes: stopped long enough to leave the peers' routing tables, then serve=on
    if [ -n "$SNAPSHOT_LATE_SERVE_NODES" ] && [ -n "${join_started:-}" ] && [ -z "${late_done:-}" ] \
        && [ "$(date +%s)" -ge $(( join_started + JOIN_LATE_SERVE_AFTER )) ]; then
        late_done=1
        for i in ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do kill -TERM "${PIDS[$i]}" 2>/dev/null || true; done
        for i in ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do
            for _ in $(seq 1 60); do kill -0 "${PIDS[$i]}" 2>/dev/null || break; sleep 1; done
            wait "${PIDS[$i]}" 2>/dev/null || true
            sed -i 's/^fetch=off$/fetch=off\nserve=on/' "$WORK/n$i/config.ini"
        done
        sleep 40
        for i in ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do
            echo "join test: n$i starts again with serve=on"
            start_node "$i"
            SNAPSHOT_SERVE_NODES="$SNAPSHOT_SERVE_NODES${SNAPSHOT_SERVE_NODES:+,}$i"
        done
    fi
    # a p2p join that stops (a refused snapshot: exit code 4, a failed bootstrap: 1) fails at once
    if [ "$JOIN_FETCH" = p2p ] && [ -n "${PIDS[$JOIN_NODE]:-}" ] && ! kill -0 "${PIDS[$JOIN_NODE]}" 2>/dev/null \
        && ! { [ "$JOIN_KILL_DURING_FETCH" = 1 ] && [ -e "$WORK/join-killed" ]; }; then
        status=0; wait "${PIDS[$JOIN_NODE]}" 2>/dev/null || status=$?
        echo "FAIL: n$JOIN_NODE exited with status $status while joining"
        { grep -a 'SNAPSHOT' "$WORK/n$JOIN_NODE/node.log" || true; } | tail -40
        exit 1
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
            kill_executor "$CRASH_NODE"
            sleep 5
            start_node "$CRASH_NODE"
            crash_wait_qs=""
            crash_next=$(( $(date +%s) + 40 + RANDOM % 60 ))
        fi
    fi

    # the whole network killed at once, as after a power cut
    # with contracts, only once the first calls are done: a cut during the deploys would end contracts.py
    if [ "$CRASH_ALL_AT" -gt 0 ] && [ "$crashed_all" -eq 0 ] && [ "$min" -ge "$CRASH_ALL_AT" ] \
        && { [ "$CONTRACTS" != "1" ] || [ -e "$WORK/contracts-ready" ]; }; then
        echo "crash test: SIGKILL every node at sequence $min"
        for i in $(seq 1 "$ALL_NODES"); do
            [ -n "${PIDS[$i]:-}" ] && kill -KILL "${PIDS[$i]}" 2>/dev/null || true
        done
        for i in $(seq 1 "$ALL_NODES"); do
            [ -n "${PIDS[$i]:-}" ] && wait "${PIDS[$i]}" 2>/dev/null || true
        done
        pkill -KILL -f "cs.testnet.node=" 2>/dev/null || true
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
        if [ "$SNAPSHOT_FORMAT" = 1 ]; then
            "$REPO/packaging/docker/snapshot-make.sh" "$WORK/n$PRUNE_NODE" "$WORK/snapshot" "$MDB_COPY" local \
                | sed 's/^/snapshot test: /'
        fi
        # format 2: the node builds the snapshot of its latest periodic checkpoint itself; the
        # manifest must declare the digest the checkpoint logged when it was saved
        # who still holds the database files (RocksDB's LOCK allows one process)
        { fuser -v "$WORK/n$PRUNE_NODE"/db/LOCK "$WORK/n$PRUNE_NODE"/db/lmdb/lock.mdb 2>&1 || true; } | sed 's/^/snapshot test: holder: /'
        ps -eo pid,ppid,etimes,args | grep -a "n$PRUNE_NODE\b\|contract-executor" | grep -v grep | sed 's/^/snapshot test: process: /' || true
        rc=0
        (cd "$WORK/n$PRUNE_NODE" && exec "$BIN" --db-path db --config-file config.ini --export-snapshot "$WORK/snapshot2") \
            > "$WORK/export.log" 2>&1 || rc=$?
        grep -a 'export-snapshot:\|rocksdb: cannot' "$WORK/export.log" | sed 's/^/snapshot test: /'
        [ "$rc" = 0 ] || { tail -20 "$WORK/export.log"; echo "FAIL: --export-snapshot exited $rc"; exit 1; }
        python3 - "$WORK/snapshot2" "$WORK/n$PRUNE_NODE/node.log" <<'PY' || exit 1
import json, os, re, sys
root, log = sys.argv[1:]
dirs = [d for d in os.listdir(root) if d.isdigit()]
assert len(dirs) == 1, dirs
m = json.load(open(os.path.join(root, dirs[0], "snapshot.json")))
lines = re.findall(r"STATE DIGEST #(\d+) ([0-9A-Fa-f]{64}) wallets (\d+) checkpoint$", open(log, errors="replace").read(), re.M)
saved = {int(s): (d.lower(), int(w)) for s, d, w in lines}
if saved.get(m["seq"]) != (m["state_digest"], m["wallets"]):
    print(f"FAIL: format-2 snapshot of #{m['seq']} declares {m['state_digest']}, the checkpoint logged {saved.get(m['seq'])}")
    sys.exit(1)
print(f"snapshot test: format-2 snapshot of #{m['seq']} ({m['first_block']}..{m['seq']}), digest as logged at the checkpoint, {len(m['files'])} files")
PY
        start_node "$PRUNE_NODE"
        rc=0
        if [ "$SNAPSHOT_FORMAT" = 1 ]; then
            if [ "$SNAPSHOT_CORRUPT" = 1 ]; then
                f=$(ls "$WORK"/snapshot/credits-pruned-*.tar.zst)
                head -c $(( $(stat -c %s "$f") / 2 )) "$f" > "$f.cut" && mv "$f.cut" "$f"
                echo "snapshot test: archive truncated"
            fi
            "$REPO/packaging/docker/snapshot-restore.sh" "$WORK/n$JOIN_NODE" "$WORK/snapshot/latest.json" \
                > "$WORK/restore.log" 2>&1 || rc=$?
        else
            manifest=$(ls "$WORK"/snapshot2/*/snapshot.json)
            if [ "$SNAPSHOT_VIA" = download ]; then
                # the release layout: the manifest, and each file as s<seq>-<name>
                seq=$(basename "$(dirname "$manifest")")
                mkdir -p "$WORK/release"
                cp "$manifest" "$WORK/release/snapshot.json"
                for f in "$(dirname "$manifest")"/*; do
                    [ "$(basename "$f")" = snapshot.json ] || cp "$f" "$WORK/release/s$seq-$(basename "$f")"
                done
                if [ "$SNAPSHOT_CORRUPT" = 1 ]; then
                    f="$WORK/release/s$seq-blocks.bin.lz4"
                    head -c $(( $(stat -c %s "$f") / 2 )) "$f" > "$f.cut" && mv "$f.cut" "$f"
                    echo "snapshot test: s$seq-blocks.bin.lz4 truncated on the web server"
                fi
                python3 -m http.server 18080 --bind 127.0.0.1 --directory "$WORK/release" > "$WORK/http.log" 2>&1 &
                http_pid=$!
                for _ in $(seq 1 20); do curl -fs -o /dev/null http://127.0.0.1:18080/snapshot.json && break; sleep 0.5; done
                (cd "$WORK/n$JOIN_NODE" && exec "$BIN" --db-path db --config-file config.ini --download-snapshot http://127.0.0.1:18080/snapshot.json) \
                    > "$WORK/restore.log" 2>&1 || rc=$?
                kill "$http_pid" 2>/dev/null || true
                grep -a 'download-snapshot:\|import-snapshot:' "$WORK/restore.log" > "$WORK/restore.short" || true
                mv "$WORK/restore.short" "$WORK/restore.log"
                if [ "$SNAPSHOT_CORRUPT" = 1 ] && [ -n "$(ls -A "$WORK/n$JOIN_NODE/db" 2>/dev/null | grep -v '^\.lock$')" ]; then
                    echo "FAIL: a refused download left files in db/: $(ls -A "$WORK/n$JOIN_NODE/db")"; exit 1
                fi
            else
            if [ "$SNAPSHOT_CORRUPT" = 1 ]; then
                f="$(dirname "$manifest")/blocks.bin.lz4"
                head -c $(( $(stat -c %s "$f") / 2 )) "$f" > "$f.cut" && mv "$f.cut" "$f"
                echo "snapshot test: blocks.bin.lz4 truncated"
            fi
            # the joining node restores the snapshot itself into its configured backend
            (cd "$WORK/n$JOIN_NODE" && exec "$BIN" --db-path db --config-file config.ini --import-snapshot "$manifest") \
                > "$WORK/restore.log" 2>&1 || rc=$?
            grep -a 'import-snapshot:' "$WORK/restore.log" > "$WORK/restore.short" || true
            mv "$WORK/restore.short" "$WORK/restore.log"
            fi
        fi
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
        # n6 is killed twice at fixed points (testnet-convert-kill, read once by test-network builds): after
        # its second batch of 50, then, resumed, with every block converted and verified but before the
        # old database is compared and deleted. Its BerkeleyDB files are then damaged, as a crash in a
        # compaction can leave them (a log recovery cannot replay): the next start cannot open them
        # and must finish without them, every block being in db/.convert-new.
        convert6() {
            echo "$1" > "$WORK/n6/testnet-convert-kill"
            # a hang is killed after 300 s (exit 137, like the kill the node gives itself): the checks
            # below then fail instead of the whole job
            (cd "$WORK/n6" && exec timeout -s KILL 300 "$BIN" --db-path db --config-file config.ini --convert-db --convert-batch 50 </dev/null >> stdout.log 2>&1) \
                && { echo "FAIL: n6 was not killed at its conversion point $1"; tail -20 "$WORK/n6/stdout.log"; exit 1; }
            [ -f "$WORK/n6/db/.convert" ] || { echo "FAIL: n6 was killed at $1 but has no db/.convert to resume"; tail -20 "$WORK/n6/stdout.log"; exit 1; }
            [ ! -e "$WORK/n6/testnet-convert-kill" ] || { echo "FAIL: n6 did not read testnet-convert-kill"; exit 1; }
        }
        convert6 2
        # the kill comes as the second batch leaves the old database, before its "converted blocks" line:
        # one line, and two batches of 50 recorded below the last block
        n6first=$(sed -n 's/^first=//p' "$WORK/n6/db/.convert")
        n6last=$(sed -n 's/^last=//p' "$WORK/n6/db/.convert")
        [ -n "$n6first" ] && [ -n "$n6last" ] || { echo "FAIL: n6 db/.convert has no first/last"; cat "$WORK/n6/db/.convert"; exit 1; }
        [ "$(grep -ac 'converted blocks' "$WORK/n6/stdout.log")" -eq 1 ] && grep -aq "^done_down_to=$(( n6last - 99 ))\$" "$WORK/n6/db/.convert" \
            || { echo "FAIL: n6 was not killed after its second batch"; cat "$WORK/n6/db/.convert"; tail -20 "$WORK/n6/stdout.log"; exit 1; }
        convert6 checked
        grep -aq "^done_down_to=$n6first\$" "$WORK/n6/db/.convert" && grep -aq '^phase=blocks$' "$WORK/n6/db/.convert" \
            || { echo "FAIL: n6 was not killed with every block converted"; cat "$WORK/n6/db/.convert"; exit 1; }
        # the first page (the table's metadata) of both block tables overwritten with garbage (not zeros,
        # which BerkeleyDB may take for a file being created): neither recovery nor open can use them
        for f in blockchain.db sequence.db; do
            [ -f "$WORK/n6/db/$f" ] || { echo "FAIL: n6 has no db/$f to damage"; ls -la "$WORK/n6/db"; exit 1; }
            head -c 4096 /dev/zero | tr '\0' '\253' | dd of="$WORK/n6/db/$f" bs=4096 count=1 conv=notrunc status=none
        done
        convert_lines6=$(wc -l < "$WORK/n6/node.log")
        echo "convert test: n6 killed after a batch and after the last check of its conversion to rocksdb, berkeleydb files damaged, restarting it"
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
            # zeros over the pages after the two meta pages, up to the last page in use
            pages=$("$(dirname "$MDB_COPY")/mdb_stat" -e "$WORK/n$hd/caches/hashdb" | sed -nE 's/.*Number of pages used: ([0-9]+).*/\1/p')
            [ "${pages:-0}" -gt 4 ] || { echo "FAIL: n$hd hashdb has only ${pages:-no} pages in use"; exit 1; }
            dd if=/dev/zero of="$WORK/n$hd/caches/hashdb/data.mdb" bs=4096 seek=2 count=$(( pages - 2 )) conv=notrunc 2>/dev/null
            if "$REPO/packaging/docker/lmdb-check.sh" "$(dirname "$MDB_COPY")" "$WORK/n$hd/caches/hashdb" 2>/dev/null; then
                echo "FAIL: n$hd hashdb is still whole after the damage"; exit 1
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
    if [ "$DISK_FULL_STOP" = 1 ] && [ "$disk_filled_at" -gt 0 ] && [ "${disk_stop_done:-0}" -eq 0 ] \
        && [ "$(date +%s)" -ge $((disk_filled_at + DISK_FULL_SECONDS / 2)) ]; then
        disk_stop_done=1
        if kill -0 "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null; then
            kill -TERM "${PIDS[$DISK_FULL_NODE]}"
            stop_t0=$(date +%s)
            while kill -0 "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null && [ $(( $(date +%s) - stop_t0 )) -lt 120 ]; do sleep 1; done
            if kill -0 "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null; then
                echo "FAIL: n$DISK_FULL_NODE did not stop on SIGTERM within 120 s while its disk was full"
                kill -KILL "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null || true
                exit 1
            fi
            wait "${PIDS[$DISK_FULL_NODE]}" 2>/dev/null || true
            pkill -f "cs.testnet.node=n$DISK_FULL_NODE " 2>/dev/null || true
            echo "disk-full test: n$DISK_FULL_NODE stopped on SIGTERM in $(( $(date +%s) - stop_t0 )) s with its disk full"
        fi
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

    if [ "$CHECKPOINT_AHEAD_AT" -gt 0 ] && [ "${ca_done:-0}" -eq 0 ] && [ "$min" -ge "$CHECKPOINT_AHEAD_AT" ]; then
        ca=$CHECKPOINT_AHEAD_NODE; ca_dir="$WORK/n$ca"
        ca_cps=$(ls "$ca_dir/qs" | grep -E '^[1-9][0-9]*$' | sort -n | tail -2)
        if [ "$(wc -w <<<"$ca_cps")" -ge 2 ]; then
            ca_done=1
            ca_top=$(( $(head -1 <<<"$ca_cps") - 5 ))
            kill -KILL "${PIDS[$ca]}" 2>/dev/null || true; wait "${PIDS[$ca]}" 2>/dev/null || true
            pkill -KILL -f "cs.testnet.node=n$ca " 2>/dev/null || true
            # offline (a host nobody listens on) with --set-bc-top, killed once the database is cut
            echo "127.0.0.1:9 11111111111111111111111111111111" > "$ca_dir/hosts-none.txt"
            awk -v port=$((BASE_PORT + 500 + ca)) '
                /^\[/ { section = $0 }
                /^hosts_filename=/ { print "hosts_filename=hosts-none.txt"; next }
                section == "[host_input]" && /^port=/ { print "port=" port; next }
                { print }' "$ca_dir/config.ini" > "$ca_dir/config-offline.ini"
            ca_before=$(grep -ac 'Changing consensus settings to' "$ca_dir/node.log" || true)
            (cd "$ca_dir" && exec "$BIN" --db-path db --config-file config-offline.ini --set-bc-top "$ca_top" </dev/null >> stdout.log 2>&1) &
            ca_pid=$!
            ca_t0=$(date +%s)
            until [ "$(grep -ac 'Changing consensus settings to' "$ca_dir/node.log" || true)" -gt "${ca_before:-0}" ] \
                || ! kill -0 "$ca_pid" 2>/dev/null || [ $(( $(date +%s) - ca_t0 )) -ge 120 ]; do sleep 1; done
            kill -KILL "$ca_pid" 2>/dev/null || true; wait "$ca_pid" 2>/dev/null || true
            pkill -KILL -f "cs.testnet.node=n$ca " 2>/dev/null || true
            echo "checkpoint-ahead test: n$ca cut to block $ca_top, its checkpoints $(tr '\n' ' ' <<<"$ca_cps")are ahead of it"
            ca_lines=$(wc -l < "$ca_dir/node.log")
            ca_restarts=0
            start_node "$ca"
        fi
    fi
    if [ "${ca_done:-0}" -eq 1 ] && ! kill -0 "${PIDS[$CHECKPOINT_AHEAD_NODE]}" 2>/dev/null; then
        wait "${PIDS[$CHECKPOINT_AHEAD_NODE]}" 2>/dev/null || true
        pkill -KILL -f "cs.testnet.node=n$CHECKPOINT_AHEAD_NODE " 2>/dev/null || true
        ca_restarts=$((ca_restarts + 1))
        if [ "$ca_restarts" -gt 5 ]; then
            echo "FAIL: n$CHECKPOINT_AHEAD_NODE stopped at every start ($ca_restarts times) with checkpoints ahead of its blocks"
            tail -n +"$((ca_lines + 1))" "$WORK/n$CHECKPOINT_AHEAD_NODE/node.log" | grep -aE 'startSequence|ahead of the blocks|Couldn.t open|QUICK START' | tail -6
            exit 1
        fi
        echo "checkpoint-ahead test: n$CHECKPOINT_AHEAD_NODE exited, start $ca_restarts"
        sleep 3
        start_node "$CHECKPOINT_AHEAD_NODE"
    fi

    if [ "$APPLY_FAIL_AT" -gt 0 ] && [ "${apply_fail_set:-0}" -eq 0 ] && [ "$min" -ge "$APPLY_FAIL_AT" ]; then
        apply_fail_set=1
        af=$APPLY_FAIL_NODE
        af_seq=$(( $(seq_of "$af") + 20 ))
        echo "$af_seq" > "$WORK/n$af/testnet-fail-apply"
        af_lines=$(wc -l < "$WORK/n$af/node.log")
        af_t0=$(date +%s)
        echo "apply-fail test: n$af fails to apply block $af_seq"
        while kill -0 "${PIDS[$af]}" 2>/dev/null && [ $(( $(date +%s) - af_t0 )) -lt 240 ]; do sleep 1; done
        if kill -0 "${PIDS[$af]}" 2>/dev/null; then
            echo "FAIL: n$af did not stop after failing to apply block $af_seq"
            tail -n +"$((af_lines + 1))" "$WORK/n$af/node.log" | grep -aE "failed to apply|Couldn't save block|Pool already present" | head -5
            exit 1
        fi
        wait "${PIDS[$af]}" 2>/dev/null || true
        pkill -f "cs.testnet.node=n$af " 2>/dev/null || true
        echo "apply-fail test: n$af stopped $(( $(date +%s) - af_t0 )) s after the marker, restarting it"
        sleep 5
        start_node "$af"
    fi

    if [ "$API_LOAD_NODE" -gt 0 ] && [ -z "${API_LOAD_PID:-}" ] && [ "$min" -ge "$API_LOAD_FROM" ]; then
        python3 "$HERE/api_load.py" "127.0.0.1:$((9100 + API_LOAD_NODE))" "$WORK/api_load.stats" > "$WORK/api_load.log" 2>&1 &
        API_LOAD_PID=$!
        echo "api load: reading random blocks through n$API_LOAD_NODE's API from sequence $min"
    fi

    if [ "$FUND" = "1" ] && [ -z "$FUND_PID" ] && [ "$min" -ge "$FUND_AT" ]; then
        echo "funding nodes from the genesis key, then $LOAD_SECONDS s of transfers"
        python3 "$HERE/fund.py" "$WORK" "$NODES" "$MASTER_SEED" --load-seconds "$LOAD_SECONDS" \
            --dpos-at "$DPOS_AT" --reward-round "$REWARD_ROUND" --delegation-seconds "$DELEGATION_SECONDS" --min-stake "$MIN_STAKE" --syncro-round "$SYNCRO_ROUND" \
            --low-stake-nodes "$LOW_STAKE_NODES" \
            $([ "$DELEGATE" = "1" ] && echo --delegations) > "$WORK/fund.log" 2>&1 &
        FUND_PID=$!
    fi

    if [ "$CONTRACTS" = "1" ] && [ -z "$CONTRACTS_PID" ] && grep -q "master transactions done" "$WORK/fund.log" 2>/dev/null; then
        echo "contracts: deploying and calling them through node 1"
        (cd "$HERE" && python3 contracts.py "$WORK" "$NODES" "$MASTER_SEED" \
            --ready-file "$WORK/contracts-ready" --wait-file "$WORK/restart-done" --wait-timeout $((TIMEOUT_MIN * 60)) \
            --check-nodes "$ALL_NODES" --final-file "$WORK/contracts-final" \
            --load-seconds "$CONTRACT_LOAD_SECONDS" --write-rate "$CONTRACT_WRITE_RATE" --read-rate "$CONTRACT_READ_RATE" \
            --stats-file "$WORK/contracts-stats.json" $([ "$CONTRACT_DICE" = 1 ] && echo --dice)) > "$WORK/contracts.log" 2>&1 &
        CONTRACTS_PID=$!
    fi
    # contracts.py ended before its first calls were done: nothing would release the restart test
    if [ -n "$CONTRACTS_PID" ] && [ ! -e "$WORK/contracts-ready" ] && ! kill -0 "$CONTRACTS_PID" 2>/dev/null; then
        cat "$WORK/contracts.log"
        echo "FAIL: contracts.py ended before its first calls"
        exit 1
    fi
    # the calls after the restart must run on the restarted nodes too, so let them catch up first
    if [ "$CONTRACTS" = "1" ] && [ ! -e "$WORK/restart-done" ] && contracts_go_ready 2>/dev/null; then
        touch "$WORK/restart-done"
    fi

    # with contracts, not before the calls after the restart were let go
    if [ "$min" -ge "$TARGET_SEQ" ] && { [ "$JOIN_NODE" -eq 0 ] || [ "$join_seq" -ge "$TARGET_SEQ" ]; } \
        && { [ "$CONTRACTS" != "1" ] || [ -e "$WORK/restart-done" ]; }; then break; fi
    if [ "$(date +%s)" -ge "$deadline" ]; then
        echo "FAIL: not every node reached sequence $TARGET_SEQ within $TIMEOUT_MIN min"
        exit 1
    fi
done

if [ -n "$CONTRACTS_PID" ]; then
    # every node, a joining one too, is at TARGET_SEQ: contracts.py may compare the states now
    touch "$WORK/contracts-final"
    # its own deadline: TIMEOUT_MIN is for reaching the target, and contracts.py still waits 60 s
    # and reads every node after it
    final_deadline=$(( $(date +%s) + 600 ))
    while kill -0 "$CONTRACTS_PID" 2>/dev/null && [ "$(date +%s)" -lt "$final_deadline" ]; do sleep 5; done
    if kill -0 "$CONTRACTS_PID" 2>/dev/null; then
        kill "$CONTRACTS_PID" 2>/dev/null || true
        cat "$WORK/contracts.log"
        echo "FAIL: contracts.py did not finish its final checks within 10 min of the target"
        exit 1
    fi
    contracts_status=0
    wait "$CONTRACTS_PID" || contracts_status=$?
    cat "$WORK/contracts.log"
    # an executor left running by a killed node keeps the port its successor needs
    if grep -l 'Address already in use' "$WORK"/n*/executor.log 2>/dev/null; then
        echo "FAIL: an executor could not bind its port (an orphan executor of a killed node)"
        exit 1
    fi
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

if [ "$CHECKPOINT_AHEAD_AT" -gt 0 ]; then
    [ "${ca_done:-0}" = 1 ] || { echo "FAIL: the checkpoint-ahead test did not run (fewer than 2 checkpoints)"; exit 1; }
    after=$(tail -n +"$((ca_lines + 1))" "$WORK/n$CHECKPOINT_AHEAD_NODE/node.log")
    grep -aq 'is ahead of the blocks on disk, moving it to' <<<"$after" \
        || { echo "FAIL: n$CHECKPOINT_AHEAD_NODE set no checkpoint aside"; exit 1; }
    echo "checkpoint-ahead test: n$CHECKPOINT_AHEAD_NODE set $(grep -ac 'is ahead of the blocks on disk, moving it to' <<<"$after") checkpoint(s) aside in $ca_restarts failed start(s), then caught up"
fi
if [ "$APPLY_FAIL_AT" -gt 0 ]; then
    after=$(tail -n +"$((af_lines + 1))" "$WORK/n$APPLY_FAIL_NODE/node.log")
    grep -aq "failed to apply to the caches" <<<"$after" || { echo "FAIL: n$APPLY_FAIL_NODE never hit the injected failure"; exit 1; }
    ! grep -aq "Pool already present" <<<"$after" || { echo "FAIL: n$APPLY_FAIL_NODE offered a stored block again"; exit 1; }
    ! grep -aq "caches for QUICK START saved" <<<"$(awk '/failed to apply to the caches/{f=1} f' <<<"$after" | awk '/QUICK START!/{exit} {print}')" \
        || { echo "FAIL: n$APPLY_FAIL_NODE saved its caches after the failure"; exit 1; }
    echo "apply-fail test: n$APPLY_FAIL_NODE stopped without a checkpoint and caught up after a restart"
fi
if [ -n "${API_LOAD_PID:-}" ]; then
    kill -TERM "$API_LOAD_PID" 2>/dev/null || true
    wait "$API_LOAD_PID" 2>/dev/null || true
    API_LOAD_PID=
    stats=$(cat "$WORK/api_load.stats" 2>/dev/null || echo "calls=0 errors=0")
    echo "api load: $stats"
    load_calls=$(sed -nE 's/.*calls=([0-9]+).*/\1/p' <<<"$stats")
    [ "${load_calls:-0}" -ge 1000 ] || { echo "FAIL: the API load made only ${load_calls:-0} calls"; tail -20 "$WORK/api_load.log"; exit 1; }
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
        if official_node "$i"; then used="berkeleydb (official node)"; fi
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
    # the binary of the node whose process left it (core.<name>.<pid>)
    core_bin=$BIN
    for i in $(seq 1 "$ALL_NODES"); do [ "${PIDS[$i]:-}" != "${core##*.}" ] || core_bin=$(bin_of "$i"); done
    gdb -q -batch -ex "bt 40" "$core_bin" "$core" 2>/dev/null | grep -E '^#' | head -n 40
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
if [ "$JOIN_FETCH" = p2p ] && [ "$JOIN_EXPECT" = fallback ]; then
    jdir="$WORK/n$JOIN_NODE"
    jlog="$jdir/node.log"
    fail_join() {
        echo "FAIL: n$JOIN_NODE $1"
        { grep -a 'SNAPSHOT' "$jlog" || true; } | tail -40
        exit 1
    }
    # the fallback: no agreement in fetch_wait, the line naming the manual ways out, then block 0
    grep -aq 'SNAPSHOT: started with an empty database after' "$jlog" || fail_join "did not fall back to syncing from block 0"
    grep -aq -- '--download-snapshot' "$jlog" || fail_join "never logged the manual way out (--download-snapshot)"
    ! grep -aq 'SNAPSHOT: started from block ' "$jlog" || fail_join "started from a snapshot although too few sources served one"
    for f in snapshot.json snapshot.verified snapshot.mismatch snapshot-tmp db/.importing; do
        [ ! -e "$jdir/$f" ] || fail_join "has $f after the fallback"
    done
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 'SNAPSHOT: empty database' "$jlog" | sed 's/.*SNAPSHOT: //')"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 'SNAPSHOT: started with an empty database' "$jlog" | sed 's/.*SNAPSHOT: //')"
elif [ "$JOIN_FETCH" = p2p ]; then
    jdir="$WORK/n$JOIN_NODE"
    jlog="$jdir/node.log"
    fail_join() {
        echo "FAIL: n$JOIN_NODE $1"
        { grep -a 'SNAPSHOT' "$jlog" || true; } | tail -40
        exit 1
    }
    S=$({ grep -a -m1 'SNAPSHOT: started from block ' "$jlog" || true; } | sed -E 's/.*started from block ([0-9]+).*/\1/')
    [ -n "$S" ] || fail_join "did not start from a snapshot fetched from its neighbours"
    # the agreed group: "SNAPSHOT: block S hash … digest … : N sources (keys …)", without a reason
    agreed=$({ grep -a -E "SNAPSHOT: block $S hash [0-9a-f.]+ digest [0-9a-f.]+ : [0-9]+ sources \(keys[^)]*\)$" "$jlog" || true; } | tail -1)
    agreed_n=$(sed -E 's/.* : ([0-9]+) sources.*/\1/' <<<"$agreed")
    [ "${agreed_n:-0}" -ge "$JOIN_MIN_SOURCES" ] || fail_join "agreed on block $S with ${agreed_n:-no} sources, $JOIN_MIN_SOURCES needed"
    for line in "SNAPSHOT: fetched block $S" "SNAPSHOT: importing block $S" "SNAPSHOT: imported block $S" \
                "Caches for QUICK START loaded successfully" "SNAPSHOT: snapshot.json verified at block $S"; do
        grep -aqF "$line" "$jlog" || fail_join "never logged '$line'"
    done
    grep -aqE "STATE DIGEST #$S [0-9A-Fa-f]+ wallets [0-9]+ checkpoint loaded" "$jlog" || fail_join "did not quick-start from qs/$S"
    for f in snapshot.verified snapshot.sources.json snapshot.json; do [ -f "$jdir/$f" ] || fail_join "has no $f"; done
    for f in snapshot-tmp db/.importing snapshot.mismatch; do [ ! -e "$jdir/$f" ] || fail_join "still has $f"; done
    # the digest of the agreed snapshot is the joiner's at its quick start
    read -r snap_seq snap_digest < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["seq"], m["state_digest"])' "$jdir/snapshot.json")
    [ "$snap_seq" = "$S" ] || fail_join "snapshot.json is for block $snap_seq, not $S"
    got=$(grep -a -m1 -E "STATE DIGEST #$S " "$jlog" | sed -E 's/.*STATE DIGEST #[0-9]+ ([0-9A-Fa-f]+).*/\1/' || true)
    [ "${got,,}" = "${snap_digest,,}" ] || fail_join "start-up digest ${got:-none} differs from the snapshot's $snap_digest"
    # only nodes that announce snapshots are asked, and every source had a known address
    serving_keys=""
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do serving_keys="$serving_keys ${KEYS[$((i - 1))]}"; done
    for k in $({ grep -a -oE 'SNAPSHOT: asking [1-9A-HJ-NP-Za-km-z]+ for its manifest' "$jlog" || true; } | awk '{print $3}' | sort -u); do
        [[ " $serving_keys " == *" $k "* ]] || fail_join "asked $k, which does not serve, for a manifest"
    done
    unknown=$({ grep -a -oE '[0-9]+ not counted: address unknown' "$jlog" || true; } | awk '$1 > 0' | wc -l)
    [ "$unknown" -eq 0 ] || fail_join "counted no address for some sources ($unknown lines)"
    sources=$(python3 -c 'import json,sys; print(" ".join(s["key"] + "@" + s["address"] for s in json.load(open(sys.argv[1]))))' "$jdir/snapshot.sources.json")
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 'SNAPSHOT: empty database' "$jlog" | sed 's/.*SNAPSHOT: //')"
    echo "p2p join: n$JOIN_NODE agreed: $(sed 's/.*SNAPSHOT: //' <<<"$agreed")"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 "SNAPSHOT: fetched block $S" "$jlog" | sed 's/.*SNAPSHOT: //')"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 "SNAPSHOT: imported block $S" "$jlog" | sed 's/.*SNAPSHOT: //')"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 -E "STATE DIGEST #$S .* checkpoint loaded" "$jlog" | sed 's/.*STATE DIGEST/STATE DIGEST/')"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 'SNAPSHOT: snapshot.json verified' "$jlog" | sed 's/.*SNAPSHOT: //'); $(cat "$jdir/snapshot.verified")"
    echo "p2p join: n$JOIN_NODE $(grep -a -m1 'SNAPSHOT: started from block' "$jlog" | sed 's/.*SNAPSHOT: //'); sources: $sources"
    key8() { echo "${KEYS[$(($1 - 1))]:0:8}"; }
    if [ "$JOIN_KILL_DURING_FETCH" = 1 ]; then
        [ "${join_kills:-0}" -ge 2 ] || fail_join "was killed ${join_kills:-0} times, 2 expected (during the fetch and the import)"
        grep -aq 'SNAPSHOT: removed the files of an interrupted import' "$jdir/stdout.log" || fail_join "never removed the files of the killed import"
        grep -aq 'left by an interrupted fetch' "$jlog" || fail_join "never removed snapshot-tmp/ of the killed fetch"
        echo "p2p join: n$JOIN_NODE killed during the fetch and the import, cleaned up and joined"
    fi
    if [ "$FORGE_NODE" -gt 0 ]; then
        grep -aqE "SNAPSHOT: minority: .*$(key8 "$FORGE_NODE")" "$jlog" || fail_join "never logged n$FORGE_NODE's forged snapshot as a minority"
        ! grep -q "$(key8 "$FORGE_NODE")" <<<"$agreed" || fail_join "agreed on a group with n$FORGE_NODE's forged snapshot"
        echo "p2p join: n$FORGE_NODE's forged digest: $(grep -a -m1 "SNAPSHOT: minority: .*$(key8 "$FORGE_NODE")" "$jlog" | sed 's/.*SNAPSHOT: //')"
    fi
    if [ "$CORRUPT_NODE" -gt 0 ]; then
        ck=${KEYS[$((CORRUPT_NODE - 1))]}
        grep -aq "SNAPSHOT: rejected piece .* from $ck" "$jlog" || fail_join "never rejected a corrupt piece of n$CORRUPT_NODE"
        grep -aq "SNAPSHOT: source $ck .*not asked again" "$jlog" || fail_join "kept asking n$CORRUPT_NODE after its corrupt pieces"
        echo "p2p join: n$CORRUPT_NODE: $(grep -ac "SNAPSHOT: rejected piece .* from $ck" "$jlog") corrupt piece(s) rejected, then not asked again"
    fi
    if [ -n "$SNAPSHOT_LATE_SERVE_NODES" ]; then
        if [ "$JOIN_FALLBACK" = wait ]; then
            grep -aq 'Waiting for more sources (fetch_fallback=wait)' "$jlog" || fail_join "never logged that it waits for more sources"
        fi
        for i in ${SNAPSHOT_LATE_SERVE_NODES//,/ }; do
            grep -aq 'SNAPSHOT: serving block' "$WORK/n$i/node.log" || fail_join "n$i never served after its restart with serve=on"
            if [ "$JOIN_SOURCES" = file ]; then
                grep -aq "SNAPSHOT: asking ${KEYS[$((i - 1))]} for its manifest" "$jlog" || fail_join "never asked n$i, listed in snapshot_sources.txt"
            fi
        done
        echo "p2p join: n$JOIN_NODE waited for the late serving node(s) $SNAPSHOT_LATE_SERVE_NODES"
    fi
    joiner_key=${KEYS[$((JOIN_NODE - 1))]}
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do
        served=$({ grep -a "SNAPSHOT: served block $S to $joiner_key" "$WORK/n$i/node.log" || true; } | tail -1 | sed 's/.*SNAPSHOT: //')
        echo "p2p join: n$i ${served:-served part of the snapshot or nothing (a source logs only a complete download)}"
    done
    if [ "$BIN_ALT_OFFICIAL" = 1 ]; then
        # the official nodes never see the snapshot messages: the joiner asks only nodes that announce them
        for i in $(seq 2 2 "$NODES"); do
            ! grep -aqiE 'unknown message type|unknown.*type (40|41|42|43)\b' "$WORK/n$i"/*.log \
                || fail_join "sent n$i (official) a message it does not know"
        done
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
    grep -aq "resuming the conversion .* below block $(( n6last - 99 ))\$" "$WORK/n6/stdout.log" || { echo "FAIL: n6 did not resume its conversion midway"; exit 1; }
    [ "$(grep -ac "resuming the conversion .* below block $n6first\$" "$WORK/n6/stdout.log")" -ge 1 ] || { echo "FAIL: n6 did not resume a fully converted database"; exit 1; }
    grep -aq 'finishing without it' "$WORK/n6/stdout.log" || { echo "FAIL: n6 did not finish without its damaged berkeleydb database"; exit 1; }
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
python3 - "$WORK" "$ALL_NODES" "$FUND" "$NODES" "$({ [ "$SNAPSHOT_AT" -gt 0 ] || [ "$JOIN_FETCH" = p2p ]; } && echo 1 || echo 0)" "$BIN_ALT_OFFICIAL" <<'PY'
import collections, glob, re, sys
work, nodes, funded = sys.argv[1], int(sys.argv[2]), sys.argv[3] == "1"
# official nodes (the even ones with BIN_ALT_OFFICIAL) log no digest: their blocks are compared below
official = {i for i in range(2, nodes + 1, 2)} if sys.argv[6] == "1" else set()
# a node that joins from a snapshot (imported or fetched over p2p) starts past block 1000: only the
# initial nodes log it
initial = int(sys.argv[4]) if int(sys.argv[5]) > 0 else nodes
initial -= len([i for i in official if i <= initial])
seen = collections.defaultdict(dict)
for i in range(1, nodes + 1):
    if i in official:
        continue
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
# official nodes: the same block hashes as n1 (every 100th sequence up to the lowest head)
if [ "$BIN_ALT_OFFICIAL" = 1 ]; then
    ports=""
    head=""
    for i in $(seq 1 "$ALL_NODES"); do
        ports="$ports $i:$(api_port "$i")"
        s=$(seq_of "$i"); s=${s:-0}
        if [ -z "$head" ] || [ "$s" -lt "$head" ]; then head=$s; fi
    done
    # pruned nodes, and a node that joined from a snapshot, lack the oldest blocks
    partial=$(for i in $PRUNED_NODES $({ [ "$JOIN_FETCH" = p2p ] || [ "$SNAPSHOT_AT" -gt 0 ]; } && echo "$JOIN_NODE"); do echo "$i"; done | sort -u | paste -sd, -)
    (cd "$HERE" && python3 blockhashes.py --head "$head" --partial "$partial" $ports) || { echo "FAIL: official and community nodes hold different blocks"; exit 1; }
fi

# two blocks of one sequence: a node kept its own block while the round went on on the writer's
# (csnode/src/blockchain.cpp updateLastBlock logs it), a fork that nothing reconciles
forks=$(grep -al 'this node keeps its own (fork)' "$WORK"/n*/node.log 2>/dev/null | sed -E 's#.*/(n[0-9]+)/node.log#\1#' | tr '\n' ' ' || true)
if [ -n "$forks" ]; then
    grep -ah 'this node keeps its own (fork)' "$WORK"/n*/node.log | head -5
    echo "FAIL: fork, nodes on another block of the same sequence: $forks"
    exit 1
fi

# a network still restarting at the end is stuck, even if it reached the target before (a bootstrap
# round, R-x.1 and up, that never gives way to R-(x+1).0): the last three round headers of n1
stuck=$(grep -a ' R-[0-9'"'"']*\.[0-9]* ' "$WORK/n1/node.log" | tail -3 | grep -vc '\.0 ' || true)
[ "$stuck" -lt 3 ] || { echo "FAIL: the network ended stuck in a bootstrap round ($(grep -a ' R-[0-9'"'"']*\.[0-9]* ' "$WORK/n1/node.log" | tail -1 | sed -E 's/.*(R-[^ ]*).*/\1/'))"; exit 1; }

# who signs the blocks, per node and, with mixed binaries, per binary (node 1's API, needs FUND=1)
if [ "$FUND" = "1" ]; then
    (cd "$HERE" && python3 signers.py "$WORK" "$NODES" "$(( $(seq_of 1) - 1 ))" $([ -n "$BIN_ALT" ] && echo --alt-even)) || true
fi

# who took part in consensus: trusted per round (from n1's round headers) and how many nodes were
# trusted at least once (a node logs TRUSTED [index] in the rounds it is trusted in)
per_round=$(grep -ah 'Trusted count:' "$WORK/n1/node.log" | sed -E 's/.*Trusted count: ([0-9]+).*/\1/' | sort -n | uniq -c \
    | awk '{printf "%s%s trusted in %s rounds", sep, $2, $1; sep=", "}' || true)
ever=0
for i in $(seq 1 "$ALL_NODES"); do
    grep -aq 'TRUSTED \[' "$WORK/n$i/node.log" && ever=$((ever + 1))
done
echo "consensus: $per_round; $ever of $ALL_NODES nodes were trusted at least once (initial confidants: $TRUSTED_NODES)"
if [ "$LOW_STAKE_NODES" -gt 0 ]; then
    # once stake counts (StartingDPOS, moved by DPOS_AT; 100 rounds of margin for the order to land),
    # a node below the minimum stake must never be trusted
    from=$((DPOS_AT + 100))
    low_trusted=0
    for i in $(seq $((NODES - LOW_STAKE_NODES + 1)) "$NODES"); do
        # rounds print with digit groups (R-1'234.0)
        n=$(grep -a 'TRUSTED \[' "$WORK/n$i/node.log" | sed -E "s/.*R-([0-9']+)\..*/\1/" | tr -d "'" | awk -v f="$from" '$1 > f' | wc -l || true)
        [ "$n" -eq 0 ] || { echo "n$i (low stake) was trusted in $n rounds after round $from"; low_trusted=$((low_trusted + 1)); }
    done
    [ "$low_trusted" -eq 0 ] || { echo "FAIL: nodes below the minimum stake were elected trusted"; exit 1; }
    echo "stake: none of the $LOW_STAKE_NODES nodes below the minimum stake was trusted after round $from"
fi
if [ -n "$SNAPSHOT_SERVE_NODES" ]; then
    # every serving node built the snapshot of a checkpoint and serves it (the build follows the
    # checkpoint at 1'000 on a thread of low priority: a minute at most)
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do
        for _ in $(seq 1 60); do grep -aq 'SNAPSHOT: serving block' "$WORK/n$i/node.log" && break; sleep 1; done
        built=$({ grep -a 'SNAPSHOT: built block' "$WORK/n$i/node.log" || true; } | tail -1 | sed -E 's/.*(built block [0-9]+[^,]*, [^,]*, [^,]*).*/\1/')
        serving=$({ grep -a 'SNAPSHOT: serving block' "$WORK/n$i/node.log" || true; } | tail -1 | sed -E 's/.*SNAPSHOT: //')
        [ -n "$built" ] && [ -n "$serving" ] || { echo "FAIL: n$i did not build and serve a snapshot"; { grep -a 'SNAPSHOT' "$WORK/n$i/node.log" || true; } | tail -5; exit 1; }
        echo "snapshot: n$i $built; $serving"
    done
    # known_peers.json is written at a clean stop: stop one node of this build that does not serve
    observer=0
    # the joining node too: with every odd node serving and official even nodes it is the only one left
    for i in $(seq 1 "$ALL_NODES"); do
        [[ ",$SNAPSHOT_SERVE_NODES," == *",$i,"* ]] && continue
        [ -n "$BIN_ALT" ] && [ $((i % 2)) -eq 0 ] && continue
        observer=$i; break
    done
    [ "$observer" -gt 0 ] || { echo "FAIL: no node left to read the capabilities from"; exit 1; }
    kill -TERM "${PIDS[$observer]}" 2>/dev/null || true
    for _ in $(seq 1 60); do kill -0 "${PIDS[$observer]}" 2>/dev/null || break; sleep 1; done
    kill -0 "${PIDS[$observer]}" 2>/dev/null && { echo "FAIL: n$observer did not stop in 60 s"; exit 1; }
    wait "${PIDS[$observer]}" 2>/dev/null || true
    [ -f "$WORK/n$observer/known_peers.json" ] || { echo "FAIL: n$observer wrote no known_peers.json"; exit 1; }
    serving=""
    late=""
    for i in ${SNAPSHOT_SERVE_NODES//,/ }; do
        # a late serving node announced the capability only at its restart, and peers keep the value
        # they first saw (the routing table is not refreshed, design 4.1, plan P9): either value is fine
        if [[ ",$SNAPSHOT_LATE_SERVE_NODES," == *",$i,"* ]]; then late="$late,${KEYS[$((i - 1))]}"; else serving="$serving ${KEYS[$((i - 1))]}"; fi
    done
    python3 - "$WORK/n$observer/known_peers.json" "$late" $serving <<'PY' || { echo "FAIL: snapshot capability not announced as expected"; exit 1; }
import json, sys
peers = {p["id"]: p for p in json.load(open(sys.argv[1]))}
late = set(k for k in sys.argv[2].split(",") if k)
serving = set(sys.argv[3:])
bad = [k for k in serving if peers.get(k, {}).get("capabilities") != 1]
extra = [k for k, p in peers.items() if k not in serving and k not in late and p["capabilities"] != 0]
community = sum(1 for p in peers.values() if p["minor"] > 0)
print(f"capabilities: {len(peers)} known peers, {len(serving) - len(bad)} of {len(serving)} serving nodes announce snapshots, "
      f"{len(extra)} others do, {community} announce a community version")
sys.exit(1 if bad or extra else 0)
PY
    PIDS[$observer]=""
fi
echo "PASS: $ALL_NODES nodes reached sequence $TARGET_SEQ with matching state digests"
