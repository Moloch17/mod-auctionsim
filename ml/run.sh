#!/bin/bash
# Runs the ml/ pipeline in order. Every stage reads what the one before wrote
# under ml/out/ and only reads ../data/scans and ../nerfed/raw. Stages 1a and 2
# skip work already done; the others rebuild their outputs.
#
#   ./run.sh            all stages
#   ./run.sh 3 4 5      just these
#
# Stage 6 writes the file Market mode ships, out/s6/auctionsim_market.dat; copy
# it to ../data/auctionsim_market.dat to ship it. Stage 7 tests it: if the
# learned offsets pass, re-export with s6_export.py --offsets out/s7/offsets.parquet.
#
# Setup once: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# (polars[rtcompat]: this box's Xeons lack AVX2). ML_WORKERS caps processes (8).
# Stage 0 reads AzerothCore's item_template.sql: set AC_ROOT when ml/ is not
# inside modules/ of an AzerothCore tree.
set -e
cd "$(dirname "$0")"
PY="nice -n 10 .venv/bin/python"
stages=${*:-0 1 2 3 4 5 6 7}
for s in $stages; do
    case $s in
        0) $PY s0_item_template.py ;;
        1) $PY s1_extract_scans.py && $PY s1_extract_nerfed.py ;;
        2) $PY s2_transitions.py ;;
        3) $PY s3_sellers.py ;;
        4) $PY s4_demand.py ;;
        5) $PY s5_targets.py && $PY s5_tune.py ;;
        6) $PY s6_tune.py ;;
        7) $PY s7_exploit.py && $PY s7_rl.py ;;
    esac
done
