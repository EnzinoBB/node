#!/usr/bin/env bash
# Assemble a ready-to-run mainnet package of the community node, laid out like the official
# release (csnode/ with the node, the contract executor, its JDK and the configuration).
#
# Usage: make-package.sh <full|pruned> <version> <node binary> <executor dir> <jdk dir> <out dir> [known_hosts]
#   executor dir: holds contract-executor.jar and sc-api-support.jar
#   jdk dir:      the java-11-openjdk-11.0.4 directory the release ships (the executor runs and
#                 compiles contracts with it)
#   known_hosts:  optional peer list to ship instead of packaging/mainnet/known_hosts.txt
#
# Full and pruned packages hold the same binaries; only config.ini differs. Compared with the
# official config.ini: [event_report] is commented out (it reports node events to a fixed address),
# executor_commit_max accepts the community executor build, and the pruned package adds [storage].
set -euo pipefail

variant=$1 version=$2 node=$3 executor=$4 jdk=$5 out=$6 hosts=${7:-}
here=$(cd "$(dirname "$0")" && pwd)
case "$variant" in full|pruned) ;; *) echo "variant must be full or pruned" >&2; exit 2 ;; esac

stage=$(mktemp -d)
trap 'rm -rf "$stage"' EXIT
dir="$stage/csnode"
mkdir -p "$dir/ojdkbuild"

install -m 755 "$node" "$dir/node"
install -m 644 "$executor/contract-executor.jar" "$executor/sc-api-support.jar" "$dir/"
cp -a "$jdk" "$dir/ojdkbuild/java-11-openjdk-11.0.4"
install -m 644 "$here/mainnet/settings.properties" "$here/mainnet/default_confidants.txt" "$dir/"
install -m 644 "${hosts:-$here/mainnet/known_hosts.txt}" "$dir/known_hosts.txt"

# the node accepts an executor only within [executor_commit_min, executor_commit_max], and the
# executor reports its git commit count, which our fixes raised above the release's 1518
commits=$(unzip -p "$executor/contract-executor.jar" git.properties | sed -n 's/^git.total.commit.count=//p' | tr -d '\r')
[ -n "$commits" ] || { echo "no git.total.commit.count in contract-executor.jar" >&2; exit 1; }

awk -v commits="$commits" '
    /^\[/ { section = $0 }
    section == "[event_report]" && !/^;/ && NF { print ";" $0; next }
    /^executor_commit_max=/ { print "executor_commit_max=" commits; next }
    { print }
' "$here/mainnet/config.ini" > "$dir/config.ini"

if [ "$variant" = pruned ]; then
    cat >> "$dir/config.ini" <<'INI'

[storage]
; pruned storage: the node syncs from block 0 by itself and keeps only recent blocks
; (from the last checkpoint but one), in an LMDB database that reuses freed space
checkpoint_every=100000
checkpoint_keep=2
prune_keep_blocks=1000
block_db=lmdb
INI
fi

cat > "$dir/README-COMMUNITY.txt" <<EOF
Credits node, community build $version ($variant storage)

Built from https://github.com/EnzinoBB/node and https://github.com/EnzinoBB/contract-executor.
Node protocol version 532, compatible with the mainnet release 4.2.532.
Runs on: ${PACKAGE_RUNS_ON:-Linux x86-64 with glibc 2.38 or later (Ubuntu 24.04, Debian 13 and later)}.

Differences from the official package:
- config.ini: [event_report] is commented out; executor_commit_max=$commits accepts the
  community executor (build $commits instead of 1518).$( [ "$variant" = pruned ] && printf '\n- config.ini: [storage] runs the node with pruned storage on LMDB.' )
- The executor listens on 127.0.0.1 only (contract.executor.host in settings.properties
  changes it). Keep port 9080 closed to other hosts anyway.

Changing the block database (db_backend)
- The node recognises the format of an existing db/ folder. If config.ini asks for another one, it
  offers to convert it (from a terminal), or stops and explains how (without a terminal). Run
  ./node --convert-only to convert and exit, or ./node --convert-db to convert and start.
  Stop the node before converting. The conversion goes batch by batch and needs about 1 GB
  free beyond the database when converting from berkeleydb; about the size of the database
  again when converting from lmdb or rocksdb (their files do not shrink until the conversion
  ends). If interrupted it resumes at the next start. Checkpoints and the transaction index are kept, so the node quick-starts
  afterwards. The official node only reads berkeleydb: convert back before running it on this
  folder. In Docker, use CREDITS_CONVERT_DB=yes. Every [storage] setting and the conversion in
  detail: https://github.com/EnzinoBB/node/blob/master/docs/storage.md

Start: cd csnode && ./node
EOF

mkdir -p "$out"
# PACKAGE_PLATFORM names a build for another baseline, e.g. glibc2.31 for Ubuntu 20.04 and later
name="credits-node-$version-linux-x64${PACKAGE_PLATFORM:+-$PACKAGE_PLATFORM}-$variant.tar.gz"
tar -C "$stage" --owner=0 --group=0 --sort=name -czf "$out/$name" csnode
echo "$out/$name"
