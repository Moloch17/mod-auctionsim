#!/usr/bin/env python3
"""Stage 6b: reference implementation of Market mode, scored against nerfed.

Runs MARKET_FORMAT.md's runtime on an exported auctionsim_market.dat, the way
the module does: 30-minute steps, house state taken once per step, posts then
buyers. The C++ must behave like this.

  ./s6_market_sim.py --market out/s6/market_holdout.dat --faction horde --window holdout
                     [--scale 1.0] [--bots 100] [--days 37] [--burn-in 7] [--demand-scale K] [--supply-scale S]

--scale is Market.Scale. At 1.0 the market is Lordaeron-sized, so units compare
directly with the targets; at smaller scales units are compared after dividing
by the scale. The house starts from the real snapshot before the window,
thinned to --scale (owners kept as anonymous sellers); there are no players.

Outputs (out/s6/): sim_<tag>_daily.parquet, sim_<tag>_items.parquet,
sim_<tag>.json (scores, counts, gold).

run() also takes, for stage 7: `offsets` {type: extra log-price offset} added
to every POLICY draw of that type, and `agent`: an object whose
step(view) -> (buys, posts) acts as a player each step after the bots post
and before buyers arrive -- buys are listing indices into view["unit"] etc.,
posts are (item_index, count, unit_price, hours). Its gold is agent.gold.
"""

import argparse
import json
import math
import time
from collections import defaultdict
from pathlib import Path

import numpy as np
import polars as pl

from common import FACTION_NAMES, OUT, TLEFT_MAX_HOURS

S6 = OUT / "s6"
DT = 0.5
CURVE_EDGES = np.array([0.25, 0.5, 0.7, 0.85, 1.0, 1.15, 1.4, 2.0, 3.0, 5.0])
MB_EDGES = [-1.0, -0.5, -0.25, -0.1, 0.0, 0.1, 0.25, 0.5, 1.0]
QP = np.array([0.02, 0.10, 0.25, 0.50, 0.75, 0.90, 0.98])
# MARKET_FORMAT.md: draws stay inside the 5th-95th percentile; price memory per (bot, item).
DRAW_LO, DRAW_HI = 0.05, 0.95
MEMORY_WEIGHT, MEMORY_HOURS = 0.75, 72.0
CUT, DEPOSIT_PER_12H, MIN_DEPOSIT = 0.05, 0.15, 100
TLEFT_MIN_HOURS = {1: 0.0, 2: 0.5, 3: 2.0, 4: 12.0}
SNAPSHOT_HOUR, LIQUID_UNITS = 20, 20


def load_market(path, faction):
    m = {"item": {}, "curve": {}, "weekday": {}, "policy": {}, "stack": {}, "craft": defaultdict(list),
         "bot": [], "basket": []}
    with open(path) as f:
        assert f.readline().split() == ["AUCTIONSIM_MARKET", "1"]
        while header := f.readline().split():
            section, n = header[0], int(header[1])
            for _ in range(n):
                r = f.readline().rstrip("\n").split(":")
                if int(r[0]) != faction:
                    continue
                if section == "META":
                    m["demand"], m["supply"] = float(r[1]), float(r[2])
                elif section == "ITEM":
                    m["item"][int(r[1])] = (int(r[2]), float(r[3]), int(r[4]), int(r[5]), int(r[6]), float(r[7]))
                elif section == "CURVE":
                    m["curve"][int(r[1])] = np.array([float(x) for x in r[2:11]])
                elif section == "WEEKDAY":
                    m["weekday"][int(r[1])] = np.array([float(x) for x in r[2:9]])
                elif section == "POLICY":
                    m["policy"][tuple(int(x) for x in r[1:6])] = (np.array([float(x) for x in r[6:13]]), float(r[13]))
                elif section == "STACK":
                    m["stack"][(int(r[1]), int(r[2]))] = np.array([float(x) for x in r[3:10]])
                elif section == "CRAFT":
                    m["craft"][int(r[1])].append((int(r[2]), int(r[3]), float(r[4])))
                elif section == "BOT":
                    m["bot"].append(r[2])
                elif section == "BASKET":
                    m["basket"].append((int(r[1]), int(r[2]), int(r[3]), float(r[4]), float(r[5]), float(r[6])))
    return m


def ub_of(units):
    return min(int(math.log1p(units)), 7)


def sb_of(n):
    return 5 if n >= 9 else 4 if n >= 5 else 3 if n >= 3 else n


def mb_of(cheapest, ref):
    if not math.isfinite(cheapest):
        return -1
    x = math.log(cheapest / ref)
    return min(sum(x >= e for e in MB_EDGES), 9)


def run(market_path, faction_name, window, scale, n_bots, days, burn_in, demand_scale, supply_scale, seed, tag,
        offsets=None, agent=None, write=True, empty_start=False):
    t0 = time.time()
    rng = np.random.default_rng(seed)
    faction = {v: k for k, v in FACTION_NAMES.items()}[faction_name]
    m = load_market(market_path, faction)
    demand = m["demand"] if demand_scale is None else demand_scale
    supply = m["supply"] if supply_scale is None else supply_scale
    targets = pl.read_parquet(OUT / "s5" / "targets.parquet").filter(pl.col("faction") == faction,
                                                                     pl.col("window") == window)
    start = int(targets["window_start"][0])

    items = np.array(sorted(m["item"]))
    pos = {int(i): k for k, i in enumerate(items)}
    n_items = len(items)
    cls = np.array([m["item"][i][0] for i in items])
    ref = np.array([m["item"][i][1] for i in items])
    conv = np.array([m["item"][i][2] for i in items], dtype=float)
    maxc = np.array([m["item"][i][3] for i in items], dtype=float)
    vendor = np.array([m["item"][i][4] for i in items], dtype=float)
    buyers_h = np.array([m["item"][i][5] for i in items])
    curve = np.array([m["curve"].get(c, m["curve"][-1]) for c in cls])
    mass = np.clip(curve - np.concatenate([curve[:, 1:], np.zeros((n_items, 1))], axis=1), 0, None)
    cum = np.cumsum(mass / np.maximum(mass.sum(axis=1, keepdims=True), 1e-12), axis=1)
    weekday = np.array([m["weekday"].get(c, m["weekday"].get(-1, np.ones(7))) for c in cls])
    craft = {pos[i]: [(pos.get(r), q, mg) for r, q, mg in rows] for i, rows in m["craft"].items() if i in pos}

    basket = [b for b in m["basket"] if b[1] in pos]
    b_owner = np.array([b[0] % n_bots for b in basket])
    b_item = np.array([pos[b[1]] for b in basket])
    b_type = np.array([b[2] for b in basket])
    b_rate = np.array([b[3] for b in basket]) * supply * scale * DT
    b_tl4 = np.array([b[4] for b in basket])
    b_batch = np.array([b[5] for b in basket])

    def policy(t, c, u, s, mb):
        for k in ((t, c, u, s, mb), (t, c, -1, -1, mb), (t, -1, -1, -1, mb), (-1, -1, -1, -1, mb)):
            if k in m["policy"]:
                return m["policy"][k]
        raise KeyError(mb)

    def stack_q(t, c):
        return m["stack"].get((t, c)) if (t, c) in m["stack"] else m["stack"].get((t, -1), m["stack"][(-1, -1)])

    def draw(q):
        return float(np.interp(DRAW_LO + (DRAW_HI - DRAW_LO) * rng.random(), QP, q))

    memory = {}  # (bot, item index) -> (per-unit price, hour posted)

    # Warm start: the real house before the window, thinned to the scale; owners anonymous (ids after the bots).
    name = FACTION_NAMES[faction]
    ok = set(pl.read_parquet(OUT / "snapshots.parquet").filter(pl.col("faction") == faction, pl.col("ok"))["scan_time"])
    warm_t = max(t for t in (int(p.stem) for p in (OUT / "auctions" / name).glob("*.parquet")) if t <= start and t in ok)
    warm = (pl.read_parquet(OUT / "auctions" / name / f"{warm_t}.parquet")
            .filter(pl.col("item").is_in(items.tolist()), pl.col("buyout") > 0))
    warm = warm.filter(pl.Series(rng.random(warm.height) < scale))
    if empty_start:  # as a fresh realm does (and the C++ parity run)
        warm = warm.head(0)
    owners = {s: n_bots + k for k, s in enumerate(warm["seller"].unique().to_list())}
    lo = warm["tleft"].replace_strict(TLEFT_MIN_HOURS, default=0.0, return_dtype=pl.Float64).to_numpy()
    hi = warm["tleft"].replace_strict(TLEFT_MAX_HOURS, default=48.0, return_dtype=pl.Float64).to_numpy()
    L = {"item": np.array([pos[i] for i in warm["item"].to_list()], dtype=np.int64),
         "owner": np.array([owners[s] for s in warm["seller"].to_list()], dtype=np.int64),
         "count": warm["count"].to_numpy().astype(float), "unit": (warm["buyout"] / warm["count"]).to_numpy(),
         "end": rng.uniform(lo, hi) - (start - warm_t) / 3600, "dep": np.zeros(warm.height),
         "type": np.full(warm.height, -9, dtype=np.int64)}
    n_owners = n_bots + len(owners)
    gold = np.zeros(n_bots)
    type_gold = defaultdict(float)
    agent_owner = n_owners
    n_owners += 1
    offsets = offsets or {}
    tot = defaultdict(float)
    daily = []
    steps = int(days * 24 / DT)

    def keep(mask):
        for k in L:
            L[k] = L[k][mask]

    for step in range(steps):
        now = step * DT
        clock = start + now * 3600
        keep(L["end"] > now)

        # House state, once per step.
        units = np.bincount(L["item"], weights=L["count"], minlength=n_items)
        uniq = np.unique(L["item"] * n_owners + L["owner"])
        sellers = np.bincount(uniq // n_owners, minlength=n_items)
        cheapest = np.full(n_items, np.inf)
        np.minimum.at(cheapest, L["item"], L["unit"])
        # Each bot's cheapest own listing per item: the memory's fallback (restarts, fills).
        bot_rows = L["owner"] < n_bots
        own_keys = L["owner"][bot_rows] * n_items + L["item"][bot_rows]
        own_order = np.argsort(own_keys, kind="stable")
        own_keys_sorted = own_keys[own_order]
        own_uniq, own_start = np.unique(own_keys_sorted, return_index=True)
        own_min = (np.minimum.reduceat(L["unit"][bot_rows][own_order], own_start) if len(own_start)
                   else np.array([]))

        def remembered(bot, item):
            hit = memory.get((bot, item))
            if hit and now - hit[1] <= MEMORY_HOURS:
                return hit[0]
            k = np.searchsorted(own_uniq, bot * n_items + item)
            if k < len(own_uniq) and own_uniq[k] == bot * n_items + item:
                return float(own_min[k])
            return None

        # Posts.
        ev = rng.poisson(b_rate)
        new = defaultdict(list)
        for j in np.flatnonzero(ev):
            i, t, c = b_item[j], b_type[j], cls[b_item[j]]
            for _ in range(ev[j]):
                n = 1 + rng.poisson(max(b_batch[j] - 1, 0))
                q, off = policy(t, c, ub_of(units[i]), sb_of(sellers[i]), mb_of(cheapest[i], ref[i]))
                off += offsets.get(t, 0.0)
                base = cheapest[i] if math.isfinite(cheapest[i]) else ref[i]
                price = base * math.exp(draw(q) + off)
                bot = int(b_owner[j])
                prior = remembered(bot, int(i))
                if prior:
                    price = math.exp(MEMORY_WEIGHT * math.log(prior) + (1 - MEMORY_WEIGHT) * math.log(price))
                floor = vendor[i]
                if i in craft:
                    cost = sum(qty * (cheapest[r] if r is not None and math.isfinite(cheapest[r]) else
                                      (ref[r] if r is not None else 0)) for r, qty, _ in craft[i])
                    floor = max(floor, craft[i][0][2] * cost)
                tot["floored"] += n * (price < floor)
                price = max(price, floor)
                memory[(bot, int(i))] = (price, now)
                count = min(max(round(conv[i] * math.exp(draw(stack_q(t, c)))), 1), maxc[i])
                dur = (24.0 if rng.random() < 0.5 else 48.0) if rng.random() < b_tl4[j] else 12.0
                dep = max(MIN_DEPOSIT, DEPOSIT_PER_12H * vendor[i] * count * dur / 12)
                gold[b_owner[j]] -= dep * n
                type_gold[int(t)] -= dep * n
                tot["posted"] += n
                tot["deposits"] += dep * n
                for k, v in (("item", i), ("owner", b_owner[j]), ("count", count), ("unit", price),
                             ("end", now + dur), ("dep", dep), ("type", t)):
                    new[k] += [v] * n
        if new:
            for k in L:
                L[k] = np.concatenate([L[k], np.array(new[k], dtype=L[k].dtype)])

        # A player (stage 7): buys whole listings at their buyout, then posts.
        if agent is not None:
            buys, posts = agent.step({"now": now, "item": L["item"], "unit": L["unit"], "count": L["count"],
                                      "owner": L["owner"], "agent_owner": agent_owner, "ref": ref,
                                      "cheapest": cheapest, "vendor": vendor, "cls": cls, "items": items})
            if len(buys):
                s = np.array(sorted(set(buys)))
                s = s[L["owner"][s] != agent_owner]
                paid = L["unit"][s] * L["count"][s]
                agent.gold -= float(paid.sum())
                agent.bought_units += float(L["count"][s].sum())
                bot = L["owner"][s] < n_bots
                np.add.at(gold, L["owner"][s][bot], (paid * (1 - CUT) + L["dep"][s])[bot])
                for ty, g in zip(L["type"][s][bot].tolist(), (paid * (1 - CUT) + L["dep"][s])[bot].tolist()):
                    type_gold[int(ty)] += g
                mask = np.ones(len(L["item"]), dtype=bool)
                mask[s] = False
                keep(mask)
            for i, count, price, hours in posts:
                dep = max(MIN_DEPOSIT, DEPOSIT_PER_12H * vendor[i] * count * hours / 12)
                agent.gold -= dep
                for k, v in (("item", i), ("owner", agent_owner), ("count", count), ("unit", price),
                             ("end", now + hours), ("dep", dep), ("type", -9)):
                    L[k] = np.concatenate([L[k], np.array([v], dtype=L[k].dtype)])

        # Buyers: cheapest listing at or under each one's reservation.
        wd = int((clock // 86400 + 3) % 7)  # 1970-01-01 was a Thursday; Monday = 0
        nb = rng.poisson(buyers_h * demand * scale * weekday[:, wd] * DT)
        if nb.sum():
            bi = rng.permutation(np.repeat(np.arange(n_items), nb))
            u = rng.random(len(bi))
            b = np.minimum((cum[bi] < u[:, None]).sum(axis=1), 8)
            cap = (CURVE_EDGES[b] + rng.random(len(bi)) * (CURVE_EDGES[b + 1] - CURVE_EDGES[b])) * ref[bi]
            order = np.lexsort((L["unit"], L["item"]))
            si = L["item"][order]
            ptr = np.searchsorted(si, np.arange(n_items), side="left")
            end = np.searchsorted(si, np.arange(n_items), side="right")
            sold = []
            for x, c in zip(bi.tolist(), cap.tolist()):
                p = ptr[x]
                if p < end[x] and L["unit"][order[p]] <= c:
                    sold.append(order[p])
                    ptr[x] = p + 1
            tot["buyers"] += len(bi)
            tot["sold"] += len(sold)
            if sold:
                s = np.array(sold)
                bot = L["owner"][s] < n_bots
                proceeds = L["unit"][s] * L["count"][s] * (1 - CUT) + L["dep"][s]
                np.add.at(gold, L["owner"][s][bot], proceeds[bot])
                for ty, g in zip(L["type"][s][bot].tolist(), proceeds[bot].tolist()):
                    type_gold[int(ty)] += g
                ag = L["owner"][s] == agent_owner
                if agent is not None and ag.any():
                    agent.gold += float(proceeds[ag].sum())
                    agent.sold_units += float(L["count"][s][ag].sum())
                tot["spent"] += float((L["unit"][s] * L["count"][s]).sum())
                mask = np.ones(len(L["item"]), dtype=bool)
                mask[s] = False
                keep(mask)

        if int(clock // 3600) % 24 == SNAPSHOT_HOUR and (clock % 3600) < DT * 3600:
            snap = (pl.DataFrame({"item": items[L["item"]], "count": L["count"], "unit": L["unit"]}).sort("unit")
                    .group_by("item").agg(units=pl.col("count").sum(), min_unit=pl.col("unit").min(),
                                          med_unit=pl.col("unit").filter(
                                              pl.col("count").cum_sum() >= pl.col("count").sum() / 2).first()))
            # Running totals ride along, so a long run can be read day by day (s6_longrun.py).
            daily.append(snap.with_columns(
                day=pl.lit(int(now // 24)), listings=pl.lit(len(L["item"])),
                **{f"{k}_cum": pl.lit(float(tot[k])) for k in ("posted", "sold", "buyers", "spent", "floored")}))

    # Same seller, same item: how often its listings' per-unit prices sit far apart (real Lordaeron sellers:
    # 3-6% of groups above 1.5x, 1-3% above 3x).
    spread = (pl.DataFrame({"owner": L["owner"], "item": L["item"], "unit": L["unit"]})
              .filter(pl.col("owner") < n_bots).group_by("owner", "item")
              .agg(n=pl.len(), r=pl.col("unit").max() / pl.col("unit").min()).filter(pl.col("n") > 1))
    seller_spread = {"groups": spread.height, "over_1_5x": float((spread["r"] > 1.5).mean()) if spread.height else 0.0,
                     "over_3x": float((spread["r"] > 3).mean()) if spread.height else 0.0}
    if not write:
        return {"type_gold": dict(type_gold), "totals": dict(tot), "days": days,
                "agent_gold": agent.gold if agent is not None else None, "daily": pl.concat(daily)}
    sim = pl.concat(daily).filter(pl.col("day") >= burn_in)
    n_days = sim["day"].n_unique()
    per_item = (sim.sort("day").with_columns(
                    ret=(pl.col("med_unit").log() - pl.col("med_unit").log().shift(1)).over("item"))
                .group_by("item").agg(s_days=pl.len(), s_units_med=pl.col("units").median() / scale,
                                      s_price_med=pl.col("med_unit").median(),
                                      s_min_med=(pl.col("min_unit") / pl.col("med_unit")).median(),
                                      s_vol=pl.col("ret").std()))
    cmp = (targets.select("item", "days", "presence", "units_med", "price_med", "min_med", "vol")
           .filter(pl.col("days") >= 10).join(pl.DataFrame({"item": items}), on="item", how="inner")
           .join(per_item, on="item", how="left").with_columns(s_presence=pl.col("s_days").fill_null(0) / n_days))
    S6.mkdir(parents=True, exist_ok=True)
    sim.write_parquet(S6 / f"sim_{tag}_daily.parquet")
    cmp.write_parquet(S6 / f"sim_{tag}_items.parquet")

    def score(s, t, log=True, liquid=False):
        c = cmp.filter(pl.col("units_med") >= LIQUID_UNITS) if liquid else cmp
        c = c.select(s, t).drop_nulls().drop_nans()
        if log:
            c = c.filter(pl.col(s) > 0, pl.col(t) > 0)
        a, b = c[s].to_numpy(), c[t].to_numpy()
        d = np.log(a) - np.log(b) if log else a - b
        rank = float(np.corrcoef(c[s].rank().to_numpy(), c[t].rank().to_numpy())[0, 1]) if len(c) > 2 else None
        return {"n": len(c), "median_abs": float(np.median(np.abs(d))), "median": float(np.median(d)),
                "rank": rank, "sim_median": float(np.median(a)), "real_median": float(np.median(b))}

    result = {
        "market": str(market_path), "faction": faction_name, "window": window, "scale": scale, "bots": n_bots,
        "demand_scale": demand, "supply_scale": supply, "start": start, "days": days, "burn_in": burn_in,
        "items": n_items, "basket_rows": len(basket), "items_scored": cmp.height, "totals": dict(tot),
        "listings_median": float(sim["listings"].median()), "seconds": round(time.time() - t0),
        "bot_gold_total": float(gold.sum()), "bot_gold_min": float(gold.min()),
        "type_gold": {str(k): v for k, v in type_gold.items()},
        "seller_spread": seller_spread,
        "scores": {tier: {"units": score("s_units_med", "units_med", liquid=liq),
                          "price": score("s_price_med", "price_med", liquid=liq),
                          "min_med": score("s_min_med", "min_med", liquid=liq),
                          "vol": score("s_vol", "vol", liquid=liq),
                          "presence": score("s_presence", "presence", log=False, liquid=liq)}
                   for tier, liq in (("all", False), ("liquid", True))},
    }
    (S6 / f"sim_{tag}.json").write_text(json.dumps(result, indent=2))
    return result


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--market", required=True)
    ap.add_argument("--faction", choices=["horde", "alliance"], default="horde")
    ap.add_argument("--window", choices=["tune", "holdout"], default="holdout")
    ap.add_argument("--scale", type=float, default=1.0)
    ap.add_argument("--bots", type=int, default=100)
    ap.add_argument("--days", type=int, default=37)
    ap.add_argument("--burn-in", type=int, default=7)
    ap.add_argument("--demand-scale", type=float, default=None)
    ap.add_argument("--supply-scale", type=float, default=None)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument("--tag", default=None)
    ap.add_argument("--empty-start", action="store_true", help="start from an empty house, like a fresh realm")
    args = ap.parse_args()
    tag = args.tag or f"{args.faction}_{args.window}_s{args.scale:g}_{Path(args.market).stem}"
    r = run(args.market, args.faction, args.window, args.scale, args.bots, args.days, args.burn_in,
            args.demand_scale, args.supply_scale, args.seed, tag, empty_start=args.empty_start)
    print(json.dumps({k: v for k, v in r.items()}, indent=1))


if __name__ == "__main__":
    main()
