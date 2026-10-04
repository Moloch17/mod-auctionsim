#!/usr/bin/env python3
"""Stage 5b: hourly auction house simulator, every item, scored against nerfed.

Inputs: out/auctions, out/s3 (posts, sellers, items, price_model.pkl),
out/s4 (reservation, item_sales), out/item_template.parquet,
out/s5/targets.parquet.

  ./s5_sim.py --faction horde --window holdout --demand-scale 1 [--days 37 --burn-in 7 --tag NAME]

* Window: the simulation starts where the targets' `tune` or `holdout`
  window starts and is scored against that window's targets.
* Items: every item posted in the RATE_DAYS before the start.
* Warm start: the real AH (those items) at the last snapshot before the
  start; remaining time drawn inside each TLEFT bucket.
* Bots: every seller of those items (empty names pooled as one), each posting
  each item as a Poisson process at the rate it did over RATE_DAYS. Price =
  the cheapest listing up x exp(stage-3 model + a residual drawn from the
  seller's own type), or reference x exp(...) when nothing is up; never below
  the item's vendor sell price (real posts go under it 0.14% of the time).
  Stack = convention x exp(model + residual), capped at the item's stack
  size. Duration 48/24 h for TLEFT-4 posters, else 12 h.
* Deposit, as AuctionHouseMgr::GetAuctionDeposit for a faction AH: 15% of the
  vendor sell price x count per 12 h, at least 100 copper; returned on sale.
  Sale proceeds less the 5% cut. Both go to each bot's gold.
* Buyers: per item per hour, Poisson(demand_scale x the item's estimated
  listings sold per hour over RATE_DAYS, stage 4). Each has a reservation
  price drawn from the item class's stage-4 curve (x reference) and buys the
  cheapest listing at or under it, whole; a buyer who finds none leaves.
* Measured once a simulated day at 20:00 UTC like the site's snapshots:
  units, lowest and unit-weighted median per-unit buyout. Days after
  --burn-in are compared per item with the window's targets, over all items
  and over liquid ones (LIQUID_UNITS+ units up in the real window): thin items
  (a median of 2 units, cheapest = median) dominate the all-item medians.

Outputs (out/s5/): sim_<tag>_daily.parquet, sim_<tag>_items.parquet,
sim_<tag>.json (scores, bot counts, gold by type).
"""

import argparse
import json
import pickle
import time

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT, TLEFT_MAX_HOURS, add_reference
from s4_demand import BIN_LABELS, RATIO_BINS

S3, S4, S5 = OUT / "s3", OUT / "s4", OUT / "s5"
RATE_DAYS = 60
CUT = 0.05
DEPOSIT_PER_12H = 0.15
MIN_DEPOSIT = 100
TLEFT_MIN_HOURS = {1: 0.0, 2: 0.5, 3: 2.0, 4: 12.0}
SNAPSHOT_HOUR = 20
LIQUID_UNITS = 20


def load(faction, start):
    name = FACTION_NAMES[faction]
    posts = pl.scan_parquet(S3 / "posts.parquet").filter(
        pl.col("faction") == faction, pl.col("t1").is_between(start - RATE_DAYS * 86400, start, closed="left")
    ).collect()
    sellers = pl.read_parquet(S3 / "sellers.parquet").filter(pl.col("faction") == faction)
    template = pl.read_parquet(OUT / "item_template.parquet")
    items_dim = pl.read_parquet(S3 / "items.parquet")

    rates = (posts.group_by("seller", "item").agg(n=pl.col("new").sum())
             .join(sellers.select("seller", "type", "tl4_share"), on="seller", how="left")
             .with_columns(rate_h=pl.col("n") / (RATE_DAYS * 24), type=pl.col("type").fill_null(-1),
                           tl4_share=pl.col("tl4_share").fill_null(0.6)))
    sales = (pl.scan_parquet(S4 / "item_sales.parquet")
             .filter(pl.col("faction") == faction,
                     pl.col("t0").is_between(start - RATE_DAYS * 86400, start, closed="left"))
             .group_by("item").agg(sold_h=pl.col("sold").sum() / pl.col("gap_h").sum()).collect())
    ref = add_reference(pl.LazyFrame({"item": rates["item"].unique(), "t": start}), faction, "t").collect()
    info = (posts.group_by("item").agg(conv=pl.col("count").mode().first(), maxc=pl.col("count").max())
            .join(ref.select("item", "ref"), on="item", how="left")
            .join(items_dim.select("item", "itype", "quality", "ilevel"), on="item", how="left")
            .join(template.select("item", "sell_price", "stackable"), on="item", how="left")
            .join(sales, on="item", how="left")
            .with_columns(sold_h=pl.col("sold_h").fill_null(0.0), sell_price=pl.col("sell_price").fill_null(0),
                          maxc=pl.max_horizontal("maxc", pl.col("stackable").fill_null(1)))
            .filter(pl.col("ref").is_not_null())
            .sort("item"))
    rates = rates.filter(pl.col("item").is_in(info["item"].implode()))
    snaps = sorted(int(p.stem) for p in (OUT / "auctions" / name).glob("*.parquet"))
    ok = set(pl.read_parquet(OUT / "snapshots.parquet").filter(pl.col("faction") == faction, pl.col("ok"))["scan_time"])
    warm_t = max(t for t in snaps if t <= start and t in ok)
    warm = (pl.read_parquet(OUT / "auctions" / name / f"{warm_t}.parquet")
            .filter(pl.col("item").is_in(info["item"].implode()), pl.col("buyout") > 0))
    return info, rates, warm, warm_t


def reservation_sampler(info, rng):
    """draw(item_idx) -> reservation prices as a ratio to each item's reference."""
    res = pl.read_parquet(S4 / "reservation.parquet").with_columns(pl.col("ratio_bin").cast(pl.String))
    curves = {}
    for (itype,), d in res.group_by("itype"):
        w = {r["ratio_bin"]: max(r["willing"], 0.0) for r in d.to_dicts()}
        curves[itype] = np.array([w.get(b, 0.0) for b in BIN_LABELS])
    fallback = curves[None]
    edges = np.array([0.25] + RATIO_BINS + [5.0])
    per_item = np.array([curves.get(t, fallback) for t in info["itype"].to_list()])
    # willing[b] = share paying at least bin b's price; mass in bin b = willing[b] - willing[b+1].
    mass = np.clip(per_item - np.concatenate([per_item[:, 1:], np.zeros((len(per_item), 1))], axis=1), 0, None)
    mass /= mass.sum(axis=1, keepdims=True)
    cum = np.cumsum(mass, axis=1)

    def draw(item_idx):
        u = rng.random(len(item_idx))
        b = np.minimum((cum[item_idx] < u[:, None]).sum(axis=1), len(edges) - 2)
        return edges[b] + rng.random(len(item_idx)) * (edges[b + 1] - edges[b])
    return draw


def run(faction, window, demand_scale, days, burn_in, seed, tag):
    t_start = time.time()
    rng = np.random.default_rng(seed)
    targets = pl.read_parquet(S5 / "targets.parquet").filter(pl.col("faction") == faction,
                                                             pl.col("window") == window)
    start = int(targets["window_start"][0])
    info, rates, warm, warm_t = load(faction, start)
    model = pickle.load(open(S3 / "price_model.pkl", "rb"))

    items = info["item"].to_numpy()
    n_items = len(items)
    item_pos = {int(i): k for k, i in enumerate(items)}
    ref = info["ref"].to_numpy()
    conv = info["conv"].to_numpy().astype(float)
    maxc = info["maxc"].to_numpy().astype(float)
    vendor = info["sell_price"].to_numpy().astype(float)
    buyers_h = demand_scale * info["sold_h"].to_numpy()
    itype_code = np.array([model["itype_codes"].get(t, -1) for t in info["itype"].to_list()], dtype=float)
    item_x = np.column_stack([itype_code, info["quality"].fill_null(0).to_numpy(),
                              info["ilevel"].fill_null(0).to_numpy(), np.log(ref)]).astype(float)

    seller_names = sorted(set(rates["seller"].to_list()) | set(warm["seller"].to_list()))
    seller_pos = {s: k for k, s in enumerate(seller_names)}
    n_sellers = len(seller_names)
    seller_type = np.full(n_sellers, -1)
    for s, t in zip(rates["seller"].to_list(), rates["type"].to_list()):
        seller_type[seller_pos[s]] = t
    p_seller = np.array([seller_pos[s] for s in rates["seller"].to_list()])
    p_item = np.array([item_pos[i] for i in rates["item"].to_list()])
    p_rate = rates["rate_h"].to_numpy()
    p_tl4 = rates["tl4_share"].to_numpy()

    def residuals(key, types):
        table = model[key]
        pooled = np.concatenate(list(table.values()))
        out = np.empty(len(types))
        for t in np.unique(types):
            m = types == t
            out[m] = rng.choice(table.get(int(t), pooled), m.sum())
        return out

    draw_reservation = reservation_sampler(info, rng)

    lo = warm["tleft"].replace_strict(TLEFT_MIN_HOURS, default=0.0, return_dtype=pl.Float64).to_numpy()
    hi = warm["tleft"].replace_strict(TLEFT_MAX_HOURS, default=48.0, return_dtype=pl.Float64).to_numpy()
    L_item = np.array([item_pos[i] for i in warm["item"].to_list()], dtype=np.int64)
    L_seller = np.array([seller_pos[s] for s in warm["seller"].to_list()], dtype=np.int64)
    L_count = warm["count"].to_numpy().astype(float)
    L_unit = (warm["buyout"] / warm["count"]).to_numpy()
    L_end = rng.uniform(lo, hi) - (start - warm_t) / 3600
    L_dep = np.zeros(len(L_item))
    gold = np.zeros(n_sellers)
    totals = {"posted": 0, "sold": 0, "sold_units": 0.0, "expired": 0, "buyers": 0, "buyers_unserved": 0,
              "deposits": 0.0, "floored": 0}
    daily = []
    hour0 = (start // 3600) % 24
    weekday0 = (start // 86400 + 3) % 7  # 1970-01-01 was a Thursday; Monday = 0

    def keep_only(mask):
        return tuple(a[mask] for a in (L_item, L_seller, L_count, L_unit, L_end, L_dep))

    for h in range(days * 24):
        now = float(h)
        # Expire (deposits stay lost).
        alive = L_end > now
        totals["expired"] += int((~alive).sum())
        L_item, L_seller, L_count, L_unit, L_end, L_dep = keep_only(alive)

        # Buyers: the cheapest listing at or under each one's reservation price.
        n_b = rng.poisson(buyers_h)
        if n_b.sum():
            b_item = rng.permutation(np.repeat(np.arange(n_items), n_b))
            cap = draw_reservation(b_item) * ref[b_item]
            order = np.lexsort((L_unit, L_item))
            sorted_items = L_item[order]
            ptr = np.searchsorted(sorted_items, np.arange(n_items), side="left")
            seg_end = np.searchsorted(sorted_items, np.arange(n_items), side="right")
            sold_idx = []
            for bi, c in zip(b_item.tolist(), cap.tolist()):
                p = ptr[bi]
                if p < seg_end[bi] and L_unit[order[p]] <= c:
                    sold_idx.append(order[p])
                    ptr[bi] = p + 1
            totals["buyers"] += len(b_item)
            totals["buyers_unserved"] += len(b_item) - len(sold_idx)
            if sold_idx:
                s = np.array(sold_idx)
                np.add.at(gold, L_seller[s], L_unit[s] * L_count[s] * (1 - CUT) + L_dep[s])
                totals["sold"] += len(s)
                totals["sold_units"] += float(L_count[s].sum())
                keep = np.ones(len(L_item), dtype=bool)
                keep[s] = False
                L_item, L_seller, L_count, L_unit, L_end, L_dep = keep_only(keep)

        # Bots post.
        n_new = rng.poisson(p_rate)
        idx = np.repeat(np.arange(len(n_new)), n_new)
        if len(idx):
            it, se = p_item[idx], p_seller[idx]
            units_now = np.bincount(L_item, weights=L_count, minlength=n_items)
            uniq, cnt = np.unique(L_item * n_sellers + L_seller, return_counts=True)
            sellers_now = np.bincount(uniq // n_sellers, minlength=n_items)
            q = it * n_sellers + se
            pos = np.minimum(np.searchsorted(uniq, q), max(len(uniq) - 1, 0))
            own = np.where((len(uniq) > 0) & (uniq[pos] == q), cnt[pos], 0) if len(uniq) else np.zeros(len(q))
            min_now = np.full(n_items, np.inf)
            np.minimum.at(min_now, L_item, L_unit)
            cheapest = min_now[it]
            has = np.isfinite(cheapest)
            safe = np.where(has, cheapest, 1.0)
            weekday = (weekday0 + (hour0 + h) // 24) % 7 + 1  # polars: Monday = 1
            x = np.column_stack([seller_type[se], item_x[it, 0], item_x[it, 1], item_x[it, 2], item_x[it, 3],
                                 np.log1p(units_now[it]), sellers_now[it], own, np.full(len(idx), weekday),
                                 np.full(len(idx), faction), np.where(has, np.log(safe / ref[it]), np.nan)])
            types = seller_type[se]
            unit = np.where(has, safe * np.exp(model["price_min"].predict(x) + residuals("price_min_resid", types)),
                            ref[it] * np.exp(model["price_ref"].predict(x[:, :-1])
                                             + residuals("price_ref_resid", types)))
            totals["floored"] += int((unit < vendor[it]).sum())
            unit = np.maximum(unit, vendor[it])
            count = np.clip(np.rint(conv[it] * np.exp(model["stack"].predict(x) + residuals("stack_resid", types))),
                            1, maxc[it])
            dur = np.where(rng.random(len(idx)) < p_tl4[idx], rng.choice([24.0, 48.0], len(idx)), 12.0)
            dep = np.maximum(MIN_DEPOSIT, DEPOSIT_PER_12H * vendor[it] * count * dur / 12)
            np.add.at(gold, se, -dep)
            totals["posted"] += len(idx)
            totals["deposits"] += float(dep.sum())
            L_item = np.concatenate([L_item, it])
            L_seller = np.concatenate([L_seller, se])
            L_count = np.concatenate([L_count, count])
            L_unit = np.concatenate([L_unit, unit])
            L_end = np.concatenate([L_end, now + dur])
            L_dep = np.concatenate([L_dep, dep])

        if (hour0 + h) % 24 == SNAPSHOT_HOUR:
            snap = (pl.DataFrame({"item": items[L_item], "count": L_count, "unit": L_unit}).sort("unit")
                    .group_by("item").agg(units=pl.col("count").sum(), min_unit=pl.col("unit").min(),
                                          med_unit=pl.col("unit").filter(
                                              pl.col("count").cum_sum() >= pl.col("count").sum() / 2).first()))
            daily.append(snap.with_columns(day=pl.lit(h // 24)))

    sim = pl.concat(daily).filter(pl.col("day") >= burn_in)
    n_days = sim["day"].n_unique()
    per_item = (sim.sort("day").with_columns(
                    ret=(pl.col("med_unit").log() - pl.col("med_unit").log().shift(1)).over("item"))
                .group_by("item").agg(s_days=pl.len(), s_units_med=pl.col("units").median(),
                                      s_price_med=pl.col("med_unit").median(),
                                      s_min_med=(pl.col("min_unit") / pl.col("med_unit")).median(),
                                      s_vol=pl.col("ret").std()))
    cmp = (targets.select("item", "days", "presence", "units_med", "price_med", "min_med", "vol")
           .filter(pl.col("days") >= 10)
           .join(pl.DataFrame({"item": items}), on="item", how="inner")
           .join(per_item, on="item", how="left")
           .with_columns(s_presence=pl.col("s_days").fill_null(0) / n_days))
    S5.mkdir(parents=True, exist_ok=True)
    sim.write_parquet(S5 / f"sim_{tag}_daily.parquet")
    cmp.write_parquet(S5 / f"sim_{tag}_items.parquet")

    def score(s, t, log=True, liquid=False):
        c = cmp.filter(pl.col("units_med") >= LIQUID_UNITS) if liquid else cmp
        c = c.select(s, t).drop_nulls().drop_nans()
        if log:
            c = c.filter(pl.col(s) > 0, pl.col(t) > 0)
        a, b = c[s].to_numpy(), c[t].to_numpy()
        d = np.log(a) - np.log(b) if log else a - b
        rank = float(np.corrcoef(c[s].rank().to_numpy(), c[t].rank().to_numpy())[0, 1]) if len(c) > 2 else None
        return {"n": len(c), "median_abs": float(np.median(np.abs(d))), "median": float(np.median(d)), "rank": rank,
                "sim_median": float(np.median(a)), "real_median": float(np.median(b))}

    result = {
        "faction": FACTION_NAMES[faction], "window": window, "demand_scale": demand_scale, "start": start,
        "days": days, "burn_in": burn_in, "items": n_items, "bots": n_sellers, "posting_processes": len(p_rate),
        "items_scored": cmp.height, "totals": totals, "seconds": round(time.time() - t_start),
        "gold_by_type": {int(t): float(gold[seller_type == t].sum()) for t in np.unique(seller_type)},
        "scores": {tier: {"units": score("s_units_med", "units_med", liquid=liq),
                          "price": score("s_price_med", "price_med", liquid=liq),
                          "min_med": score("s_min_med", "min_med", liquid=liq),
                          "vol": score("s_vol", "vol", liquid=liq),
                          "presence": score("s_presence", "presence", log=False, liquid=liq)}
                   for tier, liq in (("all", False), ("liquid", True))},
    }
    (S5 / f"sim_{tag}.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--faction", choices=["horde", "alliance"], default="horde")
    ap.add_argument("--window", choices=["tune", "holdout"], default="holdout")
    ap.add_argument("--demand-scale", type=float, default=1.0)
    ap.add_argument("--days", type=int, default=37)
    ap.add_argument("--burn-in", type=int, default=7)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--tag", default=None)
    args = ap.parse_args()
    faction = {v: k for k, v in FACTION_NAMES.items()}[args.faction]
    tag = args.tag or f"{args.faction}_{args.window}_k{args.demand_scale:g}"
    r = run(faction, args.window, args.demand_scale, args.days, args.burn_in, args.seed, tag)
    print(json.dumps({k: v for k, v in r.items() if k != "gold_by_type"}, indent=1))


if __name__ == "__main__":
    main()
