#!/usr/bin/env python3
"""Stage 5a: what the simulated market has to reproduce, from nerfed history.

Input: out/nerfed_history.parquet (stage 1b), out/s3/items.parquet.

The site keeps every 6-hourly point for ~30 days and thins older ones, so
every statistic is taken on one point per item per day (the day's last) to
keep windows comparable. Three windows per faction: `holdout` (the last
WINDOW_DAYS), `tune` (the WINDOW_DAYS before it) and `train` (the rest). The
simulator's demand scale is tuned on `tune` and scored once on `holdout`.
window_start is the window's first instant (unix s), where a simulation of it
starts.

Per faction x window x item (out/s5/targets.parquet):
  days         days the item was on the AH at the site's snapshot
  presence     days / days the site has any data in the window
  units_med    median units on the AH
  price_med    median of the daily median per-unit buyout (copper)
  min_med      median of min/median buyout: how far the cheapest sits below
  vol          std of day-to-day log change of the median buyout
  wd_0..wd_6   units by weekday (Monday = 0) over the item's mean
out/s5/targets_class.parquet: the same, medians per item class and quality.
"""

import polars as pl

from common import OUT

S5 = OUT / "s5"
WINDOW_DAYS = 90


def main():
    S5.mkdir(parents=True, exist_ok=True)
    h = pl.read_parquet(OUT / "nerfed_history.parquet")
    items = pl.read_parquet(OUT / "s3" / "items.parquet").select("item", "itype", "quality")
    daily = (h.with_columns(day=pl.from_epoch("time").dt.date())
             .sort("time").group_by("faction", "item", "day").agg(pl.all().last())
             .sort("faction", "item", "day"))
    end = daily.group_by("faction").agg(end=pl.col("day").max())
    daily = (daily.join(end, on="faction")
             .with_columns(window=pl.when(pl.col("day") > pl.col("end") - pl.duration(days=WINDOW_DAYS))
                           .then(pl.lit("holdout"))
                           .when(pl.col("day") > pl.col("end") - pl.duration(days=2 * WINDOW_DAYS))
                           .then(pl.lit("tune")).otherwise(pl.lit("train"))))
    starts = daily.group_by("faction", "window").agg(
        window_start=(pl.col("day").min().cast(pl.Datetime("ms")).dt.epoch("s")))
    site_days = daily.group_by("faction", "window").agg(site_days=pl.col("day").n_unique())
    daily = daily.with_columns(
        ret=(pl.col("buyout_median").log() - pl.col("buyout_median").log().shift(1)).over("faction", "item", "window"),
        wd=pl.col("day").dt.weekday() - 1,
        rel_units=pl.col("quantity") / pl.col("quantity").mean().over("faction", "item", "window"),
    )
    t = (daily.group_by("faction", "window", "item").agg(
            days=pl.len(), units_med=pl.col("quantity").median(), price_med=pl.col("buyout_median").median(),
            min_med=(pl.col("buyout_min") / pl.col("buyout_median")).median(), vol=pl.col("ret").std(),
            *[pl.col("rel_units").filter(pl.col("wd") == d).mean().alias(f"wd_{d}") for d in range(7)])
         .join(site_days, on=["faction", "window"])
         .join(starts, on=["faction", "window"])
         .with_columns(presence=pl.col("days") / pl.col("site_days"))
         .join(items, on="item", how="left"))
    t.write_parquet(S5 / "targets.parquet")
    c = (t.filter(pl.col("days") >= 10).group_by("faction", "window", "itype", "quality").agg(
            items=pl.len(), *[pl.col(k).median() for k in
                              ["presence", "units_med", "price_med", "min_med", "vol"] + [f"wd_{d}" for d in range(7)]])
         .sort("faction", "window", "itype", "quality"))
    c.write_parquet(S5 / "targets_class.parquet")
    with pl.Config(tbl_rows=30, tbl_cols=-1, tbl_width_chars=200, float_precision=3):
        print(t.group_by("faction", "window").agg(pl.len().alias("items"), pl.col("site_days").first(),
                                                  pl.col("presence").median(), pl.col("min_med").median(),
                                                  pl.col("vol").median()).sort("faction", "window"))


if __name__ == "__main__":
    main()
