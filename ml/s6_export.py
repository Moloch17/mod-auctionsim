#!/usr/bin/env python3
"""Stage 6a: export auctionsim_market.dat (see MARKET_FORMAT.md) as of a date.

  ./s6_export.py [--asof latest|tune|holdout|UNIX] [--out PATH] [--demand-scale K] [--supply-scale S] [--tilt G]
                 [--offsets PATH]

Everything is learned from data before --asof, so an export as of a window's
start can be scored on that window without seeing it. `latest` (the default)
is the shipping export: everything up to the newest scan. `tune`/`holdout`
take that window's start from out/s5/targets.parquet.

* POLICY: empirical quantiles of log(price / cheapest up) -- or of log(price /
  reference) when nothing was up -- over the POLICY_DAYS of posts, per seller
  type x item class x units-up bin x sellers bin x cheapest-vs-reference bin,
  with the format's fallback levels. Cells need MIN_CELL posts.
* STACK: quantiles of log(count / the item's stack convention) per type x class.
* BOT: BOTS + SPARES names per faction, split across seller types by their
  posting volume, each the busiest valid names of that type in the window
  (letters only, 2-12 chars, no letter three times running), unique across
  both factions.
* BASKET: each item's posting events per hour over RATE_DAYS, per seller type
  (an event is one seller posting one listing key, batch = listings in it),
  split evenly over up to BOTS_PER_ITEM bots of that type (fewer if the item
  had fewer sellers of it), picked by item so baskets differ. More would only
  grow the file: at a realm's scale an item rarely has more owners up at once.
* ITEM / CURVE / WEEKDAY: reference at --asof, stack convention, vendor price,
  stage-4 buyers per hour over RATE_DAYS; stage-4 reservation curves raised to
  --tilt (> 1: buyers pickier at higher prices); buyer weekday pattern.
* CRAFT: reagents from the nerfed pages; margin = the 10th percentile of the
  item's cheapest buyout over its cost price, clipped to [0.5, 1].
* --offsets: a parquet of (faction, type, class, mb, offset) learned in stage 7.
"""

import argparse
import math
import re
from pathlib import Path

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT, add_reference

SCHEMA = 1
RATE_DAYS = 60
POLICY_DAYS = 180
WEEKDAY_DAYS = 365
MIN_CELL = 50
BOTS, SPARES = 100, 20
BOTS_PER_ITEM = 2
QS = [0.02, 0.10, 0.25, 0.50, 0.75, 0.90, 0.98]
MB_EDGES = [-1.0, -0.5, -0.25, -0.1, 0.0, 0.1, 0.25, 0.5, 1.0]
SB_EDGES = [1, 2, 3, 5, 9]  # 0 -> 0, 1 -> 1, 2 -> 2, 3-4 -> 3, 5-8 -> 4, 9+ -> 5
NAME = re.compile(r"^[A-Za-z]{2,12}$")
TRIPLE = re.compile(r"(.)\1\1", re.I)


def ub_expr(units):
    return pl.min_horizontal(units.fill_null(0).log1p().floor(), 7).cast(pl.Int8)


def sb_expr(sellers):
    s = sellers.fill_null(0)
    return (pl.when(s >= 9).then(5).when(s >= 5).then(4).when(s >= 3).then(3).otherwise(s)).cast(pl.Int8)


def mb_expr(cheapest, ref):
    x = (cheapest / ref).log()
    b = pl.lit(0)
    for k, e in enumerate(MB_EDGES):
        b = pl.when(x >= e).then(k + 1).otherwise(b) if k else pl.when(x >= e).then(1).otherwise(0)
    # edges -1..1 -> bins 0..9 (>= 1.0 -> 9)
    return pl.when(cheapest.is_null()).then(-1).otherwise(pl.min_horizontal(b, 9)).cast(pl.Int8)


def quantile_cols(y):
    return [y.quantile(q, "linear").alias(f"q{int(q * 100):02d}") for q in QS]


def valid_name(n):
    return bool(NAME.match(n)) and not TRIPLE.search(n)


def export(asof, out, demand_scale, supply_scale, tilt, offsets_path):
    posts_all = pl.scan_parquet(OUT / "s3" / "posts.parquet")
    if asof == "latest":
        asof = int(posts_all.select(pl.col("t1").max()).collect().item()) + 1
    elif asof in ("tune", "holdout"):
        asof = int(pl.read_parquet(OUT / "s5" / "targets.parquet").filter(pl.col("window") == asof)
                   ["window_start"].min())
    asof = int(asof)
    items_dim = pl.read_parquet(OUT / "s3" / "items.parquet").select("item", "itype")
    itypes = sorted(items_dim["itype"].drop_nulls().unique().to_list())
    class_code = {t: k for k, t in enumerate(itypes)}
    template = pl.read_parquet(OUT / "item_template.parquet").select("item", "sell_price", "stackable")
    sellers = pl.read_parquet(OUT / "s3" / "sellers.parquet").select("faction", "seller", "type")
    reservation = pl.read_parquet(OUT / "s4" / "reservation.parquet").with_columns(pl.col("ratio_bin").cast(pl.String))
    sales = pl.scan_parquet(OUT / "s4" / "item_sales.parquet")
    recipes = pl.read_parquet(OUT / "nerfed_recipes.parquet")
    history = pl.scan_parquet(OUT / "nerfed_history.parquet")
    offsets = pl.read_parquet(offsets_path) if offsets_path else None

    lines = {k: [] for k in ["META", "ITEM", "CLASS", "CURVE", "WEEKDAY", "POLICY", "STACK", "CRAFT", "BOT", "BASKET"]}
    lines["CLASS"] = [f"{k}:{t}" for t, k in class_code.items()]
    used_names = set()
    summary = {}

    for faction in FACTION_NAMES:
        base = (posts_all.filter(pl.col("faction") == faction, pl.col("t1") < asof)
                .join(sellers.lazy().filter(pl.col("faction") == faction).drop("faction"), on="seller", how="left")
                .join(items_dim.lazy(), on="item", how="left")
                .with_columns(type=pl.col("type").fill_null(-1).cast(pl.Int8),
                              cls=pl.col("itype").replace_strict(class_code, default=-1, return_dtype=pl.Int16)))
        window = base.filter(pl.col("t1") >= asof - RATE_DAYS * 86400).collect()
        pol = (base.filter(pl.col("t1") >= asof - POLICY_DAYS * 86400, pl.col("buyout") > 0,
                           pl.col("ref_unit") > 0)
               .with_columns(ub=ub_expr(pl.col("units")), sb=sb_expr(pl.col("sellers")),
                             mb=mb_expr(pl.col("min_unit"), pl.col("ref_unit")))
               .with_columns(y=pl.when(pl.col("mb") >= 0).then((pl.col("unit") / pl.col("min_unit")).log())
                             .otherwise((pl.col("unit") / pl.col("ref_unit")).log()))
               .filter(pl.col("y").is_finite())
               .select("type", "cls", "ub", "sb", "mb", "y", "item", "count").collect())

        # ITEM: what the window's bots trade.
        ref = add_reference(pl.LazyFrame({"item": window["item"].unique(), "t": asof}), faction, "t").collect()
        sold = (sales.filter(pl.col("faction") == faction, pl.col("t0").is_between(asof - RATE_DAYS * 86400, asof,
                                                                                   closed="left"))
                .group_by("item").agg(buyers_h=pl.col("sold").sum() / pl.col("gap_h").sum()).collect())
        item = (window.group_by("item").agg(conv=pl.col("count").mode().first(), maxc=pl.col("count").max(),
                                             cls=pl.col("cls").first())
                .join(ref.select("item", "ref"), on="item", how="left")
                .join(template, on="item", how="left").join(sold, on="item", how="left")
                .filter(pl.col("ref") > 0)
                .with_columns(maxc=pl.min_horizontal("maxc", pl.col("stackable").fill_null(pl.col("maxc"))),
                              vendor=pl.col("sell_price").fill_null(0), buyers_h=pl.col("buyers_h").fill_null(0.0))
                .with_columns(maxc=pl.max_horizontal("maxc", "conv"))
                .sort("item"))
        traded = set(item["item"].to_list())
        conv = dict(zip(item["item"].to_list(), item["conv"].to_list()))
        lines["ITEM"] += [f"{faction}:{r['item']}:{r['cls']}:{r['ref']:.0f}:{r['conv']}:{r['maxc']}:{r['vendor']}:"
                          f"{r['buyers_h']:.6g}" for r in item.iter_rows(named=True)]

        # CURVE: per class, from stage-4 reservation (itype null = fallback), tilted.
        for (itype,), d in reservation.group_by("itype"):
            w = {r["ratio_bin"]: max(r["willing"], 0.0) for r in d.to_dicts()}
            from s4_demand import BIN_LABELS
            vals = np.minimum.accumulate(np.array([1.0] + [w.get(b, 0.0) for b in BIN_LABELS[1:]]) ** tilt)
            code = -1 if itype is None else class_code.get(itype)
            if code is not None:
                lines["CURVE"].append(f"{faction}:{code}:" + ":".join(f"{v:.5f}" for v in vals))

        # WEEKDAY: buyers by weekday of the day they bought, per class (and overall).
        wd = (sales.filter(pl.col("faction") == faction,
                           pl.col("t0").is_between(asof - WEEKDAY_DAYS * 86400, asof, closed="left"))
              .join(items_dim.lazy(), on="item", how="left")
              .with_columns(cls=pl.col("itype").replace_strict(class_code, default=-1, return_dtype=pl.Int16),
                            wd=pl.from_epoch("t0").dt.weekday() - 1, day=pl.col("t0") // 86400)
              .group_by("cls", "wd", "day").agg(sold=pl.col("sold").sum()).collect())
        for cls, d in [(-1, wd)] + [(c, wd.filter(pl.col("cls") == c)) for c in sorted(wd["cls"].unique()) if c >= 0]:
            per = d.group_by("wd", "day").agg(pl.col("sold").sum()).group_by("wd").agg(pl.col("sold").mean())
            m = dict(zip(per["wd"].to_list(), per["sold"].to_list()))
            if len(m) == 7 and sum(m.values()) > 0:
                mean = sum(m.values()) / 7
                lines["WEEKDAY"].append(f"{faction}:{cls}:" + ":".join(f"{m[k] / mean:.4f}" for k in range(7)))

        # POLICY with fallback levels; offsets from stage 7 joined on (type, class, mb).
        off = {}
        if offsets is not None:
            for r in offsets.filter(pl.col("faction") == faction).iter_rows(named=True):
                off[(r["type"], r["class"], r["mb"])] = r["offset"]
        levels = [["type", "cls", "ub", "sb", "mb"], ["type", "cls", "mb"], ["type", "mb"], ["mb"]]
        for keys in levels:
            cells = (pol.group_by(keys).agg(n=pl.len(), *quantile_cols(pl.col("y")))
                     .filter(pl.col("n") >= (MIN_CELL if keys != ["mb"] else 1)))
            for r in cells.iter_rows(named=True):
                t, c = r.get("type", -1), r.get("cls", -1)
                u, s = r.get("ub", -1), r.get("sb", -1)
                q = ":".join(f"{r[f'q{int(x * 100):02d}']:.4f}" for x in QS)
                o = off.get((t, c, r["mb"]), off.get((t, -1, r["mb"]), 0.0))
                lines["POLICY"].append(f"{faction}:{t}:{c}:{u}:{s}:{r['mb']}:{q}:{o:.4f}")

        # STACK: log(count / convention) quantiles per (type, class) with fallbacks.
        st = pol.with_columns(conv=pl.col("item").replace_strict(conv, default=None)).filter(
            pl.col("conv").is_not_null()).with_columns(z=(pl.col("count") / pl.col("conv")).log())
        for keys in [["type", "cls"], ["type"], []]:
            cells = (st.group_by(keys).agg(n=pl.len(), *quantile_cols(pl.col("z"))) if keys else
                     st.select(n=pl.len(), *quantile_cols(pl.col("z"))))
            for r in cells.filter(pl.col("n") >= (MIN_CELL if keys else 1)).iter_rows(named=True):
                q = ":".join(f"{r[f'q{int(x * 100):02d}']:.4f}" for x in QS)
                lines["STACK"].append(f"{faction}:{r.get('type', -1)}:{r.get('cls', -1)}:{q}")

        # CRAFT: reagents and a floor margin from the nerfed history.
        margin = (history.filter(pl.col("faction") == faction, pl.col("time") < asof,
                                 pl.col("time") >= asof - POLICY_DAYS * 86400, pl.col("cost_price") > 0,
                                 pl.col("buyout_min") > 0)
                  .group_by("item").agg(m=(pl.col("buyout_min") / pl.col("cost_price")).quantile(0.10)).collect())
        margin = dict(zip(margin["item"].to_list(), margin["m"].to_list()))
        for r in recipes.filter(pl.col("faction") == faction).iter_rows(named=True):
            if r["item"] in traded and r["item"] in margin:
                m = min(max(margin[r["item"]], 0.5), 1.0)
                lines["CRAFT"].append(f"{faction}:{r['item']}:{r['reagent']}:{r['qty']}:{m:.3f}")

        # BOT: names per type, by posting volume.
        vol = window.group_by("type").agg(v=pl.col("new").sum()).sort("v", descending=True)
        share = dict(zip(vol["type"].to_list(), (vol["v"] / vol["v"].sum()).to_list()))
        n_type = {t: max(1, math.floor(s * BOTS)) for t, s in share.items()}
        while sum(n_type.values()) > BOTS:
            n_type[max(n_type, key=n_type.get)] -= 1
        rest = sorted(share, key=lambda t: share[t] * BOTS - n_type[t], reverse=True)
        for t in rest[:BOTS - sum(n_type.values())]:
            n_type[t] += 1
        ranked = (window.filter(pl.col("seller") != "").group_by("seller", "type").agg(v=pl.col("new").sum())
                  .sort("v", descending=True))
        bots, spare_pool = [], []
        for t, k in n_type.items():
            names = [n for n in ranked.filter(pl.col("type") == t)["seller"].to_list()
                     if valid_name(n) and n.lower() not in used_names]
            if t == -1 and len(names) < k + SPARES:
                names += [n for n in ranked["seller"].to_list() if valid_name(n) and n.lower() not in used_names
                          and n not in names]
            for n in names[:k]:
                bots.append((n, t))
                used_names.add(n.lower())
            spare_pool += [(n, t) for n in names[k:k + SPARES]]
        spare_pool.sort(key=lambda x: -share.get(x[1], 0))
        for n, t in spare_pool:
            if len(bots) >= BOTS + SPARES:
                break
            if n.lower() not in used_names:
                bots.append((n, t))
                used_names.add(n.lower())
        lines["BOT"] += [f"{faction}:{b}:{n}:{t}" for b, (n, t) in enumerate(bots)]
        bots_of = {}
        for b, (_, t) in enumerate(bots[:BOTS]):
            bots_of.setdefault(t, []).append(b)

        # BASKET: per (item, type) events/hour, batch and duration mix, spread over that type's bots.
        ev = (window.filter(pl.col("item").is_in(list(traded)))
              .group_by("item", "type")
              .agg(events=pl.len(), listings=pl.col("new").sum(), tl4=pl.col("n1_tl4").sum(),
                   sellers=pl.col("seller").n_unique()))
        rows = 0
        for r in ev.iter_rows(named=True):
            group = bots_of.get(r["type"]) or bots_of[max(bots_of, key=lambda t: len(bots_of[t]))]
            m = max(1, min(len(group), r["sellers"], BOTS_PER_ITEM))
            rate = r["events"] / (RATE_DAYS * 24) / m
            batch = r["listings"] / r["events"]
            tl4 = min(1.0, r["tl4"] / max(r["listings"], 1))
            start = (r["item"] * 2654435761) % len(group)
            for j in range(m):
                b = group[(start + j) % len(group)]
                lines["BASKET"].append(f"{faction}:{b}:{r['item']}:{r['type']}:{rate:.6g}:{tl4:.3f}:{batch:.3f}")
                rows += 1

        ref_listings = int(pl.read_parquet(OUT / "snapshots.parquet")
                           .filter(pl.col("faction") == faction, pl.col("ok"), pl.col("scan_time") < asof)
                           .sort("scan_time").tail(30)["records"].median())
        lines["META"].append(f"{faction}:{demand_scale}:{supply_scale}:{ref_listings}:"
                             f"{window.filter(pl.col('seller') != '')['seller'].n_unique()}")
        summary[FACTION_NAMES[faction]] = {"items": item.height, "basket_rows": rows, "bots": len(bots),
                                           "types": n_type}

    out = Path(out)
    out.parent.mkdir(parents=True, exist_ok=True)
    with open(out, "w") as f:
        f.write(f"AUCTIONSIM_MARKET {SCHEMA}\n")
        for section, rows in lines.items():
            f.write(f"{section} {len(rows)}\n")
            f.writelines(r + "\n" for r in rows)
    return asof, summary, {k: len(v) for k, v in lines.items()}, out.stat().st_size


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--asof", default="latest")
    ap.add_argument("--out", default=str(OUT / "s6" / "auctionsim_market.dat"))
    ap.add_argument("--demand-scale", type=float, default=2.0)
    ap.add_argument("--supply-scale", type=float, default=1.0)
    ap.add_argument("--tilt", type=float, default=1.0)
    ap.add_argument("--offsets", default=None)
    args = ap.parse_args()
    asof, summary, counts, size = export(args.asof, args.out, args.demand_scale, args.supply_scale, args.tilt,
                                         args.offsets)
    print(f"asof {asof} -> {args.out} ({size / 1e6:.1f} MB)")
    print(counts)
    for f, s in summary.items():
        print(f, s)


if __name__ == "__main__":
    main()
