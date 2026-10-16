#!/bin/sh
# Healthy while the last written block advances: fails when it has not changed for 10 minutes.
seq=$(cat $(ls -t /data/log/log_*.txt 2>/dev/null | head -2) 2>/dev/null \
      | grep -a 'Last written sequence = ' | tail -1 | sed -E "s/.*Last written sequence = ([0-9']+).*/\1/" | tr -d "'")
[ -n "$seq" ] || exit 0  # still starting
state=/tmp/credits-health
now=$(date +%s)
if [ -f "$state" ] && [ "$(cut -d' ' -f1 "$state")" = "$seq" ]; then
    [ $((now - $(cut -d' ' -f2 "$state"))) -lt 600 ] || { echo "block $seq for 10 minutes"; exit 1; }
else
    echo "$seq $now" > "$state"
fi
exit 0
