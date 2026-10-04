#!/usr/bin/env python3
"""Stage 2: snapshot-to-snapshot transitions and per-item market context.

There are no auction ids in the scans, so auctions are not tracked one by one.
A listing key is (seller, item, suffix, enchant, count, minbid, buyout); a bid
changes none of these. For each pair of consecutive snapshots of a faction the
keys present on either side are counted:

  out/transitions/<faction>/<t0>.parquet
    t0, t1, gap_h, key columns, n0, n1 (auctions with that key), n0_tl1..4 /
    n1_tl1..4 (by TLEFT bucket), bid0 (of n0, how many had a bid),
    persisted = min(n0, n1), gone = n0 - persisted, new = n1 - persisted

  out/market/<faction>/<t>.parquet  (one row per item per snapshot)
    t, item, auctions, units, sellers, min_unit (lowest per-unit buyout),
    med_unit (median per-unit buyout over auctions), min_unit_bid

Empty seller names (about 9% of auctions) stay in: they are real supply and
real sale candidates, just with a weaker key.

Only snapshots marked ok and not duplicate in snapshots.parquet are used.
Pairs already written are skipped on a rerun.
"""

import polars as pl

from common import FACTION_NAMES, OUT, pool

KEY = ["seller", "item", "suffix", "enchant", "count", "minbid", "buyout"]


def auctions(faction, t):
    return pl.read_parquet(OUT / "auctions" / FACTION_NAMES[faction] / f"{t}.parquet").with_columns(
        unit=pl.when(pl.col("buyout") > 0).then(pl.col("buyout") / pl.col("count")))


def market(args):
    faction, t = args
    path = OUT / "market" / FACTION_NAMES[faction] / f"{t}.parquet"
    if path.exists():
        return
    a = auctions(faction, t)
    m = a.group_by("item").agg(
        auctions=pl.len(), units=pl.col("count").sum(),
        sellers=pl.col("seller").filter(pl.col("seller") != "").n_unique(),
        min_unit=pl.col("unit").min(), med_unit=pl.col("unit").median(),
        min_unit_bid=(pl.col("minbid") / pl.col("count")).min(),
    ).with_columns(t=pl.lit(t, pl.Int64))
    path.parent.mkdir(parents=True, exist_ok=True)
    m.write_parquet(path)


def counts(a, side):
    return a.group_by(KEY).agg(
        pl.len().alias(f"n{side}"),
        *[(pl.col("tleft") == k).sum().alias(f"n{side}_tl{k}") for k in (1, 2, 3, 4)],
        *([(pl.col("curbid") > 0).sum().alias("bid0")] if side == 0 else []),
    )


def transition(args):
    faction, t0, t1 = args
    path = OUT / "transitions" / FACTION_NAMES[faction] / f"{t0}.parquet"
    if path.exists():
        return
    c0, c1 = counts(auctions(faction, t0), 0), counts(auctions(faction, t1), 1)
    j = c0.join(c1, on=KEY, how="full", coalesce=True).fill_null(0)
    j = j.with_columns(
        t0=pl.lit(t0, pl.Int64), t1=pl.lit(t1, pl.Int64), gap_h=pl.lit((t1 - t0) / 3600),
        persisted=pl.min_horizontal("n0", "n1"),
    ).with_columns(gone=pl.col("n0") - pl.col("persisted"), new=pl.col("n1") - pl.col("persisted"))
    path.parent.mkdir(parents=True, exist_ok=True)
    j.write_parquet(path)


def main():
    snaps = (pl.read_parquet(OUT / "snapshots.parquet")
             .filter(pl.col("ok") & ~pl.col("duplicate")).sort("faction", "scan_time"))
    jobs_m, jobs_t = [], []
    for (faction,), s in snaps.group_by("faction"):
        times = s["scan_time"].to_list()
        jobs_m += [(faction, t) for t in times]
        jobs_t += [(faction, a, b) for a, b in zip(times, times[1:])]
    with pool() as workers:
        for i, _ in enumerate(workers.imap_unordered(market, jobs_m), 1):
            print(f"market {i}/{len(jobs_m)}", end="\r", flush=True)
        print()
        for i, _ in enumerate(workers.imap_unordered(transition, jobs_t), 1):
            print(f"transitions {i}/{len(jobs_t)}", end="\r", flush=True)
    print()
    t = pl.scan_parquet(OUT / "transitions" / "*" / "*.parquet")
    print(t.group_by(pl.col("gap_h").round(0)).agg(pl.len().alias("keys"), pl.col("n0").sum(),
          pl.col("persisted").sum(), pl.col("gone").sum(), pl.col("new").sum())
          .sort("gap_h").collect())


if __name__ == "__main__":
    main()
