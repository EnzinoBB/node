#!/usr/bin/env bash
# First-start setup and launch of a community Credits node in a container.
#
# Everything persistent lives in the data directory (/data, ~/.credits on the host): config.ini,
# keys/, the block database, caches and checkpoints, logs. config.ini is written from the .env
# values on the first start only; afterwards it is the operator's file and the source of truth.
# A pruned node with an empty data directory first restores the latest published snapshot.
# See packaging/docker/README.md for the variables.
set -euo pipefail

OPT=${CREDITS_OPT:-/opt/credits}
DATA=${CREDITS_DATA:-/data}
STORAGE=${CREDITS_STORAGE:-pruned}
case "$STORAGE" in pruned) default_backend=lmdb ;; full) default_backend=berkeleydb ;;
    *) echo "[entrypoint] CREDITS_STORAGE must be pruned or full, not '$STORAGE'"; exit 2 ;; esac
BACKEND=${CREDITS_DB_BACKEND:-$default_backend}
SNAPSHOT=${CREDITS_SNAPSHOT:-auto}
SNAPSHOT_URL=${CREDITS_SNAPSHOT_URL:-https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/latest.json}
P2P_PORT=${CREDITS_P2P_PORT:-9510}
API=${CREDITS_API:-off}
EXECUTOR=${CREDITS_EXECUTOR:-on}
RESET=${CREDITS_RESET:-0}
DIGEST_WAIT=${CREDITS_DIGEST_WAIT:-600}

log() { echo "[entrypoint] $*"; }
refuse() { log "NOT STARTING: $*"; log "To start over (the current data is moved to $DATA/old-<date>, not deleted), set CREDITS_RESET=1 once."; exit "${2:-3}"; }
case "$BACKEND" in berkeleydb|lmdb|rocksdb) ;; *) log "CREDITS_DB_BACKEND must be berkeleydb, lmdb or rocksdb"; exit 2 ;; esac
case "$SNAPSHOT" in auto|off) ;; *) log "CREDITS_SNAPSHOT must be auto or off"; exit 2 ;; esac
for v in "$API" "$EXECUTOR"; do case "$v" in on|off) ;; *) log "CREDITS_API and CREDITS_EXECUTOR must be on or off"; exit 2 ;; esac; done
[[ "$P2P_PORT" =~ ^[0-9]+$ ]] || { log "CREDITS_P2P_PORT must be a number"; exit 2; }

cd "$DATA"
rm -rf .restore-tmp

# config.ini from the package's template: no [storage] section from the template, ports, absolute
# executor paths, one executor per container
write_config() {
    local api_port=0 apiexec=0 command=""
    [ "$API" = on ] && api_port=9090
    if [ "$EXECUTOR" = on ]; then
        apiexec=9070
        command="$OPT/ojdkbuild/java-11-openjdk-11.0.4/bin/java -Xmx768m -XX:MaxMetaspaceSize=256m -jar $OPT/contract-executor.jar"
    fi
    awk -v p2p="$P2P_PORT" -v api="$api_port" -v apiexec="$apiexec" -v cmd="$command" '
        /^\[/ { section = $0 }
        section == "[storage]" { next }
        section == "[host_input]" && /^port=/ { print "port=" p2p; next }
        section == "[api]" && /^port=/ { print "port=" api; next }
        section == "[api]" && /^apiexec_port=/ { print "apiexec_port=" apiexec; next }
        section == "[api]" && /^executor_command=/ { print "executor_command=" cmd; next }
        section == "[api]" && /^executor_multi_instance=/ { print "executor_multi_instance=true"; next }
        { print }
    ' "$OPT/config.ini.template" > config.ini.new
    if [ "$STORAGE" = pruned ]; then
        printf '\n[storage]\n; pruned storage: only recent blocks are kept, from the last checkpoint but one\ncheckpoint_every=100000\ncheckpoint_keep=2\nprune_keep_blocks=1000\ndb_backend=%s\n' "$BACKEND" >> config.ini.new
    else
        printf '\n[storage]\ndb_backend=%s\n' "$BACKEND" >> config.ini.new
    fi
    mv config.ini.new config.ini
    log "config.ini written: $STORAGE storage on $BACKEND, p2p port $P2P_PORT, API $API, executor $EXECUTOR"
}

# what config.ini asks for: "<pruned|full> <backend>"
config_storage() {
    awk -F= '
        /^\[/ { section = $0; next }
        section == "[storage]" && $1 == "prune_keep_blocks" { keep = $2 + 0 }
        section == "[storage]" && ($1 == "db_backend" || ($1 == "block_db" && backend == "")) { backend = $2 }
        END {
            if (backend == "" || backend == "berkeley") backend = "berkeleydb"
            if (backend == "rocks") backend = "rocksdb"
            print (keep > 0 ? "pruned" : "full"), backend
        }' config.ini
}

has_data() { [ -d db ] && [ -n "$(ls -A db 2>/dev/null)" ]; }

first_start=0
[ -f config.ini ] || { write_config; first_start=1; }
read -r cfg_mode cfg_backend < <(config_storage)
if [ "$first_start" = 0 ]; then
    [ "$STORAGE" = "$cfg_mode" ] || log "warning: CREDITS_STORAGE=$STORAGE, but config.ini runs a $cfg_mode node; config.ini is used"
    [ -z "${CREDITS_DB_BACKEND:-}" ] || [ "$BACKEND" = "$cfg_backend" ] \
        || log "warning: CREDITS_DB_BACKEND=$BACKEND, but config.ini uses $cfg_backend; config.ini is used"
fi

if [ "$RESET" = 1 ] && { has_data || [ -e caches ] || [ -e qs ]; }; then
    old=old-$(date -u +%Y%m%d-%H%M%S)
    mkdir "$old"
    for p in db caches qs roundstat.dat smartcontracts.dat snapshot.json snapshot.verified snapshot.mismatch .storage; do
        [ ! -e "$p" ] || mv "$p" "$old/"
    done
    log "CREDITS_RESET=1: the previous state is in $DATA/$old; remove CREDITS_RESET from .env"
fi

[ ! -f snapshot.mismatch ] || refuse "the state restored from a snapshot did not match the snapshot's digest" 4

if has_data; then
    if [ -f .storage ]; then
        read -r rec_mode rec_backend < <(sed -E 's/mode=([a-z]+) backend=([a-z]+)/\1 \2/' .storage)
        [ "$rec_backend" = "$cfg_backend" ] \
            || refuse "the data was written by $rec_backend, config.ini asks for $cfg_backend; checkpoints and caches belong to the old database"
        if [ "$rec_mode" = pruned ] && [ "$cfg_mode" = full ]; then
            refuse "this node pruned its old blocks, so it cannot become a full node; a full node syncs from block 0"
        fi
        if [ "$rec_mode" = full ] && [ "$cfg_mode" = pruned ]; then
            log "switching from full to pruned storage: old blocks are removed at the next checkpoints"
        fi
    fi
elif [ "$cfg_mode" = pruned ] && [ "$SNAPSHOT" = auto ]; then
    if [ "$cfg_backend" != lmdb ]; then
        log "snapshots hold an LMDB database, this node uses $cfg_backend: syncing from block 0"
    else
        log "restoring the latest snapshot from $SNAPSHOT_URL"
        "$OPT/snapshot-restore.sh" "$DATA" "$SNAPSHOT_URL" || log "syncing from block 0 instead"
    fi
fi
echo "mode=$cfg_mode backend=$cfg_backend" > .storage

for f in settings.properties known_hosts.txt default_confidants.txt; do
    [ -e "$f" ] || cp "$OPT/$f" "$f"
done
if [ ! -f keys/NodePublic.txt ]; then
    log "new node keys, public key $(python3 "$OPT/keygen.py" keys); back up $DATA/keys"
fi
mkdir -p log
find log -maxdepth 1 -name 'executor-log-*.txt' -mtime +7 -delete 2>/dev/null || true

# after a restore, the node's start-up digest must equal the snapshot's
if [ -f snapshot.json ] && [ ! -f snapshot.verified ]; then
    read -r snap_seq snap_digest < <(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(m["seq"], m["state_digest"])' snapshot.json)
    node_pid=$$
    (
        deadline=$(( $(date +%s) + DIGEST_WAIT ))
        while [ "$(date +%s)" -lt "$deadline" ]; do
            line=$(cat log/log_*.txt 2>/dev/null | grep -a "STATE DIGEST #$snap_seq " | tail -1 || true)
            if [ -n "$line" ]; then
                got=$(sed -E 's/.*STATE DIGEST #[0-9]+ ([0-9A-Fa-f]+).*/\1/' <<<"$line")
                if [ "${got,,}" = "${snap_digest,,}" ]; then
                    touch snapshot.verified
                    log "snapshot digest verified at block $snap_seq"
                else
                    touch snapshot.mismatch
                    log "SNAPSHOT DIGEST MISMATCH at block $snap_seq: snapshot $snap_digest, node $got; stopping"
                    kill -TERM "$node_pid" 2>/dev/null || true
                fi
                exit 0
            fi
            sleep 5
        done
        log "warning: no start-up digest for block $snap_seq within $DIGEST_WAIT s, snapshot not verified"
    ) &
fi

exec "$OPT/node" --db-path db --config-file config.ini \
    --public-key-file keys/NodePublic.txt --private-key-file keys/NodePrivate.txt
