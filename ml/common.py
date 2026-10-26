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


# Cadence calibration. Daily snapshots miss what is posted and gone within the
# gap, so daily-era rates read low. Over the 6-hourly era, the same days seen
# both ways -- every pair (gap <= SHORT_GAP_H) and the thinned day-apart pairs
# stage 2 writes (out/transitions_thin) -- give a factor per faction and item
# class, short-gap rate over day-gap rate. Daily-era rows are scaled by it.
SHORT_GAP_H = 7.0       # pairs at most this far apart are short-gap
DAILY_GAP_H = (20, 28)  # pairs this far apart are daily
WEIGHT_GAP_H = 13.0     # a pair further apart than this counts at daily cadence
FACTOR_CLIP = (0.5, 4.0)
FACTOR_MIN_N = 2000     # day-gap events per class before a class gets its own factor


def cadence_factors(short, thin, value, itype_of):
    """Per (faction, itype) factor short-rate / thin-rate of `value` summed per
    pair hour, over the span the thinned pairs cover. short and thin are lazy
    transitions-like frames with faction, t0, t1, gap_h, item and `value`;
    itype_of maps item -> itype. Classes with fewer than FACTOR_MIN_N on the
    thinned side take the faction's pooled factor (itype null in the output).
    Returns a DataFrame faction, itype, factor, n_short, n_thin."""
    import polars as pl

    span = thin.group_by("faction").agg(lo=pl.col("t0").min(), hi=pl.col("t1").max())
    short = short.join(span, on="faction").filter(pl.col("t0") >= pl.col("lo"), pl.col("t1") <= pl.col("hi"))

    def rates(lf, by):
        hours = lf.select("faction", "t0", "gap_h").unique().group_by("faction").agg(h=pl.col("gap_h").sum())
        return (lf.join(itype_of.lazy(), on="item", how="left").group_by(by).agg(v=value.sum())
                .join(hours, on="faction").with_columns(rate=pl.col("v") / pl.col("h")))

    out = []
    for by in (["faction", "itype"], ["faction"]):
        s = rates(short, by).select(*by, n_short=pl.col("v"), r_short=pl.col("rate"))
        t = rates(thin, by).select(*by, n_thin=pl.col("v"), r_thin=pl.col("rate"))
        f = (s.join(t, on=by).with_columns(factor=(pl.col("r_short") / pl.col("r_thin")).clip(*FACTOR_CLIP))
             .collect())
        if by == ["faction"]:
            f = f.with_columns(itype=pl.lit(None, pl.String))
        else:
            f = f.filter(pl.col("n_thin") >= FACTOR_MIN_N)
        out.append(f.select("faction", "itype", "factor", "n_short", "n_thin"))
    return pl.concat(out)


def apply_factor(lf, factors, gap_col="gap_h"):
    """lf (with faction, itype, gap_col) plus `w`: 1 for rows from pairs at most
    WEIGHT_GAP_H apart, else the class's cadence factor (the faction's pooled
    one where the class has none)."""
    import polars as pl

    per = factors.filter(pl.col("itype").is_not_null()).select("faction", "itype", f_cls="factor")
    pooled = factors.filter(pl.col("itype").is_null()).select("faction", f_all="factor")
    return (lf.join(per.lazy(), on=["faction", "itype"], how="left").join(pooled.lazy(), on="faction", how="left")
            .with_columns(w=pl.when(pl.col(gap_col) <= WEIGHT_GAP_H).then(1.0)
                          .otherwise(pl.coalesce("f_cls", "f_all", pl.lit(1.0))))
            .drop("f_cls", "f_all"))
