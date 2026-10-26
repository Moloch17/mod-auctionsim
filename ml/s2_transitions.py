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

Empty seller names (about 7% of auctions) stay in: they are real supply and
real sale candidates, just with a weaker key. Auctioneer often has a seller's
name in one scan and not the next (or the reverse), so one auction would read
as gone under one key and new under the other: at 6-hourly scans that was
18-44% of everything that went, by price. Within each pair, an empty seller
can take a name from the other scan. Per listing (item, suffix, enchant,
count, minbid, buyout), only the empty ones in excess of the other scan's
empty ones are named, each after a named seller who has more of that listing
in the other scan than in this one, up to how many that seller is short.

  out/transitions_thin/<faction>/<t0>.parquet
    The same, between snapshots picked about a day apart (THIN_MIN_H) from a
    run of short gaps: the 6-hourly scans seen at the old daily cadence. Only
    the calibration of daily-era rates reads these (stages 3 and 4).

A KEY_VERSION marker in out/transitions guards the skip-if-done rerun: pairs
written under another key version are refused rather than mixed in.

Only snapshots marked ok and not duplicate in snapshots.parquet are used.
Pairs already written are skipped on a rerun.
"""

import polars as pl

from common import FACTION_NAMES, OUT, pool

KEY = ["seller", "item", "suffix", "enchant", "count", "minbid", "buyout"]
LISTING = KEY[1:]
KEY_VERSION = "4"  # 4: surplus empty sellers named from the other scan of the pair
SHORT_H = 7.0      # a gap this short or shorter is part of a run of short gaps
THIN_MIN_H, THIN_MAX_H = 23.0, 28.0


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


def name_empty(a, other):
    """a with empty sellers named from `other`, the other scan of the pair.
    Per listing, only the empty ones in excess of `other`'s empty ones are
    named (an empty one on both sides already matches), each after a named
    seller with more of that listing in `other` than in `a`, as many as that
    seller is short; several such sellers are filled in name order."""
    blank = pl.col("seller") == ""
    surplus = (a.filter(blank).group_by(LISTING).agg(here=pl.len())
               .join(other.filter(blank).group_by(LISTING).agg(there=pl.len()), on=LISTING, how="left")
               .with_columns(surplus=pl.col("here") - pl.col("there").fill_null(0))
               .filter(pl.col("surplus") > 0).select(*LISTING, "surplus"))
    short = (other.filter(~blank).group_by(["seller"] + LISTING).agg(there=pl.len())
             .join(a.filter(~blank).group_by(["seller"] + LISTING).agg(here=pl.len()),
                   on=["seller"] + LISTING, how="left")
             .with_columns(d=pl.col("there") - pl.col("here").fill_null(0)).filter(pl.col("d") > 0)
             .sort("seller")
             .with_columns(hi=pl.col("d").cum_sum().over(LISTING))
             .with_columns(lo=pl.col("hi") - pl.col("d"))
             .select(*LISTING, fill="seller", lo="lo", hi="hi"))
    a = a.with_row_index("_row")
    named = (a.filter(blank).join(surplus, on=LISTING)
             .with_columns(k=pl.int_range(1, pl.len() + 1).over(LISTING))
             .filter(pl.col("k") <= pl.col("surplus"))
             .join(short, on=LISTING).filter(pl.col("k") > pl.col("lo"), pl.col("k") <= pl.col("hi"))
             .select("_row", "fill"))
    return (a.join(named, on="_row", how="left")
            .with_columns(seller=pl.coalesce("fill", "seller")).drop("fill", "_row"))


def transition(args):
    faction, t0, t1, kind = args
    path = OUT / kind / FACTION_NAMES[faction] / f"{t0}.parquet"
    if path.exists():
        return
    a0, a1 = auctions(faction, t0), auctions(faction, t1)
    a0, a1 = name_empty(a0, a1), name_empty(a1, a0)
    c0, c1 = counts(a0, 0), counts(a1, 1)
    j = c0.join(c1, on=KEY, how="full", coalesce=True).fill_null(0)
    j = j.with_columns(
        t0=pl.lit(t0, pl.Int64), t1=pl.lit(t1, pl.Int64), gap_h=pl.lit((t1 - t0) / 3600),
        persisted=pl.min_horizontal("n0", "n1"),
    ).with_columns(gone=pl.col("n0") - pl.col("persisted"), new=pl.col("n1") - pl.col("persisted"))
    path.parent.mkdir(parents=True, exist_ok=True)
    j.write_parquet(path)


def check_version(kind):
    d = OUT / kind
    marker = d / "KEY_VERSION"
    if marker.exists():
        if marker.read_text().strip() != KEY_VERSION:
            raise SystemExit(f"{d} was written under key version {marker.read_text().strip()}, this is "
                             f"{KEY_VERSION}: rm -rf {d} and rerun stage 2 (and the stages after it)")
    elif d.exists() and any(d.glob("*/*.parquet")):
        raise SystemExit(f"{d} predates key versions: rm -rf {d} and rerun stage 2 (and the stages after it)")
    d.mkdir(parents=True, exist_ok=True)
    marker.write_text(KEY_VERSION + "\n")


def thinned(times):
    """Pairs about a day apart inside each run of short gaps."""
    pairs, run = [], [times[0]]
    for a, b in zip(times, times[1:]):
        if (b - a) / 3600 <= SHORT_H:
            run.append(b)
            continue
        pairs += thin_run(run)
        run = [b]
    return pairs + thin_run(run)


def thin_run(run):
    pairs, cur = [], run[0]
    for t in run[1:]:
        if (t - cur) / 3600 >= THIN_MIN_H:
            if (t - cur) / 3600 <= THIN_MAX_H:
                pairs.append((cur, t))
            cur = t
    return pairs


def main():
    for kind in ("transitions", "transitions_thin"):
        check_version(kind)
    snaps = (pl.read_parquet(OUT / "snapshots.parquet")
             .filter(pl.col("ok") & ~pl.col("duplicate")).sort("faction", "scan_time"))
    jobs_m, jobs_t = [], []
    for (faction,), s in snaps.group_by("faction"):
        times = s["scan_time"].to_list()
        jobs_m += [(faction, t) for t in times]
        jobs_t += [(faction, a, b, "transitions") for a, b in zip(times, times[1:])]
        jobs_t += [(faction, a, b, "transitions_thin") for a, b in thinned(times)]
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
