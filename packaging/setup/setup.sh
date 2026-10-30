#!/usr/bin/env bash
# Guided set-up of the node in this folder: storage (pruned or full), database, how the first start
# gets the chain, whether to share snapshots, which contract executor to run. Writes the [snapshot]
# and [storage] sections of config.ini and the executor builds it accepts (the previous file is
# kept as config.ini.bak-<date>), can replace the executor jars, and can start the node.
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

# --- contract executor ---------------------------------------------------------------------------
# The node runs smart contracts with contract-executor.jar (it loads sc-api-support.jar from the same
# folder) and starts it only if the build the jar reports (its git commit count) is within
# [executor_commit_min, executor_commit_max] of [api]; otherwise it stops the executor and runs no
# contract. The official release is build 1518; community builds add fixes on top and count higher.
official_build=1518
official_url=https://github.com/CREDITSCOM/node/releases/download/mainnet-4.2.532.0/Ubuntu20_Mainnet-4.2.532.0.tar.gz
official_sha_executor=592510d1586818348c65bea9b443e7f58aa6947113e43aa8906af8426aeef028
official_sha_support=395d9a108a66fe1d92c1d2096a9d11f4c7662b51b47af792db0ff4cc91df9807
community_url=https://github.com/EnzinoBB/contract-executor/releases/latest/download

# the java of executor_command, to find the JDK's jar tool when neither unzip nor python3 is there
executor_java() {
    current api executor_command | awk '{ print $1 }'
}

# the build (git commit count) an executor jar reports, empty when it cannot be read
jar_build() {
    local jar=$1 props="" java tmp
    [ -f "$jar" ] || return 0
    if command -v unzip >/dev/null 2>&1; then
        props=$(unzip -p "$jar" git.properties 2>/dev/null || true)
    elif command -v python3 >/dev/null 2>&1; then
        props=$(python3 -c 'import sys, zipfile; print(zipfile.ZipFile(sys.argv[1]).read("git.properties").decode())' "$jar" 2>/dev/null || true)
    else
        java=$(executor_java)
        case "$java" in /*) ;; *) java="$(pwd)/$java" ;; esac
        if [ -n "$java" ] && [ -x "$(dirname "$java")/jar" ]; then
            tmp=$(mktemp -d)
            (cd "$tmp" && "$(dirname "$java")/jar" xf "$OLDPWD/$jar" git.properties 2>/dev/null) || true
            props=$(cat "$tmp/git.properties" 2>/dev/null || true)
            rm -rf "$tmp"
        fi
    fi
    printf '%s\n' "$props" | sed -n 's/^git.total.commit.count=//p' | tr -d '\r'
}

sha256() {
    if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | awk '{ print $1 }'; else shasum -a 256 "$1" | awk '{ print $1 }'; fi
}

describe_build() {
    case "$1" in
        "") echo "none or unreadable" ;;
        "$official_build") echo "build $1 (official)" ;;
        *) [ "$1" -gt "$official_build" ] 2>/dev/null && echo "build $1 (community)" || echo "build $1" ;;
    esac
}

# downloads the jars of the chosen executor into $1; prints the reason and fails when it cannot
download_executor() {
    local kind=$1 dir=$2
    command -v curl >/dev/null 2>&1 || { echo "   curl is not installed."; return 1; }
    if [ "$kind" = community ]; then
        curl -fsSL -o "$dir/contract-executor.jar" "$community_url/contract-executor.jar" \
            && curl -fsSL -o "$dir/sc-api-support.jar" "$community_url/sc-api-support.jar" \
            && curl -fsSL -o "$dir/SHA256SUMS" "$community_url/SHA256SUMS" \
            || { echo "   The download from GitHub failed."; return 1; }
        for f in contract-executor.jar sc-api-support.jar; do
            grep -q "^$(sha256 "$dir/$f")  *\*\{0,1\}$f\$" "$dir/SHA256SUMS" \
                || { echo "   $f does not match the SHA256SUMS of the release."; return 1; }
        done
    else
        echo "   The jars come from the official release package (about 350 MB to download, only they are kept)."
        curl -fsSL "$official_url" | tar -xzf - -C "$dir" csnode/contract-executor.jar csnode/sc-api-support.jar \
            || { echo "   The download of the official release failed."; return 1; }
        mv "$dir/csnode/contract-executor.jar" "$dir/csnode/sc-api-support.jar" "$dir/"
        [ "$(sha256 "$dir/contract-executor.jar")" = "$official_sha_executor" ] \
            && [ "$(sha256 "$dir/sc-api-support.jar")" = "$official_sha_support" ] \
            || { echo "   The jars of the official release do not have the expected checksums."; return 1; }
    fi
}

# sets key=value in [api] of config.ini, adding the key at the end of the section when missing
set_api_key() {
    local tmp="$config.tmp.$$"
    awk -v k="$1" -v v="$2" '
        /^[[:space:]]*\[/ { if (in_api && !done) { print k "=" v; done = 1 } in_api = ($0 ~ /^[[:space:]]*\[api\]/) }
        in_api && $0 ~ "^[[:space:]]*" k "[[:space:]]*=" { if (!done) print k "=" v; done = 1; next }
        { print }
        END { if (in_api && !done) print k "=" v }
    ' "$config" > "$tmp" && mv "$tmp" "$config"
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

# 5. the contract executor (when config.ini runs one)
executor_choice=none
executor_action=keep
if [ -n "$(current api executor_command)" ]; then
    have_build=$(jar_build contract-executor.jar)
    support_build=$(jar_build sc-api-support.jar)
    cur_min=$(current api executor_commit_min)
    cur_max=$(current api executor_commit_max)
    echo "5. Contract executor"
    echo "   The node runs smart contracts with contract-executor.jar and sc-api-support.jar, and starts"
    echo "   the executor only if its build is within executor_commit_min..executor_commit_max of config.ini."
    echo "   In this folder: $(describe_build "$have_build"); config.ini accepts ${cur_min:-1506}..${cur_max:-any}."
    if [ -n "$have_build" ] && [ -n "$support_build" ] && [ "$have_build" != "$support_build" ]; then
        echo "   sc-api-support.jar is $(describe_build "$support_build"): the two jars come from different packages."
    fi
    echo "   community: the latest community build, the official one with fixes (code of a contract that never"
    echo "              returns is stopped, tokens of every standard are reported to the API). Contracts give"
    echo "              the same results as with the official one."
    echo "   official:  build $official_build of the official release."
    # unattended (--defaults), nothing is downloaded: the jars in the folder are kept
    download_default=yes
    [ "$defaults" = no ] || download_default=no
    executor_default=official
    if [ -n "$have_build" ] && [ "$have_build" -gt "$official_build" ] 2>/dev/null; then executor_default=community; fi
    ask "   Which executor? (community/official)" "$executor_default" community official
    executor_choice=$answer
    if [ "$executor_choice" = community ] && { [ -z "$have_build" ] || [ "$have_build" -le "$official_build" ] 2>/dev/null; }; then
        echo "   The jars in this folder are not a community build (copying only the node binary keeps the old ones)."
        ask "   Download the latest community executor from GitHub now (about 25 MB)? (yes/no)" "$download_default" yes no
        [ "$answer" = yes ] && executor_action=download || executor_action=manual
    elif [ "$executor_choice" = official ] && [ "$have_build" != "$official_build" ]; then
        if [ -f contract-executor.jar.official ] && [ -f sc-api-support.jar.official ]; then
            echo "   The official jars set aside by an earlier set-up are here: they are put back."
            executor_action=restore
        else
            ask "   Download the official executor now (from the official release, about 350 MB)? (yes/no)" "$download_default" yes no
            [ "$answer" = yes ] && executor_action=download || executor_action=manual
        fi
    fi
    echo
fi

# 6. start now
ask "6. Start the node when the set-up is done? (yes/no)" yes yes no
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
case "$executor_choice/$executor_action" in
    none/*)            ;;
    */keep)            echo "  executor:     $executor_choice, the jars in this folder are kept" ;;
    */download)        echo "  executor:     $executor_choice, downloaded now (the jars in this folder are set aside)" ;;
    */restore)         echo "  executor:     official, the jars set aside before are put back" ;;
    */manual)          echo "  executor:     $executor_choice, to copy by hand (see below)" ;;
esac
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

# the executor: put the chosen jars in place, then make config.ini accept their build
if [ "$executor_choice" != none ]; then
    stamp=$(date +%Y%m%d-%H%M%S)
    # keeps the jars in this folder aside: as .official when they are the official ones (a later
    # set-up puts them back without a download), as .bak-<date> otherwise
    set_aside() {
        local suffix=".bak-$stamp"
        [ "$(jar_build contract-executor.jar)" != "$official_build" ] || suffix=.official
        for f in contract-executor.jar sc-api-support.jar; do
            [ ! -f "$f" ] || mv "$f" "$f$suffix"
        done
        echo "The previous executor jars are kept as contract-executor.jar$suffix and sc-api-support.jar$suffix."
    }
    case "$executor_action" in
        download)
            echo
            echo "Downloading the $executor_choice executor..."
            dl=$(mktemp -d)
            if download_executor "$executor_choice" "$dl"; then
                set_aside
                mv "$dl/contract-executor.jar" "$dl/sc-api-support.jar" .
                echo "contract-executor.jar and sc-api-support.jar replaced: $(describe_build "$(jar_build contract-executor.jar)")."
            else
                executor_action=manual
                echo "The jars in this folder are kept."
            fi
            rm -rf "$dl"
            ;;
        restore)
            set_aside
            mv contract-executor.jar.official contract-executor.jar
            mv sc-api-support.jar.official sc-api-support.jar
            echo "The official executor jars are back."
            ;;
    esac
    build=$(jar_build contract-executor.jar)
    if [ -n "$build" ]; then
        min=$(current api executor_commit_min)
        max=$(current api executor_commit_max)
        if [ -n "$min" ] && [ "$build" -lt "$min" ] 2>/dev/null; then set_api_key executor_commit_min "$build"; echo "config.ini: executor_commit_min=$build"; fi
        if [ -n "$max" ] && [ "$max" != -1 ] && [ "$build" -gt "$max" ] 2>/dev/null; then set_api_key executor_commit_max "$build"; echo "config.ini: executor_commit_max=$build (it was $max: the node would not start this executor)"; fi
    fi
    if [ "$executor_action" = manual ]; then
        echo
        echo "To use the $executor_choice executor, with the node stopped:"
        if [ "$executor_choice" = community ]; then
            echo "  copy contract-executor.jar and sc-api-support.jar, both, from the csnode folder of the community"
            echo "  package (or from $community_url/...) into $(pwd),"
        else
            echo "  copy contract-executor.jar and sc-api-support.jar, both, from the official release"
            echo "  ($official_url) into $(pwd),"
        fi
        echo "  then run this set-up again: it makes config.ini accept their build (executor_commit_min/max)."
    fi
    echo
    echo "Updating this folder later: a new node binary is not enough when its package brings a new executor."
    echo "Copy node, contract-executor.jar and sc-api-support.jar together, then run the set-up again."
fi

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
