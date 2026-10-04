#!/usr/bin/env python3
"""Stage 7c: does the player buyer do what MARKET_FORMAT.md promises, and can it be farmed?

Runs s6_market_sim with a scripted player at Market.Scale 0.05 (house warm-started from the real one):

  seller   keeps one listing up for each of a sample of items (by class and quality), always at the current
           market price m = min(reference, cheapest seller listing), 24 h; reposts as soon as it sells or expires.
           Reports the share sold within 24 h and the mean wait per group, next to the formula's prediction.
  flipper  buys the cheapest seller listing of liquid items whenever it sits below the reference and relists it at
           the new market price; reports gold made per day (it should lose: the buyer never pays more than the
           cheapest seller copy, and the AH takes 5%).

  ./s7_player_buyer.py [--days 30] [--bonus 0|1]

Output: out/s7/player_buyer_<bonus>.txt
"""

import argparse
import math
import os
from collections import defaultdict

import numpy as np
import polars as pl

from common import OUT

S7 = OUT / "s7"
QNAME = {0: "grey", 1: "white", 2: "green", 3: "blue", 4: "epic", 5: "legendary"}


class Seller:
    def __init__(self, sample):
        self.sample = sample  # item index -> group label
        self.gold = 0.0
        self.bought_units = self.sold_units = 0.0
        self.active = {}  # item index -> (posted hour, end hour)
        self.waits = defaultdict(list)  # group -> list of (sold?, hours)

    def step(self, v):
        mine = v["owner"] == v["agent_owner"]
        up = set(v["item"][mine].tolist())
        posts = []
        for i, (t0, end) in list(self.active.items()):
            if i not in up:
                sold = v["now"] < end - 1e-6
                self.waits[self.sample[i]].append((sold, v["now"] - t0))
                del self.active[i]
        sellers = v["item"][~mine]
        for i in self.sample:
            if i in self.active:
                continue
            on = v["unit"][~mine][sellers == i]
            m = min(v["ref"][i], on.min()) if len(on) else v["ref"][i]
            posts.append((i, 1, float(m), 24.0))
            self.active[i] = (v["now"], v["now"] + 24.0)
        return [], posts


class Flipper:
    def __init__(self, items):
        self.items = items
        self.gold = 0.0
        self.bought_units = self.sold_units = 0.0
        self.paid = self.paid_units = 0.0  # all purchases, for the cost of stock left unsold

    def step(self, v):
        mine = v["owner"] == v["agent_owner"]
        buys, posts = [], []
        for i in self.items:
            rows = np.flatnonzero((v["item"] == i) & ~mine)
            if not len(rows):
                continue
            k = rows[np.argmin(v["unit"][rows])]
            if v["unit"][k] >= v["ref"][i] or v["count"][k] > 20:
                continue
            buys.append(int(k))
            self.paid += float(v["unit"][k] * v["count"][k])
            self.paid_units += float(v["count"][k])
            rest = rows[rows != k]
            m = min(v["ref"][i], v["unit"][rest].min()) if len(rest) else v["ref"][i]
            posts.append((i, float(v["count"][k]), float(m), 24.0))
        return buys, posts


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--days", type=int, default=30)
    ap.add_argument("--bonus", choices=["0", "1"], default="0")
    args = ap.parse_args()
    os.environ["ML_PLAYER_QUALITY_BONUS"] = args.bonus
    from s6_market_sim import load_market, run
    market = str(OUT / "s6" / "ship.dat")
    m = load_market(market, 6)
    items = np.array(sorted(m["item"]))
    q = dict(pl.read_parquet(OUT / "item_template.parquet", columns=["item", "quality"]).iter_rows())
    cls_name = {}
    with open(market) as f:
        f.readline()
        while h := f.readline().split():
            for _ in range(int(h[1])):
                r = f.readline().rstrip("\n").split(":")
                if h[0] == "CLASS":
                    cls_name[int(r[0])] = r[1]
    rng = np.random.default_rng(1)
    sample = {}
    groups = defaultdict(list)
    for k, i in enumerate(items):
        groups[(cls_name.get(m["item"][i][0], "?"), QNAME.get(min(q.get(int(i), 1), 5), "?"))].append(k)
    for (c, qn), ks in groups.items():
        if c in ("Trade Goods", "Armor", "Weapon", "Gem", "Glyph", "Consumable", "Recipe") and len(ks) >= 10:
            for k in rng.choice(ks, min(25, len(ks)), replace=False):
                sample[int(k)] = f"{c} / {qn}"
    seller = Seller(sample)
    run(market, "horde", "holdout", 0.05, 100, args.days, 0, None, None, 3, "pb", agent=seller, write=False)

    # The formula's prediction for comparison: P(sold within 24 h at m) = 1 - 0.05^(24 * L / 24).
    from s6_market_sim import LIQUID_BUYERS_H, PLAYER_LIQUIDITY, QUALITY_FACTOR
    lines = [f"player buyer check: Market.Scale 0.05, {args.days} days, quality bonus {'on' if args.bonus == '1' else 'off'}",
             f"{'group':28s} {'listings':>8s} {'sold <24h':>9s} {'predicted':>9s} {'mean wait h':>11s}"]
    for g in sorted(set(sample.values())):
        res = seller.waits.get(g, [])
        ks = [k for k, gg in sample.items() if gg == g]
        pred = []
        for k in ks:
            qq = min(q.get(int(items[k]), 1), 4)
            b = m["item"][int(items[k])][5] * (QUALITY_FACTOR.get(qq, 0.0) if args.bonus == "1" else (0.0 if qq == 0 else 1.0))
            liq = min(1.0, (b / LIQUID_BUYERS_H) ** PLAYER_LIQUIDITY) if b > 0 else 0.0
            pred.append(1 - 0.05 ** liq)
        sold = [w for s, w in res if s]
        lines.append(f"{g:28s} {len(res):8d} {np.mean([s for s, _ in res]) if res else 0:9.0%} "
                     f"{np.mean(pred):9.0%} {np.mean(sold) if sold else float('nan'):11.1f}")
    lines.append(f"seller: player buyer paid {seller.gold / 10000:,.0f} gold over {args.days} days")

    liquid = [k for k in range(len(items)) if m["item"][int(items[k])][5] >= 0.5][:40]
    flip = Flipper(liquid)
    run(market, "horde", "holdout", 0.05, 100, args.days, 0, None, None, 4, "pbflip", agent=flip, write=False)
    unsold = max(flip.bought_units - flip.sold_units, 0) * flip.paid / max(flip.paid_units, 1)
    lines.append(f"flipper on {len(liquid)} liquid items: realised {flip.gold / 10000 / args.days:+,.1f} gold/day, "
                 f"with unsold stock at cost {(flip.gold + unsold) / 10000 / args.days:+,.1f} gold/day "
                 f"(bought {flip.bought_units:,.0f} units for {flip.paid / 10000:,.0f}g, sold {flip.sold_units:,.0f})")
    S7.mkdir(parents=True, exist_ok=True)
    (S7 / f"player_buyer_{args.bonus}.txt").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
