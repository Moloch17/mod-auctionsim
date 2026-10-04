#!/bin/bash

cd "$(dirname "$0")"
curl -o "scans/lordaeron_horde_$(date +%Y-%m-%d_%H).lua" "https://ah.nerfed.net/realm/getfile?id=14&faction=1"
curl -o "scans/lordaeron_alliance_$(date +%Y-%m-%d_%H).lua" "https://ah.nerfed.net/realm/getfile?id=14&faction=2"
