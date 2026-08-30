#!/bin/bash
# Runs compile-data.lua across multiple worker processes, sharding data/scans/ by file,
# then merges the per-shard accumulators into the final auctionsim.dat.
#
# Usage: ./compile-data-parallel.sh [shard-count]   (defaults to `nproc`)
set -euo pipefail
cd "$(dirname "$0")"

shards="${1:-$(nproc)}"
tmpdir=$(mktemp -d "${TMPDIR:-/tmp}/compile-data.XXXXXX")
trap 'rm -rf "$tmpdir"' EXIT

pids=()
for ((i = 0; i < shards; i++)); do
    lua compile-data.lua --worker "$i" "$shards" "$tmpdir/partial-$i.dat" &
    pids+=($!)
done

for pid in "${pids[@]}"; do
    wait "$pid"
done

partials=()
for ((i = 0; i < shards; i++)); do
    partials+=("$tmpdir/partial-$i.dat")
done

lua compile-data.lua --merge auctionsim.dat "${partials[@]}"

echo "Complete"
