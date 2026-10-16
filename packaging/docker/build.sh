#!/usr/bin/env bash
# Build the image from an unpacked release package: build.sh <csnode dir> <image tag>
set -euo pipefail
package=$(cd "$1" && pwd) tag=$2
here=$(cd "$(dirname "$0")" && pwd)
ctx=$(mktemp -d)
trap 'rm -rf "$ctx"' EXIT
cp -a "$package" "$ctx/package"
mkdir "$ctx/docker"
cp "$here"/entrypoint.sh "$here"/keygen.py "$here"/healthcheck.sh "$here"/snapshot-restore.sh "$ctx/docker/"
docker build -f "$here/Dockerfile" -t "$tag" "$ctx"
