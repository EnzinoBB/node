#!/usr/bin/env bash
# Guided set-up of the node in this folder: storage (pruned or full), database, how the first start
# gets the chain, whether to share snapshots. Writes the [snapshot] and [storage] sections of
# config.ini (the previous file is kept as config.ini.bak-<date>) and can start the node.
#
#   ./setup.sh            asks, showing each default: Enter keeps it
#   ./setup.sh --defaults takes every default without asking
#
# Runs with the bash of Linux and of macOS (3.2).
set -eu

cd "$(dirname "$0")"

defaults=no
case "${1:-}" in
    --defaults|-y) defaults=yes ;;
    "") ;;
    *) echo "usage: $0 [--defaults]"; exit 2 ;;
esac

node=./node
config=config.ini
[ -x "$node" ] || { echo "No node binary here: run setup.sh from the csnode folder of the package."; exit 1; }
[ -f "$config" ] || { echo "No $config here: run setup.sh from the csnode folder of the package."; exit 1; }

# ask "question" default choice... -> sets $answer to one of the choices (the first letters are enough)
ask() {
    local question=$1 default=$2 reply c
    shift 2
    while :; do
        if [ "$defaults" = yes ]; then
            answer=$default
            echo "$question [$default]: $default"
            return
        fi
        printf '%s [%s]: ' "$question" "$default"
        read -r reply || reply=
        reply=$(printf '%s' "$reply" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')
        [ -n "$reply" ] || { answer=$default; return; }
        for c in "$@"; do
            case "$c" in "$reply"*) answer=$c; return ;; esac
        done
        echo "  Please answer one of: $*"
    done
}

# the backend of an existing database (as the node recognises it), empty when there is none
existing_backend() {
    if [ -f db/.backend ]; then tr -d '[:space:]' < db/.backend; return; fi
    if [ -f db/blockchain.db ] || [ -f db/sequence.db ]; then echo berkeleydb; return; fi
    if [ -f db/lmdb/data.mdb ]; then echo lmdb; return; fi
    if [ -f db/CURRENT ]; then echo rocksdb; return; fi
}

# the value of key in [section] of the current config.ini, empty when not set
current() {
    awk -v s="[$1]" -v k="$2" '
        /^[[:space:]]*\[/ { in_s = ($0 ~ "^[[:space:]]*\\[" substr(s, 2, length(s) - 2) "\\]") ; next }
        in_s && $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { sub(/^[^=]*=[[:space:]]*/, ""); sub(/[[:space:]]+$/, ""); v = $0 }
        END { print v }
    ' "$config" | tr -d '\r'
}

echo "Credits node set-up ($(cat VERSION 2>/dev/null || echo 'unknown version'))"
echo "Each question shows the default in brackets: press Enter to keep it."
echo

have=$(existing_backend)
if [ -n "$have" ]; then
    echo "This folder already holds a $have database: it is kept."
    echo
fi

# defaults from the current configuration, so that running the set-up again changes nothing by Enter
cur_backend=$(current storage db_backend)
[ -n "$cur_backend" ] || cur_backend=$(current storage block_db)
cur_prune=$(current storage prune_keep_blocks)
cur_serve=$(current snapshot serve)

# 1. pruned or full
storage_default=pruned
if [ -n "$cur_backend$cur_prune" ] && { [ -z "$cur_prune" ] || [ "$cur_prune" = 0 ]; }; then storage_default=full; fi
[ "$have" != berkeleydb ] || storage_default=full
echo "1. Storage"
echo "   pruned: keeps only recent blocks (a few GB), starts in minutes from a snapshot. Enough to run a node."
echo "   full:   keeps every block since the start of the chain (tens of GB), for history and explorers."
ask "   Pruned or full node? (pruned/full)" "$storage_default" pruned full
storage=$answer
if [ "$storage" = full ] && [ -n "$have" ] && [ -n "$cur_prune" ] && [ "$cur_prune" != 0 ]; then
    echo "   This folder holds a pruned database: its removed blocks are gone. To get a full node,"
    echo "   move db, caches and qs away and run the set-up again on the empty folder."
fi
echo

# 2. database
if [ "$storage" = pruned ]; then
    backend_default=lmdb
    [ "$cur_backend" != rocksdb ] && [ "$have" != rocksdb ] || backend_default=rocksdb
    echo "2. Database"
    echo "   lmdb:    the usual choice for pruned nodes."
    echo "   rocksdb: compressed, smaller on disk, a little more CPU."
    ask "   Database? (lmdb/rocksdb)" "$backend_default" lmdb rocksdb
else
    backend_default=berkeleydb
    case "$cur_backend" in lmdb|rocksdb) [ "$cur_prune" != "" ] && [ "$cur_prune" != 0 ] || backend_default=$cur_backend ;; esac
    [ -z "$have" ] || backend_default=$have
    echo "2. Database"
    echo "   berkeleydb: the official node's format (the official node can open this folder too)."
    echo "   rocksdb:    the smallest full node (about a third of berkeleydb)."
    echo "   lmdb:       fast, larger on disk."
    ask "   Database? (berkeleydb/rocksdb/lmdb)" "$backend_default" berkeleydb rocksdb lmdb
fi
backend=$answer
echo

convert=no
if [ -n "$have" ] && [ "$have" != "$backend" ]; then
    echo "   The database in this folder is $have, you chose $backend."
    if [ "$backend" = berkeleydb ] && [ "$storage" = pruned ]; then
        echo "   A pruned node cannot use berkeleydb."
    fi
    ask "   Convert it to $backend at the next start (it can take hours)? (yes/no)" yes yes no
    if [ "$answer" = yes ]; then
        convert=yes
    else
        backend=$have
        echo "   Keeping $have."
    fi
    echo
fi

# 3. how the first start gets the chain (only for an empty folder)
first=keep
if [ -z "$have" ]; then
    echo "3. First start"
    if [ "$backend" = berkeleydb ]; then
        echo "   A berkeleydb node syncs from block 0 (about 35 hours on mainnet)."
        first=sync
    else
        echo "   github:  download the snapshot published on GitHub now, check it and restore it (minutes)."
        echo "   network: the node fetches a snapshot from other nodes that share it; it needs at least two"
        echo "            of them, and few nodes share yet: without them it syncs from block 0 after 10 minutes."
        echo "   sync:    sync from block 0 by itself (about 35 hours on mainnet)."
        first_default=github
        if [ "$storage" = full ]; then
            echo "   A full node started from a snapshot holds the blocks from the snapshot on only."
            first_default=sync
        fi
        ask "   How should the node get the chain? (github/network/sync)" "$first_default" github network sync
        first=$answer
    fi
    echo
fi

# 4. sharing snapshots
serve_default=yes
[ "$cur_serve" != off ] || serve_default=no
echo "4. Sharing"
echo "   The node can share snapshots of the chain with new nodes, so that they start in minutes."
echo "   It uses at most 2 MB/s of upload, only while the node is synced, and about 200 MB of disk."
ask "   Share snapshots with other nodes? (yes/no)" "$serve_default" yes no
serve=$answer
echo

# 5. start now
ask "5. Start the node when the set-up is done? (yes/no)" yes yes no
start=$answer
echo

echo "Summary"
echo "  storage:      $storage"
echo "  database:     $backend$( [ "$convert" = yes ] && echo " (converted from $have at the next start)")"
case "$first" in
    keep)    echo "  first start:  the database in this folder is kept" ;;
    network) echo "  first start:  snapshot from other nodes" ;;
    github)  echo "  first start:  snapshot downloaded from GitHub now" ;;
    sync)    echo "  first start:  sync from block 0" ;;
esac
echo "  share:        $serve"
echo "  start now:    $start"
if [ "$defaults" = no ]; then
    ask "Write config.ini? (yes/no)" yes yes no
    [ "$answer" = yes ] || { echo "Nothing changed."; exit 0; }
fi

backup="$config.bak-$(date +%Y%m%d-%H%M%S)"
cp -p "$config" "$backup"

case "$first" in network) fetch=auto ;; *) fetch=off ;; esac
[ "$serve" = yes ] && serve_value=on || serve_value=off

# config.ini without its [snapshot] and [storage] sections, then the new ones ([snapshot] first)
tmp="$config.tmp.$$"
awk '
    /^[[:space:]]*\[/ { skip = ($0 ~ /^[[:space:]]*\[(snapshot|storage)\]/) }
    !skip && !/^; written by setup\.(sh|bat) on / { print }
' "$backup" | tr -d '\r' | awk 'NF { blank = 0; print; next } !blank++ { print }' > "$tmp"
{
    echo
    echo "; written by setup.sh on $(date '+%Y-%m-%d %H:%M'), the previous file is $backup"
    echo "[snapshot]"
    echo "serve=$serve_value"
    echo "fetch=$fetch"
    echo
    echo "[storage]"
    echo "db_backend=$backend"
    if [ "$storage" = pruned ]; then
        echo "checkpoint_every=100000"
        echo "checkpoint_keep=2"
        echo "prune_keep_blocks=1000"
    fi
} >> "$tmp"
mv "$tmp" "$config"
echo "config.ini written; the previous one is $backup"

if [ "$first" = github ]; then
    echo
    echo "Downloading the published snapshot..."
    if ! "$node" --config-file "$config" --download-snapshot; then
        echo "The download did not finish. Run ./setup.sh again, or start the node: it can get the snapshot from other nodes."
        exit 1
    fi
fi

args="--config-file $config"
[ "$convert" = no ] || args="$args --convert-db"
if [ "$start" = yes ]; then
    echo
    echo "Starting the node (Ctrl+C stops it and saves its state)..."
    # shellcheck disable=SC2086
    exec "$node" $args
fi
echo
echo "Start the node with: cd \"$(pwd)\" && $node $args"
