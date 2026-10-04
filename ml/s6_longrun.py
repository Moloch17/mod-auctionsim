#!/usr/bin/env python3
"""Stage 6f: does the market hold up over a long run, or degrade?

Reads the daily snapshots of long s6_market_sim runs (run with --days 365 and the
shipped market file) and the market file's reference and vendor prices, and
reports per week, per faction:

  listings     auctions up (the whole house)
  items        items with something up
  price        median over items of ln(median price / reference): 0 = at reference
  p99          99th percentile over items of ln(median price / reference): a runaway minority shows here
               long before it moves a median
  cheapest     median over items of ln(cheapest price / reference)
  spread       median over items of cheapest / median
  at_floor     share of items whose cheapest listing sits at the vendor-price floor
  vol          median over items of the std of day-to-day ln changes of the median price
  posted, sold, buyers, spent_g   per day; sold_per_buyer = sold / buyers

Then compares the first quarter after the house has filled (weeks 2-13) with the
last quarter (the last 13 weeks) and flags any metric that moved more than its
tolerance -- the signature of an undercutting spiral, prices creeping up, a
thinning or bloating house, or demand drying up.

  ./s6_longrun.py out/s6/ship.dat sim_year_horde sim_year_alliance

Writes out/s6/longrun_weekly.parquet and out/s6/longrun_report.txt.
"""

import sys

import numpy as np
import polars as pl

from common import OUT

S6 = OUT / "s6"
# Metric -> (kind, tolerance): "log" metrics compare by difference of logs/levels, "rel" by ratio.
TOLERANCE = {"listings": ("rel", 0.10), "items": ("rel", 0.10), "price": ("diff", 0.10), "p99": ("diff", 0.25),
             "cheapest": ("diff", 0.10),
             "spread": ("diff", 0.05), "at_floor": ("diff", 0.03), "vol": ("rel", 0.20), "sold": ("rel", 0.10),
             "sold_per_buyer": ("diff", 0.03), "spent_g": ("rel", 0.10)}


def item_table(market):
    rows = {2: [], 6: []}
    with open(market) as f:
        f.readline()
        while header := f.readline().split():
            for _ in range(int(header[1])):
                r = f.readline().rstrip("\n").split(":")
                if header[0] == "ITEM":
                    rows[int(r[0])].append((int(r[1]), float(r[3]), float(r[6])))
    return {k: pl.DataFrame(v, schema={"item": pl.Int64, "ref": pl.Float64, "vendor": pl.Float64}, orient="row")
            for k, v in rows.items()}


def weekly(daily, items):
    d = (daily.with_columns(pl.col("item").cast(pl.Int64), pl.col("day").cast(pl.Int64))
         .join(items, on="item", how="left").filter(pl.col("ref") > 0)
         .sort("item", "day")
         .with_columns(week=pl.col("day") // 7,
                       price=(pl.col("med_unit") / pl.col("ref")).log(),
                       cheapest=(pl.col("min_unit") / pl.col("ref")).log(),
                       spread=pl.col("min_unit") / pl.col("med_unit"),
                       at_floor=(pl.col("vendor") > 0) & (pl.col("min_unit") <= pl.col("vendor") * 1.01),
                       ret=(pl.col("med_unit").log() - pl.col("med_unit").log().shift(1)).over("item")))
    per_item_week = d.group_by("week", "item").agg(vol=pl.col("ret").std())
    days = (d.group_by("day").agg(pl.col("week").first(), listings=pl.col("listings").first(), items=pl.len(),
                                  *[pl.col(c).first() for c in d.columns if c.endswith("_cum")])
            .sort("day"))
    cum = [c for c in days.columns if c.endswith("_cum")]
    days = days.with_columns([pl.col(c).diff().alias(c[:-4]) for c in cum])
    w = (d.group_by("week").agg(pl.col("price").median(), pl.col("cheapest").median(), pl.col("spread").median(),
                                p99=pl.col("price").quantile(0.99),
                                at_floor=pl.col("at_floor").mean())
         .join(per_item_week.group_by("week").agg(pl.col("vol").median()), on="week")
         .join(days.group_by("week").agg(pl.col("listings").median(), pl.col("items").median(),
                                          pl.col("posted").mean(), pl.col("sold").mean(), pl.col("buyers").mean(),
                                          spent_g=pl.col("spent").mean() / 10000), on="week")
         .with_columns(sold_per_buyer=pl.col("sold") / pl.col("buyers"))
         .sort("week"))
    return w


def compare(w):
    first = w.filter(pl.col("week").is_between(2, 13))
    last = w.filter(pl.col("week") >= w["week"].max() - 12)
    out = []
    for m, (kind, tol) in TOLERANCE.items():
        a, b = float(first[m].mean()), float(last[m].mean())
        change = (b / a - 1) if kind == "rel" and a else (b - a)
        weeks = w.filter(pl.col("week") >= 2)
        slope = float(np.polyfit(weeks["week"].to_numpy(), weeks[m].to_numpy(), 1)[0]) * 52 if weeks.height > 2 else 0
        out.append({"metric": m, "first_q": a, "last_q": b, "change": change, "kind": kind, "tolerance": tol,
                    "per_year_trend": slope, "flag": abs(change) > tol})
    return pl.DataFrame(out)


def main():
    market, tags = sys.argv[1], sys.argv[2:]
    items = item_table(market)
    lines, all_w = [], []
    for tag in tags:
        daily = pl.read_parquet(S6 / f"{tag}_daily.parquet")
        faction = 6 if "horde" in tag else 2
        w = weekly(daily, items[faction]).with_columns(run=pl.lit(tag))
        all_w.append(w)
        c = compare(w)
        with pl.Config(tbl_rows=-1, tbl_cols=-1, tbl_width_chars=220, float_precision=3):
            lines += [f"== {tag}: {w.height} weeks", str(w.drop("run").gather_every(4)),
                      "first quarter (weeks 2-13) vs last quarter; change is relative for rel, absolute for diff:",
                      str(c), f"FLAGGED: {', '.join(c.filter(pl.col('flag'))['metric'].to_list()) or 'none'}", ""]
    pl.concat(all_w).write_parquet(S6 / "longrun_weekly.parquet")
    report = "\n".join(lines)
    (S6 / "longrun_report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
