#!/usr/bin/env bash
# Checks that an LMDB environment is whole: every database in it reads through with mdb_dump and
# yields as many records as mdb_stat says it holds. mdb_dump alone is not enough: a page zeroed on
# disk reads as an empty tree, with no error.
#
# Usage: lmdb-check.sh <dir with mdb_dump and mdb_stat> <environment>
# Exit 0 if whole, 1 with a reason on stderr otherwise.
set -euo pipefail

tools=$1 env=$2
mdb_dump=$tools/mdb_dump mdb_stat=$tools/mdb_stat
[ -x "$mdb_dump" ] && [ -x "$mdb_stat" ] || { echo "no mdb_dump and mdb_stat in $tools" >&2; exit 1; }

stats=$("$mdb_stat" -a "$env" 2>/dev/null) || { echo "$env: mdb_stat cannot read it" >&2; exit 1; }
counts=$(awk '/^Status of Main DB/{n="-"; next} /^Status of /{sub(/^Status of /, ""); n=$0; next}
              /Entries:/ && n != "" {print n "\t" $2; n=""}' <<<"$stats")
grep -q $'^-\t' <<<"$counts" || { echo "$env: no main database" >&2; exit 1; }
while IFS=$'\t' read -r name expect; do
    if [ "$name" = - ]; then
        dump=$("$mdb_dump" "$env" 2>/dev/null) || { echo "$env: the main database does not read through" >&2; exit 1; }
    else
        dump=$("$mdb_dump" -s "$name" "$env" 2>/dev/null) || { echo "$env: database $name does not read through" >&2; exit 1; }
    fi
    got=$(awk '/^HEADER=END/{d=1; next} /^DATA=END/{d=0} d{n++} END{print int(n/2)}' <<<"$dump")
    [ "$got" = "$expect" ] || { echo "$env: database ${name/#-/main} reads $got records, its header says $expect" >&2; exit 1; }
done <<<"$counts"
