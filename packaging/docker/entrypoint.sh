#!/usr/bin/env bash
# First-start setup and launch of a community Credits node in a container.
#
# Everything persistent lives in the data directory (/data, ~/.credits on the host): config.ini,
# keys/, the block database, caches and checkpoints, logs. config.ini is written from the .env
# values on the first start only; afterwards it is the operator's file and the source of truth.
# A pruned node with an empty data directory first downloads the latest published snapshot
# (CREDITS_SNAPSHOT=github, the default: node --download-snapshot); when that fails, the node itself
# looks for a snapshot among its p2p neighbours, then syncs from block 0.
# See packaging/docker/README.md for the variables.
set -euo pipefail

OPT=${CREDITS_OPT:-/opt/credits}
DATA=${CREDITS_DATA:-/data}
STORAGE=${CREDITS_STORAGE:-pruned}
case "$STORAGE" in pruned) default_backend=lmdb ;; full) default_backend=berkeleydb ;;
    *) echo "[entrypoint] CREDITS_STORAGE must be pruned or full, not '$STORAGE'"; exit 2 ;; esac
BACKEND=${CREDITS_DB_BACKEND:-$default_backend}
SNAPSHOT=${CREDITS_SNAPSHOT:-github}
[ "$SNAPSHOT" != auto ] || SNAPSHOT=github   # the value of earlier releases
SNAPSHOT_URL=${CREDITS_SNAPSHOT_URL:-https://github.com/EnzinoBB/node/releases/download/pruned-snapshot/snapshot.json}
# earlier releases pointed at the format-1 latest.json: the format-2 manifest sits next to it
SNAPSHOT_URL=${SNAPSHOT_URL%/latest.json}
[[ "$SNAPSHOT_URL" == *.json ]] || SNAPSHOT_URL=$SNAPSHOT_URL/snapshot.json
SERVE=${CREDITS_SERVE_SNAPSHOT:-on}
MIN_SOURCES=${CREDITS_SNAPSHOT_MIN_SOURCES:-}
P2P_PORT=${CREDITS_P2P_PORT:-9510}
API=${CREDITS_API:-off}
EXECUTOR=${CREDITS_EXECUTOR:-on}
RESET=${CREDITS_RESET:-0}
CONVERT_ARGS=()

log() { echo "[entrypoint] $*"; }
# refuse <message> [exit code]; the reset hint is no answer to an interrupted restore (5), which a
# plain restart retries, and following it would move the data aside for nothing
refuse() {
    log "NOT STARTING: $1"
    [ "${2:-3}" = 5 ] || log "To start over (the current data is moved to $DATA/old-<date>, not deleted), set CREDITS_RESET=1 once."
    exit "${2:-3}"
}
case "$BACKEND" in berkeleydb|lmdb|rocksdb) ;; *) log "CREDITS_DB_BACKEND must be berkeleydb, lmdb or rocksdb"; exit 2 ;; esac
case "$SNAPSHOT" in github|p2p|off) ;; *) log "CREDITS_SNAPSHOT must be github, p2p or off"; exit 2 ;; esac
for v in "$API" "$EXECUTOR" "$SERVE"; do case "$v" in on|off) ;; *) log "CREDITS_API, CREDITS_EXECUTOR and CREDITS_SERVE_SNAPSHOT must be on or off"; exit 2 ;; esac; done
[ -z "$MIN_SOURCES" ] || [[ "$MIN_SOURCES" =~ ^[0-9]+$ ]] || { log "CREDITS_SNAPSHOT_MIN_SOURCES must be a number"; exit 2; }
[[ "$P2P_PORT" =~ ^[0-9]+$ ]] || { log "CREDITS_P2P_PORT must be a number"; exit 2; }

cd "$DATA"
# left by the restore script of earlier releases; the node cleans up its own interrupted imports
rm -rf .restore-tmp .digest-before
if [ -e .restore-incomplete ]; then
    log "an interrupted snapshot restore of an earlier release left part of a snapshot: removing it"
    rm -rf db caches qs roundstat.dat smartcontracts.dat snapshot.json snapshot.verified
    rm -f .restore-incomplete
fi

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
        section == "[storage]" || section == "[snapshot]" { next }
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
    # snapshots: served to the neighbours (CREDITS_SERVE_SNAPSHOT); fetched from them at a first start
    # by a pruned node unless CREDITS_SNAPSHOT=off (a full node syncs from block 0: a snapshot holds
    # only recent blocks)
    local fetch=auto
    { [ "$SNAPSHOT" = off ] || [ "$STORAGE" = full ]; } && fetch=off
    printf '\n[snapshot]\nserve=%s\nfetch=%s\n' "$SERVE" "$fetch" >> config.ini.new
    [ -z "$MIN_SOURCES" ] || printf 'min_sources=%s\n' "$MIN_SOURCES" >> config.ini.new
    mv config.ini.new config.ini
    log "config.ini written: $STORAGE storage on $BACKEND, p2p port $P2P_PORT, API $API, executor $EXECUTOR, snapshots: serve $SERVE, fetch $fetch"
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

# db/.lock alone is no data: the node locks an existing db/ before it opens the database
has_data() { [ -d db ] && [ -n "$(ls -A db 2>/dev/null | grep -vxF .lock)" ]; }

first_start=0
download=0
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
    for p in db caches qs roundstat.dat smartcontracts.dat snapshot.json snapshot.verified snapshot.mismatch snapshot.sources.json \
             snapshot-tmp snapshot-download snapshot .storage; do
        [ ! -e "$p" ] || mv "$p" "$old/"
    done
    log "CREDITS_RESET=1: the previous state is in $DATA/$old; remove CREDITS_RESET from .env"
fi

[ ! -f snapshot.mismatch ] || refuse "the state restored from a snapshot did not match the snapshot's digest" 4

if has_data; then
    if [ -f .storage ]; then
        read -r rec_mode rec_backend < <(sed -E 's/mode=([a-z]+) backend=([a-z]+)/\1 \2/' .storage)
        if [ "$rec_backend" != "$cfg_backend" ]; then
            if [ "${CREDITS_CONVERT_DB:-}" = yes ]; then
                log "converting the block database from $rec_backend to $cfg_backend (CREDITS_CONVERT_DB=yes)"
            else
                refuse "the data was written by $rec_backend, config.ini asks for $cfg_backend: set CREDITS_CONVERT_DB=yes to convert it, or set CREDITS_DB_BACKEND=$rec_backend (and db_backend in config.ini)" 3
            fi
        fi
        if [ "$rec_mode" = pruned ] && [ "$cfg_mode" = full ]; then
            refuse "this node pruned its old blocks, so it cannot become a full node; a full node syncs from block 0"
        fi
        if [ "$rec_mode" = full ] && [ "$cfg_mode" = pruned ]; then
            log "switching from full to pruned storage: old blocks are removed at the next checkpoints"
        fi
    fi
elif [ "$cfg_mode" = pruned ] && [ "$SNAPSHOT" = github ]; then
    if [ "$cfg_backend" = berkeleydb ]; then
        log "a BerkeleyDB node cannot start from a snapshot: syncing from block 0"
    else
        download=1   # below, once the keys exist
    fi
fi
echo "mode=$cfg_mode backend=$cfg_backend" > .storage
# the node decides from db/ itself (nothing to do when the formats match): passed on every start, so
# a conversion refused or paused by the node is retried after .storage already follows config.ini
[ "${CREDITS_CONVERT_DB:-}" != yes ] || CONVERT_ARGS=(--convert-db)

for f in settings.properties known_hosts.txt default_confidants.txt; do
    [ -e "$f" ] || cp "$OPT/$f" "$f"
done
if [ ! -f keys/NodePublic.txt ]; then
    log "new node keys, public key $(python3 "$OPT/keygen.py" keys); back up $DATA/keys"
fi
mkdir -p log
if [ "$download" = 1 ]; then
    # explicit, on request of this image: the node's normal start never downloads from GitHub
    log "downloading the latest snapshot from $SNAPSHOT_URL"
    download_rc=0
    "$OPT/node" --db-path db --config-file config.ini --public-key-file keys/NodePublic.txt --private-key-file keys/NodePrivate.txt \
        --download-snapshot "$SNAPSHOT_URL" || download_rc=$?
    if [ "$download_rc" = 0 ]; then
        log "snapshot restored"
    else
        log "no snapshot downloaded (exit $download_rc): the node looks for one among its neighbours, then syncs from block 0"
    fi
fi
find log -maxdepth 1 -name 'executor-log-*.txt' -mtime +7 -delete 2>/dev/null || true

# the node checks a restored snapshot itself at its first start: snapshot.verified, or
# snapshot.mismatch and exit code 4 (refused above at the next start)
exec "$OPT/node" --db-path db --config-file config.ini \
    --public-key-file keys/NodePublic.txt --private-key-file keys/NodePrivate.txt \
    "${CONVERT_ARGS[@]}"
