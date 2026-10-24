#!/usr/bin/env bash
# Start a stopped node once without peers, so that its start-up STATE DIGEST line is the digest of
# its head, then stop it: digest_offline.sh <node dir> [timeout seconds]
# The node runs as ./node in <node dir> with a copy of config.ini that has no hosts and another p2p
# port, so that no peer reaches it and it writes no block. Prints the digest line; exit 1 if none.
set -euo pipefail
dir=$(cd "$1" && pwd) timeout=${2:-600}
cd "$dir"
# an empty hosts file stops the node ("No hosts specified"): one host nobody listens on
echo "127.0.0.1:9 11111111111111111111111111111111" > hosts-none.txt
awk '
    /^\[/ { section = $0 }
    /^hosts_filename=/ { print "hosts_filename=hosts-none.txt"; next }
    section == "[host_input]" && /^port=/ { print "port=19510"; next }
    section == "[api]" && /^port=/ { print "port=0"; next }
    { print }' config.ini > config-offline.ini
# the node's console output of this run (the packaged config.ini logs info to the console): a count
# of digest lines in log/ can go down when the start rotates the logs and removes the oldest file
digests() { grep -a 'STATE DIGEST #' offline.out 2>/dev/null || true; }
./node --db-path db --config-file config-offline.ini </dev/null > offline.out 2>&1 &
pid=$!
deadline=$(( $(date +%s) + timeout ))
until [ -n "$(digests)" ] || [ "$(date +%s)" -ge "$deadline" ]; do
    kill -0 "$pid" 2>/dev/null || break
    sleep 2
done
kill -TERM "$pid" 2>/dev/null || true
for _ in $(seq 1 120); do kill -0 "$pid" 2>/dev/null || break; sleep 1; done
kill -0 "$pid" 2>/dev/null && { kill -KILL "$pid"; echo "the offline node did not stop in 120 s" >&2; exit 1; }
pkill -f "[c]ontract-executor[.]jar" 2>/dev/null || true
rm -f config-offline.ini hosts-none.txt
line=$(digests | tail -1)
[ -n "$line" ] || { echo "no start-up digest" >&2; exit 1; }
echo "STATE DIGEST #${line#*STATE DIGEST #}"
