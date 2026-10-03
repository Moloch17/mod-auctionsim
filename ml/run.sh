#!/bin/bash
# Runs the ml/ pipeline in order. Every stage reads what the one before wrote
# under ml/out/ and only reads ../data/scans and ../nerfed/raw. Stages 1a and 2
# skip work already done; the others rebuild their outputs.
#
#   ./run.sh            all stages
#   ./run.sh 3 4 5      just these
#
# Setup once: python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
# (polars[rtcompat]: this box's Xeons lack AVX2). ML_WORKERS caps processes (8).
set -e
cd "$(dirname "$0")"
PY="nice -n 10 .venv/bin/python"
stages=${*:-1 2 3 4 5}
for s in $stages; do
    case $s in
        1) $PY s1_extract_scans.py && $PY s1_extract_nerfed.py ;;
        2) $PY s2_transitions.py ;;
        3) $PY s3_sellers.py ;;
        4) $PY s4_demand.py ;;
        5) $PY s5_targets.py && $PY s5_sim.py ;;
    esac
done
