#!/usr/bin/env python3
"""Stage 4: buyer side -- sell-through by price, from snapshot transitions.

Inputs: out/transitions, out/s3/items.parquet.

A listing that is gone at the next snapshot sold, expired or was cancelled.
  * Price is measured against common.add_reference (rolling median of the
    prices the item was posted at), not a median of listings: walls of
    identical listings that rarely sell set those, and listings sitting
    exactly at the median of snapshot medians showed a false dip (gone 44%
    vs 68% for the rest of their bin).
  * Cancels: a named seller's new listings of the same item priced below the
    dearest one of theirs that went are undercut reposts, so min(gone, those)
    of their gone listings count as cancels, not sales. Same-price relists
    are restocks or expiries and stay in.
  * Hidden restocks: a persisting listing's TLEFT can only fall, so more
    TLEFT-4 listings of a key at t1 than at t0 plus its new ones means that
    many identical listings were relisted after the old ones went:
    gone += max(0, n1_tl4 - n0_tl4 - new).
  * Expiry: listings at >= BASELINE_RATIO x reference almost never sell; their
    disappearance rate per item class and TLEFT bucket is the expiry-only
    baseline, and sale probability is the excess over it. TLEFT 1-2 listings
    (under 2 h left) always expire within a daily gap.
  * Rate: hazard h = -ln(1 - p) / gap_h, so different gaps compare.

Daily pairs (gap 20-28 h) are the fit; pairs no more than 7 h apart (the
Sept 2025 scans) are the check: a TLEFT-4 listing (12-48 h left) gone within
7 h cannot have expired.

Outputs (out/s4/):
  demand.parquet       itype x TLEFT x ratio bin: rates, baseline, hazard
  reservation.parquet  itype x ratio bin: share of buyers willing to pay at
                       least the bin's price (TLEFT-4 hazard over the
                       cheapest bin's, made non-increasing) -- the simulator's
                       buyer reservation prices
  item_sales.parquet   faction x item x t0 (daily pairs): estimated listings
                       and units sold over the gap (disappearances less
                       expected expiries) -- the simulator's buyer arrival rates
  check.parquet, report.txt
"""

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT, add_reference

S4 = OUT / "s4"
RATIO_BINS = [0.5, 0.7, 0.85, 1.0, 1.15, 1.4, 2.0, 3.0]
BIN_LABELS = [f"<{RATIO_BINS[0]}"] + [f"{a}-{b}" for a, b in zip(RATIO_BINS, RATIO_BINS[1:])] + \
    [f">={RATIO_BINS[-1]}"]
BASELINE_RATIO = 2.0
MIN_N = 200  # listings per cell before a rate is reported


def listings(faction):
    name = FACTION_NAMES[faction]
    tr = (pl.scan_parquet(OUT / "transitions" / name / "*.parquet")
          .with_columns(pl.col(c).cast(pl.Int64) for c in
                        ["n0", "n1", "persisted", "gone", "new", "n0_tl1", "n0_tl2", "n0_tl3", "n0_tl4",
                         "n1_tl4", "bid0"])
          .with_columns(unit=pl.when(pl.col("buyout") > 0).then(pl.col("buyout") / pl.col("count"))))
    sv = (tr.filter(pl.col("seller") != "")
          .group_by("t0", "seller", "item")
          .agg(g=pl.col("gone").sum(),
               cheaper=pl.col("new").filter(pl.col("unit") < pl.col("unit").filter(pl.col("gone") > 0).max()).sum())
          .with_columns(keep=pl.when(pl.col("g") > 0)
                        .then(1 - pl.min_horizontal("g", "cheaper") / pl.col("g")).otherwise(1.0)))
    return (tr.filter(pl.col("n0") > 0, pl.col("buyout") > 0)
            .join(sv.select("t0", "seller", "item", "keep"), on=["t0", "seller", "item"], how="left")
            .pipe(add_reference, faction, "t0")
            .with_columns(hidden=(pl.col("n1_tl4") - pl.col("n0_tl4") - pl.col("new")).clip(0, None))
            .with_columns(gone_true=pl.min_horizontal(pl.col("gone") + pl.col("hidden"), pl.col("n0")))
            .with_columns(
                faction=pl.lit(faction, pl.Int8),
                gone_adj=pl.col("gone_true") * pl.col("keep").fill_null(1.0),
                ratio_ref=pl.col("unit") / pl.col("ref"),
                tleft0=pl.when(pl.col("n0_tl4") == pl.col("n0")).then(4)
                       .when(pl.col("n0_tl3") == pl.col("n0")).then(3).otherwise(0),
            )
            .filter(pl.col("ratio_ref").is_finite())
            .select("faction", "t0", "gap_h", "item", "seller", "count", "n0", "n0_tl1", "n0_tl2", "n0_tl3",
                    "n0_tl4", "gone", "hidden", "gone_adj", "bid0", "ratio_ref", "tleft0")
            .collect())


def binned(df):
    return df.with_columns(ratio_bin=pl.col("ratio_ref").cut(RATIO_BINS, labels=BIN_LABELS, left_closed=True))


def hazard(p, gap):
    return -(1 - p).log() / gap


def main():
    S4.mkdir(parents=True, exist_ok=True)
    items = pl.read_parquet(OUT / "s3" / "items.parquet").select("item", "itype")
    df = binned(pl.concat([listings(f) for f in FACTION_NAMES]).join(items, on="item", how="left"))
    daily = df.filter(pl.col("gap_h").is_between(20, 28))
    short = df.filter(pl.col("gap_h") <= 7)

    one = daily.filter(pl.col("tleft0") > 0)
    cells = (one.group_by("itype", "tleft0", "ratio_bin")
             .agg(n=pl.col("n0").sum(), gone=pl.col("gone").sum(), hidden=pl.col("hidden").sum(),
                  gone_adj=pl.col("gone_adj").sum(), bids=pl.col("bid0").sum(), gap_h=pl.col("gap_h").mean())
             .with_columns(p_gone=pl.col("gone_adj") / pl.col("n"), bid_share=pl.col("bids") / pl.col("n")))
    base = (one.filter(pl.col("ratio_ref") >= BASELINE_RATIO).group_by("itype", "tleft0")
            .agg(base_n=pl.col("n0").sum(), baseline=pl.col("gone_adj").sum() / pl.col("n0").sum()))
    demand = (cells.join(base, on=["itype", "tleft0"], how="left")
              .with_columns(sale_p=((pl.col("p_gone") - pl.col("baseline")) / (1 - pl.col("baseline")))
                            .clip(0, 0.999))
              .with_columns(hazard_h=hazard(pl.col("sale_p"), pl.col("gap_h")),
                            reliable=(pl.col("n") >= MIN_N) & (pl.col("base_n") >= MIN_N))
              .sort("itype", "tleft0", "ratio_bin"))
    demand.write_parquet(S4 / "demand.parquet")

    # Reservation prices: TLEFT-4 hazard by bin over the cheapest bin's, per class (overall as fallback).
    def survival(d):
        h = d.sort("ratio_bin")["hazard_h"].fill_null(0).to_numpy()
        s = h / h[0] if h[0] > 0 else np.zeros_like(h)
        return np.minimum.accumulate(np.clip(s, 0, 1))
    t4 = demand.filter(pl.col("tleft0") == 4)
    overall = (t4.group_by("ratio_bin").agg(hazard_h=(pl.col("hazard_h").fill_null(0) * pl.col("n")).sum()
                                            / pl.col("n").sum()))
    rows = [{"itype": None, "ratio_bin": b, "willing": w} for b, w in zip(BIN_LABELS, survival(overall))]
    for (itype,), d in t4.filter(pl.col("reliable")).group_by("itype"):
        if d.height == len(BIN_LABELS):
            rows += [{"itype": itype, "ratio_bin": b, "willing": w} for b, w in zip(BIN_LABELS, survival(d))]
    reservation = pl.DataFrame(rows)
    reservation.write_parquet(S4 / "reservation.parquet")

    # Sales per item per daily pair: disappearances less the expiries expected at each listing's TLEFT.
    b = base.pivot(on="tleft0", index="itype", values="baseline").rename({"3": "b3", "4": "b4"})
    sales = (daily.join(b, on="itype", how="left")
             .with_columns(expire=pl.col("n0_tl1") + pl.col("n0_tl2") + pl.col("n0_tl3") * pl.col("b3").fill_null(1.0)
                           + pl.col("n0_tl4") * pl.col("b4").fill_null(0.4))
             .with_columns(gone_adj_frac=pl.col("gone_adj") / pl.col("n0"))
             .group_by("faction", "item", "t0")
             .agg(gap_h=pl.col("gap_h").first(), listings=pl.col("n0").sum(),
                  sold=(pl.col("gone_adj").sum() - pl.col("expire").sum()).clip(0, None),
                  sold_units=((pl.col("gone_adj") - pl.col("expire")) * pl.col("count")).sum().clip(0, None)))
    sales.write_parquet(S4 / "item_sales.parquet")

    def overall_hazard(d, corrected):
        g = d.filter(pl.col("tleft0") == 4).group_by("ratio_bin").agg(
            n=pl.col("n0").sum(), gone_adj=pl.col("gone_adj").sum(), gap_h=pl.col("gap_h").mean())
        g = g.with_columns(p=pl.col("gone_adj") / pl.col("n"))
        if corrected:
            e = d.filter(pl.col("tleft0") == 4, pl.col("ratio_ref") >= BASELINE_RATIO)
            e = e["gone_adj"].sum() / e["n0"].sum()
            g = g.with_columns(p=((pl.col("p") - e) / (1 - e)).clip(0, 0.999))
        return g.with_columns(hazard_h=hazard(pl.col("p"), pl.col("gap_h")))

    check = (overall_hazard(daily, True).select("ratio_bin", pl.col("n").alias("daily_n"),
                                                pl.col("hazard_h").alias("daily_hazard_h"))
             .join(overall_hazard(short, False).select("ratio_bin", pl.col("n").alias("short_n"),
                                                       pl.col("hazard_h").alias("short_hazard_h")),
                   on="ratio_bin", how="full", coalesce=True)
             .with_columns(ratio=pl.col("short_hazard_h") / pl.col("daily_hazard_h")).sort("ratio_bin"))
    check.write_parquet(S4 / "check.parquet")
    ok = check.drop_nulls()
    rho = float(np.corrcoef(ok["daily_hazard_h"].rank().to_numpy(), ok["short_hazard_h"].rank().to_numpy())[0, 1])

    top = (demand.filter(pl.col("reliable"), pl.col("tleft0") == 4)
           .group_by("itype").agg(pl.col("n").sum()).sort("n", descending=True).head(4)["itype"])
    with pl.Config(tbl_rows=-1, tbl_cols=-1, tbl_width_chars=200, float_precision=4):
        report = "\n".join([
            f"listing-pairs: daily {daily['n0'].sum()}, short-gap {short['n0'].sum()}",
            f"gone {df['gone'].sum()}, hidden restocks added {df['hidden'].sum()}, "
            f"after undercut reposts {df['gone_adj'].sum():.0f}",
            "", "check -- hourly sale hazard by price vs reference, TLEFT 4:", str(check),
            f"rank correlation daily vs short-gap: {rho:.2f}", "",
            "reservation (share of buyers who pay at least the bin):",
            str(reservation.pivot(on="ratio_bin", index="itype", values="willing")),
            "", "fit -- TLEFT 4, largest classes:",
            str(demand.filter(pl.col("tleft0") == 4, pl.col("itype").is_in(top.to_list()))
                .select("itype", "ratio_bin", "n", "p_gone", "baseline", "sale_p", "hazard_h", "reliable")),
            "", "estimated sales per day, per faction:",
            str(sales.group_by("faction").agg(pl.col("sold").sum() / pl.col("t0").n_unique(),
                                              pl.col("sold_units").sum() / pl.col("t0").n_unique())),
        ])
    (S4 / "report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
