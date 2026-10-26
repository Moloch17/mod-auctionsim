#!/bin/bash
# Fetches the current Lordaeron scan for each faction from ah.nerfed.net into
# scans/ (cron: every 6 hours). A failed or empty download leaves nothing
# behind, so an error page never ends up looking like a scan.

cd "$(dirname "$0")" || exit 1
stamp=$(date +%Y-%m-%d_%H)
status=0
for pair in horde:1 alliance:2; do
    name=${pair%%:*}
    out="scans/lordaeron_${name}_${stamp}.lua"
    tmp="$out.part"
    if curl -fsS --retry 3 --retry-delay 30 -o "$tmp" "https://ah.nerfed.net/realm/getfile?id=14&faction=${pair##*:}" \
        && [ -s "$tmp" ] && head -c 64 "$tmp" | grep -q '^AucScanData'; then
        mv "$tmp" "$out"
    else
        echo "dl-data: $name download failed" >&2
        rm -f "$tmp"
        status=1
    fi
done
exit $status
