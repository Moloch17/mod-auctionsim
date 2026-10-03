#!/usr/bin/env python3
"""Stage 4: buyer side -- sell-through by price, v1 from snapshot transitions.

Inputs: out/transitions, out/market, out/s3/items.parquet.

A listing that is gone at the next snapshot sold, expired or was cancelled.
  * Cancels: when the same named seller has new keys of the same item at t1,
    min(gone, new) of their gone listings count as reposts (an undercut or a
    re-list), not sales.
  * Expiry: does not depend on price. The disappearance rate of listings
    priced at >= BASELINE_RATIO x the item's reference price -- which almost
    never sell -- is taken as the expiry-only baseline for the same item
    class, TLEFT bucket and gap; sale probability is the excess over it.
  * Rate: sale probability over the gap is turned into an hourly hazard
    h = -ln(1 - p) / gap_h, so pairs with different gaps compare.

Daily pairs (gap 20-28 h) are the fit. Pairs no more than 7 h apart (the
Sept 2025 scans) are the check: a TLEFT 4 listing (12-48 h left) gone within
7 h cannot have expired, so there the repost-adjusted disappearance IS the
sale (or non-repost cancel) rate, with no baseline needed.

Outputs (out/s4/): demand.parquet (fit: itype x ratio bin x TLEFT), check.parquet
(daily vs short-gap hazard by ratio bin), report.txt.
"""

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT

S4 = OUT / "s4"
RATIO_BINS = [0.5, 0.7, 0.85, 1.0, 1.15, 1.4, 2.0, 3.0]
BASELINE_RATIO = 2.0
MIN_N = 200  # listings per cell before a rate is reported


def listings(faction):
    name = FACTION_NAMES[faction]
    tr = pl.scan_parquet(OUT / "transitions" / name / "*.parquet")
    mk = pl.scan_parquet(OUT / "market" / name / "*.parquet")
    ref = mk.group_by("item").agg(ref_unit=pl.col("med_unit").median())
    # Same-seller same-item new keys at t1: reposts.
    sv = (tr.filter(pl.col("seller") != "")
          .group_by("t0", "seller", "item").agg(g=pl.col("gone").sum(), nw=pl.col("new").sum())
          .with_columns(keep=pl.when(pl.col("g") > 0)
                        .then(1 - pl.min_horizontal("g", "nw") / pl.col("g")).otherwise(1.0)))
    return (tr.filter(pl.col("n0") > 0, pl.col("buyout") > 0)
            .join(sv.select("t0", "seller", "item", "keep"), on=["t0", "seller", "item"], how="left")
            .join(ref, on="item", how="left")
            .join(mk.rename({"t": "t0"}).select("t0", "item", "min_unit"), on=["t0", "item"], how="left")
            .with_columns(
                faction=pl.lit(faction, pl.Int8),
                gone_adj=pl.col("gone") * pl.col("keep").fill_null(1.0),
                ratio_ref=pl.col("buyout") / pl.col("count") / pl.col("ref_unit"),
                tleft0=pl.when(pl.col("n0_tl4") == pl.col("n0")).then(4)
                       .when(pl.col("n0_tl3") == pl.col("n0")).then(3).otherwise(0),
                weekday=pl.from_epoch("t0").dt.weekday(),
            )
            .filter(pl.col("tleft0") > 0, pl.col("ratio_ref").is_finite())
            .select("faction", "t0", "gap_h", "item", "seller", "n0", "gone", "gone_adj", "bid0", "ratio_ref",
                    "tleft0", "weekday")
            .collect())


def binned(df):
    labels = [f"<{RATIO_BINS[0]}"] + [f"{a}-{b}" for a, b in zip(RATIO_BINS, RATIO_BINS[1:])] + \
        [f">={RATIO_BINS[-1]}"]
    return df.with_columns(ratio_bin=pl.col("ratio_ref").cut(RATIO_BINS, labels=labels, left_closed=True))


def main():
    S4.mkdir(parents=True, exist_ok=True)
    items = pl.read_parquet(OUT / "s3" / "items.parquet").select("item", "itype")
    df = binned(pl.concat([listings(f) for f in FACTION_NAMES]).join(items, on="item", how="left"))

    daily = df.filter(pl.col("gap_h").is_between(20, 28))
    cells = (daily.group_by("itype", "tleft0", "ratio_bin")
             .agg(n=pl.col("n0").sum(), gone=pl.col("gone").sum(), gone_adj=pl.col("gone_adj").sum(),
                  bids=pl.col("bid0").sum(), gap_h=pl.col("gap_h").mean(),
                  expensive=(pl.col("ratio_ref") >= BASELINE_RATIO).first())
             .with_columns(p_gone=pl.col("gone_adj") / pl.col("n"), bid_share=pl.col("bids") / pl.col("n")))
    base = (daily.filter(pl.col("ratio_ref") >= BASELINE_RATIO).group_by("itype", "tleft0")
            .agg(base_n=pl.col("n0").sum(), baseline=pl.col("gone_adj").sum() / pl.col("n0").sum()))
    demand = (cells.join(base, on=["itype", "tleft0"], how="left")
              .with_columns(sale_p=((pl.col("p_gone") - pl.col("baseline")) / (1 - pl.col("baseline")))
                            .clip(0, 0.999))
              .with_columns(hazard_h=-(1 - pl.col("sale_p")).log() / pl.col("gap_h"))
              .with_columns(reliable=(pl.col("n") >= MIN_N) & (pl.col("base_n") >= MIN_N))
              .sort("itype", "tleft0", "ratio_bin"))
    demand.write_parquet(S4 / "demand.parquet")

    # Check: overall hazard by ratio bin, daily (baseline-corrected) vs short gaps (TLEFT 4: no expiry).
    def overall(d, corrected):
        g = d.filter(pl.col("tleft0") == 4).group_by("ratio_bin").agg(
            n=pl.col("n0").sum(), gone_adj=pl.col("gone_adj").sum(), gap_h=pl.col("gap_h").mean())
        g = g.with_columns(p=pl.col("gone_adj") / pl.col("n"))
        if corrected:
            b = d.filter(pl.col("tleft0") == 4, pl.col("ratio_ref") >= BASELINE_RATIO)
            b = b["gone_adj"].sum() / b["n0"].sum()
            g = g.with_columns(p=((pl.col("p") - b) / (1 - b)).clip(0, 0.999))
        return g.with_columns(hazard_h=-(1 - pl.col("p")).log() / pl.col("gap_h"))

    short = df.filter(pl.col("gap_h") <= 7)
    check = (overall(daily, True).select("ratio_bin", pl.col("n").alias("daily_n"),
                                         pl.col("hazard_h").alias("daily_hazard_h"))
             .join(overall(short, False).select("ratio_bin", pl.col("n").alias("short_n"),
                                                pl.col("hazard_h").alias("short_hazard_h")),
                   on="ratio_bin", how="full", coalesce=True)
             .sort("ratio_bin"))
    check.write_parquet(S4 / "check.parquet")
    ok = check.drop_nulls()
    rho = float(np.corrcoef(ok["daily_hazard_h"].rank().to_numpy(), ok["short_hazard_h"].rank().to_numpy())[0, 1]) \
        if ok.height > 2 else float("nan")

    top = (demand.filter(pl.col("reliable"), pl.col("tleft0") == 4)
           .group_by("itype").agg(pl.col("n").sum()).sort("n", descending=True).head(6)["itype"])
    with pl.Config(tbl_rows=-1, tbl_cols=-1, tbl_width_chars=200, float_precision=4):
        report = "\n".join([
            f"listing-pairs: daily {daily['n0'].sum()}, short-gap {short['n0'].sum()}",
            f"reposts removed: {(df['gone'].sum() - df['gone_adj'].sum()):.0f} of {df['gone'].sum()} gone",
            "", "check -- hourly sale hazard by price vs reference, TLEFT 4:", str(check),
            f"rank correlation daily vs short-gap: {rho:.2f}", "",
            "fit -- hourly sale hazard, TLEFT 4, largest classes:",
            str(demand.filter(pl.col("tleft0") == 4, pl.col("itype").is_in(top.to_list()))
                .select("itype", "ratio_bin", "n", "p_gone", "baseline", "sale_p", "hazard_h", "bid_share",
                        "reliable")),
        ])
    (S4 / "report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
