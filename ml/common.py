"""Paths and constants shared by the ml/ stages.

Inputs are read only: ../data/scans (dl-data.sh) and ../nerfed/raw
(nerfed/scrape.py). Everything the stages write goes under ml/out/.
"""

import os
from pathlib import Path

ML = Path(__file__).resolve().parent
ROOT = ML.parent
SCANS = ROOT / "data" / "scans"
NERFED = ROOT / "nerfed" / "raw"
OUT = ML / "out"

# AuctionHouseId, as in data/compile-data.cpp and the module.
ALLIANCE, HORDE = 2, 6
FACTION_NAMES = {ALLIANCE: "alliance", HORDE: "horde"}

# Auctioneer TLEFT buckets (3.3.5): upper bound of the time left, in hours.
TLEFT_MAX_HOURS = {1: 0.5, 2: 2.0, 3: 12.0, 4: 48.0}

# The box also runs the live worldserver: cap worker processes, and polars
# threads per worker (set before any `import polars`).
WORKERS = int(os.environ.get("ML_WORKERS", "8"))
os.environ.setdefault("POLARS_MAX_THREADS", "3")


def pool():
    """A worker pool started with spawn: forking after polars has started its
    thread pool deadlocks the children."""
    import multiprocessing
    return multiprocessing.get_context("spawn").Pool(WORKERS)
