#!/usr/bin/env bash
# The weekly verified known_hosts list of .github/workflows/known-hosts.yml, run on any host with
# Docker instead of GitHub Actions (for example when Actions is unavailable).
#
# The pruned package of the latest community release joins mainnet for MINUTES minutes from an
# empty database with a throwaway key, in a CPU- and memory-limited container. collect.py lists the
# peers that served the oldest blocks; the report goes to the rolling release "known-hosts" and the
# list is proposed as a pull request updating packaging/mainnet/known_hosts.txt, for review.
#
# Usage: run-weekly.sh
# Env:   WORK (default ~/Projects/credits/known-hosts-run), MINUTES (25), REPO (EnzinoBB/node),
#        CPUS (1.0), MEMORY (3g), PUBLISH (1)
# Needs: docker, gh (authenticated), python3 with base58 and pynacl.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
WORK=${WORK:-$HOME/Projects/credits/known-hosts-run}
MINUTES=${MINUTES:-25}
REPO=${REPO:-EnzinoBB/node}
CPUS=${CPUS:-1.0}
MEMORY=${MEMORY:-3g}
PUBLISH=${PUBLISH:-1}
container=credits-known-hosts

mkdir -p "$WORK"
exec > >(tee "$WORK/last-run.log") 2>&1
echo "== $(date -Is) known hosts run, $MINUTES min"

# a previous run that was interrupted leaves its container and files behind
docker rm -f "$container" >/dev/null 2>&1 || true
rm -rf "${WORK:?}/run"
mkdir -p "$WORK/run"
cd "$WORK/run"

tag=$(gh release list --repo "$REPO" --limit 50 --json tagName,isDraft \
    --jq '[.[] | select(.isDraft | not) | .tagName | select(startswith("community-v"))][0]')
echo "$tag" | grep -qE '^community-v[0-9A-Za-z._-]+$' || { echo "no community release found"; exit 1; }
gh release download "$tag" --repo "$REPO" --pattern '*-linux-x64.tar.gz' --pattern '*-linux-x64-pruned.tar.gz' --pattern SHA256SUMS
sha256sum -c --ignore-missing SHA256SUMS
tar -xzf "$(ls ./*-linux-x64.tar.gz ./*-linux-x64-pruned.tar.gz 2>/dev/null | head -1)"
rm -f ./*-linux-x64.tar.gz ./*-linux-x64-pruned.tar.gz
# one package since community.14: the pruned configuration is config.pruned.ini
[ ! -f csnode/config.pruned.ini ] || cp csnode/config.pruned.ini csnode/config.ini
echo "package $tag"

# start from every peer we know: the shipped list, the last published one and the repository's
gh release download known-hosts --repo "$REPO" --pattern known_hosts.txt --output previous.txt 2>/dev/null || : > previous.txt
cat csnode/known_hosts.txt previous.txt "$root/packaging/mainnet/known_hosts.txt" \
    | awk 'NF == 2 && !seen[$2]++' > seed.txt
cp seed.txt csnode/known_hosts.txt
echo "seed: $(wc -l < seed.txt) peers"

# one executor per container: skip the jps check for other executors
sed -i 's/^executor_multi_instance=.*/executor_multi_instance=true/' csnode/config.ini
python3 "$root/ci/testnet/gen_keys.py" 1 keys >/dev/null
cp keys/n1/NodePublic.txt keys/n1/NodePrivate.txt csnode/

docker run -d --name "$container" --cpus "$CPUS" --memory "$MEMORY" --cpu-shares 256 --user "$(id -u):$(id -g)" \
    -v "$WORK/run/csnode:/node" -w /node ubuntu:24.04 \
    ./node --db-path db --config-file config.ini >/dev/null
sleep $((MINUTES * 60))
docker kill -s INT "$container" >/dev/null 2>&1 || true
for _ in $(seq 1 60); do [ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" = true ] || break; sleep 1; done
docker rm -f "$container" >/dev/null 2>&1 || true
grep -ah 'Last written sequence' csnode/log/log_*.txt | tail -1 || true

python3 "$root/ci/known-hosts/collect.py" csnode out
cat out/peers.md
listed=$(wc -l < out/known_hosts.txt)
# the database and the package are not needed any more; keep the logs of the last run
rm -rf csnode/db csnode/caches csnode/ojdkbuild
if [ "$listed" -lt 5 ]; then
    echo "only $listed peers verified, not publishing"
    exit 1
fi
[ "$PUBLISH" = 1 ] || { echo "PUBLISH=0, not publishing"; exit 0; }

{
    echo "Peers verified on $(date -u +%Y-%m-%d) by a node that joined mainnet from an empty database,"
    echo "running the pruned package of $tag."
    echo
    cat out/peers.md
} > notes.md
if ! gh release view known-hosts --repo "$REPO" >/dev/null 2>&1; then
    gh release create known-hosts --repo "$REPO" --title "Known hosts" --notes-file notes.md --latest=false
else
    gh release edit known-hosts --repo "$REPO" --notes-file notes.md
fi
gh release upload known-hosts --repo "$REPO" --clobber out/known_hosts.txt out/peers.md out/peers.json

# propose the list for review: a branch from the default branch with only this file changed
default=$(gh repo view "$REPO" --json defaultBranchRef --jq .defaultBranchRef.name)
branch=auto/known-hosts
clone="$WORK/run/repo"
git clone -q --depth 1 --branch "$default" "https://github.com/$REPO.git" "$clone"
cp out/known_hosts.txt "$clone/packaging/mainnet/known_hosts.txt"
cd "$clone"
if git diff --quiet; then
    echo "known_hosts.txt unchanged"
    exit 0
fi
git checkout -q -B "$branch"
git -c user.name="known-hosts bot" -c user.email="known-hosts@users.noreply.github.com" \
    commit -qam "packaging: known hosts verified on $(date -u +%Y-%m-%d)"
git push -q -f origin "$branch"
if gh pr view "$branch" --repo "$REPO" --json state --jq .state 2>/dev/null | grep -q OPEN; then
    gh pr edit "$branch" --repo "$REPO" --body-file "$WORK/run/notes.md"
else
    gh pr create --repo "$REPO" --head "$branch" --base "$default" \
        --title "packaging: update known hosts" --body-file "$WORK/run/notes.md"
fi
echo "== $(date -Is) done"
