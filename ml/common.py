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


# Reference price: the rolling median, over the REF_DAYS before t, of the prices
# an item was newly posted at. Built from listings, it would be set by walls
# of identical listings that sit for weeks and rarely sell (as the median of
# snapshot medians, or as the cheapest); listings priced exactly at it then
# showed a false dip in the demand curve. A wall counts once, when posted.
REF_DAYS = 30


def add_reference(lf, faction, t_col):
    """lf (lazy, with `item` and unix-time `t_col`) plus `ref`: the rolling
    median of the item's posting prices over the REF_DAYS before t_col, from
    the latest snapshot at or before it; the item's all-time posting median
    where it has none."""
    import polars as pl

    posts = (pl.scan_parquet(OUT / "transitions" / FACTION_NAMES[faction] / "*.parquet")
             .filter(pl.col("new") > 0, pl.col("buyout") > 0)
             .group_by("t1", "item").agg(post_med=(pl.col("buyout") / pl.col("count")).median())
             .with_columns(ts=pl.from_epoch("t1")).sort("ts"))
    rolling = (posts.rolling(index_column="ts", period=f"{REF_DAYS}d", group_by="item", closed="left")
               .agg(ref=pl.col("post_med").median()).drop_nulls("ref").sort("ts"))
    overall = posts.group_by("item").agg(ref_all=pl.col("post_med").median())
    return (lf.with_columns(ts=pl.from_epoch(t_col)).sort("ts")
            .join_asof(rolling, on="ts", by="item", strategy="backward")
            .join(overall, on="item", how="left")
            .with_columns(ref=pl.coalesce("ref", "ref_all")).drop("ts", "ref_all"))


# AzerothCore's item_template, for vendor sell prices (deposits, price floor).
AC_ROOT = Path(os.environ.get("AC_ROOT", ROOT.parent.parent))
ITEM_TEMPLATE_SQL = AC_ROOT / "data" / "sql" / "base" / "db_world" / "item_template.sql"
