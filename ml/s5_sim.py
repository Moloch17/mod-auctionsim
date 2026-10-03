#!/usr/bin/env python3
"""Stage 5b: hourly auction house simulator, v1 skeleton, scored against nerfed.

Inputs: out/auctions, out/s3 (posts, sellers, types, items, price_model.pkl),
out/s4/demand.parquet, out/s5/targets.parquet.

  ./s5_sim.py [--faction horde|alliance] [--items 300] [--days 37] [--burn-in 7]

* Items: the faction's --items most-posted items.
* Warm start: the real AH (those items) at the last snapshot before the
  targets' holdout window; remaining time drawn inside each TLEFT bucket.
* Sellers: every seller (empty name included, as one pooled seller) of those
  items, each posting each item as a Poisson process at the rate it posted
  it over the RATE_DAYS before the holdout. Price = the cheapest listing up
  x exp(stage-3 model + a sampled residual), or reference x exp(...) when
  nothing is up; stack = convention x exp(model + residual);
  duration 48/24 h for TLEFT-4 posters, else 12 h.
* Buyers: every listing sells, whole, at the stage-4 hourly hazard for its
  item class and price-to-reference bin (TLEFT-4 cells; overall bin rate
  where a class has too little data). Independent of competition in v1.
* Seller gold: buyout less the 5% cut. Deposits need vendor prices (world
  DB), not loaded yet.
* Measured once a simulated day at 20:00 UTC like the site's snapshots:
  units, lowest and unit-weighted median per-unit buyout. Days after
  --burn-in are compared, per item, with the holdout targets.

Outputs (out/s5/): sim_daily.parquet, sim_vs_target.parquet, sim_report.txt.
"""

import argparse
import pickle

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT, TLEFT_MAX_HOURS
from s4_demand import RATIO_BINS

S3, S4, S5 = OUT / "s3", OUT / "s4", OUT / "s5"
RATE_DAYS = 60
CUT = 0.05
TLEFT_MIN_HOURS = {1: 0, 2: 0.5, 3: 2, 4: 12}


def ratio_bin_index(r):
    return np.searchsorted(RATIO_BINS, r, side="right")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--faction", choices=["horde", "alliance"], default="horde")
    ap.add_argument("--items", type=int, default=300)
    ap.add_argument("--days", type=int, default=37)
    ap.add_argument("--burn-in", type=int, default=7)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args()
    rng = np.random.default_rng(args.seed)
    faction = {v: k for k, v in FACTION_NAMES.items()}[args.faction]

    targets = pl.read_parquet(S5 / "targets.parquet").filter(pl.col("faction") == faction)
    nh = pl.read_parquet(OUT / "nerfed_history.parquet", columns=["faction", "time"]).filter(pl.col("faction") == faction)
    holdout_start = int(nh["time"].max()) - 90 * 86400
    snaps = sorted(int(p.stem) for p in (OUT / "auctions" / args.faction).glob("*.parquet"))
    start = max(t for t in snaps if t <= holdout_start)

    posts = pl.read_parquet(S3 / "posts.parquet").filter(pl.col("faction") == faction)
    items_dim = pl.read_parquet(S3 / "items.parquet")
    sellers = pl.read_parquet(S3 / "sellers.parquet").filter(pl.col("faction") == faction)
    model = pickle.load(open(S3 / "price_model.pkl", "rb"))

    recent = posts.filter(pl.col("t1").is_between(start - RATE_DAYS * 86400, start))
    item_ids = (recent.group_by("item").agg(pl.col("new").sum()).sort("new", descending=True)
                .head(args.items)["item"].to_list())
    info = (posts.filter(pl.col("item").is_in(item_ids)).group_by("item")
            .agg(ref=pl.col("ref_unit").first(), conv=pl.col("count").mode().first(), maxc=pl.col("count").max())
            .join(items_dim, on="item", how="left"))
    info = {r["item"]: r for r in info.iter_rows(named=True)}
    itype_code = model["itype_codes"]

    # Posting processes: (seller, item) -> hourly rate.
    span_h = RATE_DAYS * 24
    rates = (recent.filter(pl.col("item").is_in(item_ids)).group_by("seller", "item").agg(n=pl.col("new").sum())
             .join(sellers.select("seller", "type", "tl4_share"), on="seller", how="left")
             .with_columns(rate_h=pl.col("n") / span_h, type=pl.col("type").fill_null(-1),
                           tl4_share=pl.col("tl4_share").fill_null(0.6)))
    p_seller = rates["seller"].to_list()
    p_item = rates["item"].to_numpy()
    p_rate = rates["rate_h"].to_numpy()
    p_type = rates["type"].to_numpy()
    p_tl4 = rates["tl4_share"].to_numpy()

    # Demand: hazard by (itype, ratio bin), falling back to the bin overall.
    d = pl.read_parquet(S4 / "demand.parquet").filter(pl.col("tleft0") == 4)
    overall = (d.group_by("ratio_bin").agg(h=(pl.col("hazard_h") * pl.col("n")).sum() / pl.col("n").sum()))
    overall = {str(r["ratio_bin"]): r["h"] for r in overall.iter_rows(named=True)}
    cell = {(r["itype"], str(r["ratio_bin"])): r["hazard_h"]
            for r in d.filter(pl.col("reliable")).iter_rows(named=True)}
    bin_labels = [f"<{RATIO_BINS[0]}"] + [f"{a}-{b}" for a, b in zip(RATIO_BINS, RATIO_BINS[1:])] + \
        [f">={RATIO_BINS[-1]}"]
    hazard = {}
    for it in item_ids:
        ty = info[it]["itype"]
        hazard[it] = np.array([cell.get((ty, b), overall.get(b, 0.0)) or 0.0 for b in bin_labels])

    # Warm start from the real snapshot.
    a = (pl.read_parquet(OUT / "auctions" / args.faction / f"{start}.parquet")
         .filter(pl.col("item").is_in(item_ids), pl.col("buyout") > 0))
    lo = a["tleft"].replace_strict(TLEFT_MIN_HOURS, default=0).to_numpy()
    hi = a["tleft"].replace_strict(TLEFT_MAX_HOURS, default=48).to_numpy()
    L = {"seller": a["seller"].to_list(), "item": a["item"].to_numpy(), "count": a["count"].to_numpy(),
         "unit": (a["buyout"] / a["count"]).to_numpy(), "end": rng.uniform(lo, hi)}

    gold, sold_units = {}, 0
    daily = []
    hour0 = (start // 3600) % 24
    for h in range(args.days * 24):
        now = float(h)
        # Expire, then sell.
        keep = L["end"] > now
        ratio = L["unit"] / np.array([info[i]["ref"] for i in L["item"]])
        b = ratio_bin_index(ratio)
        hz = np.array([hazard[i][k] for i, k in zip(L["item"], b)])
        sold = keep & (rng.random(len(hz)) < 1 - np.exp(-hz))
        for k in np.flatnonzero(sold):
            gold[L["seller"][k]] = gold.get(L["seller"][k], 0) + L["unit"][k] * L["count"][k] * (1 - CUT)
        sold_units += int(L["count"][sold].sum())
        keep &= ~sold
        L = {k: (np.asarray(v)[keep] if not isinstance(v, list) else [x for x, m in zip(v, keep) if m])
             for k, v in L.items()}

        # Post.
        n_new = rng.poisson(p_rate)
        idx = np.repeat(np.arange(len(n_new)), n_new)
        if len(idx):
            units_now, sellers_now, own_now, min_now = {}, {}, {}, {}
            for s, i, c, u in zip(L["seller"], L["item"], L["count"], L["unit"]):
                units_now[i] = units_now.get(i, 0) + c
                sellers_now.setdefault(i, set()).add(s)
                own_now[(s, i)] = own_now.get((s, i), 0) + 1
                min_now[i] = min(min_now.get(i, u), u)
            cheapest = np.array([min_now.get(p_item[j], np.nan) for j in idx])
            ref = np.array([info[p_item[j]]["ref"] for j in idx])
            weekday = ((start + h * 3600) // 86400 + 3) % 7 + 1  # 1970-01-01 was a Thursday; polars Mon=1
            x = np.array([[p_type[j], itype_code.get(info[p_item[j]]["itype"], -1), info[p_item[j]]["quality"],
                           info[p_item[j]]["ilevel"], np.log(info[p_item[j]]["ref"]),
                           np.log1p(units_now.get(p_item[j], 0)), len(sellers_now.get(p_item[j], ())),
                           own_now.get((p_seller[j], p_item[j]), 0), weekday, faction,
                           np.log(min_now[p_item[j]] / info[p_item[j]]["ref"]) if p_item[j] in min_now else np.nan]
                          for j in idx], dtype=float)
            vs_min = np.exp(model["price_min"].predict(x) + rng.choice(model["price_min_resid"], len(idx)))
            vs_ref = np.exp(model["price_ref"].predict(x[:, :-1]) + rng.choice(model["price_ref_resid"], len(idx)))
            unit = np.where(np.isnan(cheapest), ref * vs_ref, cheapest * vs_min)
            stack = np.exp(model["stack"].predict(x) + rng.choice(model["stack_resid"], len(idx)))
            conv = np.array([info[p_item[j]]["conv"] for j in idx])
            maxc = np.array([info[p_item[j]]["maxc"] for j in idx])
            count = np.clip(np.rint(conv * stack), 1, maxc)
            dur = np.where(rng.random(len(idx)) < p_tl4[idx], rng.choice([24.0, 48.0], len(idx)), 12.0)
            L["seller"] += [p_seller[j] for j in idx]
            L["item"] = np.concatenate([L["item"], p_item[idx]])
            L["count"] = np.concatenate([L["count"], count])
            L["unit"] = np.concatenate([L["unit"], unit])
            L["end"] = np.concatenate([L["end"], now + dur])

        if (hour0 + h) % 24 == 20:
            df = pl.DataFrame({"item": L["item"], "count": L["count"], "unit": L["unit"]})
            snap = (df.sort("unit").group_by("item").agg(
                units=pl.col("count").sum(), min_unit=pl.col("unit").min(),
                med_unit=pl.col("unit").filter(pl.col("count").cum_sum() >= pl.col("count").sum() / 2).first()))
            daily.append(snap.with_columns(day=pl.lit(h // 24)))
            print(f"day {h // 24}: {len(L['item'])} listings, {sold_units} units sold so far", end="\r", flush=True)
    print()

    sim = pl.concat(daily).filter(pl.col("day") >= args.burn_in)
    sim.write_parquet(S5 / "sim_daily.parquet")
    days = sim["day"].n_unique()
    full = pl.DataFrame({"item": item_ids}).join(
        sim.sort("day").with_columns(ret=(pl.col("med_unit").log() - pl.col("med_unit").log().shift(1)).over("item"))
        .group_by("item").agg(s_days=pl.len(), s_units_med=pl.col("units").median(),
                              s_price_med=pl.col("med_unit").median(),
                              s_min_med=(pl.col("min_unit") / pl.col("med_unit")).median(), s_vol=pl.col("ret").std()),
        on="item", how="left").with_columns(s_presence=pl.col("s_days").fill_null(0) / days)
    cmp = full.join(targets.filter(pl.col("window") == "holdout").select(
        "item", "presence", "units_med", "price_med", "min_med", "vol"), on="item", how="inner")
    cmp.write_parquet(S5 / "sim_vs_target.parquet")

    def mae_log(a, b):
        v = (cmp[a].log() - cmp[b].log()).drop_nans().drop_nulls()
        return float(v.abs().median()), float(v.median())

    def rank_corr(a, b):
        c = cmp.select(a, b).drop_nulls()
        return float(np.corrcoef(c[a].rank().to_numpy(), c[b].rank().to_numpy())[0, 1])

    lines = [f"faction {args.faction}, {len(item_ids)} items, start {start}, {args.days} days, "
             f"burn-in {args.burn_in}, posting processes {len(p_rate)}, units sold {sold_units}",
             f"items compared with holdout targets: {cmp.height}", ""]
    for name, s, t in [("units on AH", "s_units_med", "units_med"), ("median price", "s_price_med", "price_med"),
                       ("min/median", "s_min_med", "min_med"), ("volatility", "s_vol", "vol"),
                       ("presence", "s_presence", "presence")]:
        err, bias = mae_log(s, t)
        lines.append(f"{name:14s} median |log sim/target| {err:.3f}, median log sim/target {bias:+.3f}, "
                     f"rank corr across items {rank_corr(s, t):.2f}")
    (S5 / "sim_report.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
