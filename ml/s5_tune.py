#!/usr/bin/env python3
"""Stage 5c: tune the simulator's demand scale, then score it once on the holdout.

Stage 4's sales estimates come from daily snapshots, and the Sept 2025 short
gaps put the true sale rate 2.5-3x higher at normal prices, so the absolute
demand level is a free parameter. For each faction, s5_sim runs on the `tune`
window (the 90 days before the holdout) at every scale in GRID, in parallel;
the scale with the lowest loss is then run once on `holdout`.

Loss, on liquid items: |median log sim/real| of units, price and
cheapest/median, plus |median sim - real| of presence over all items.

Output: out/s5/tune.parquet (one row per run), out/s5/tune_report.txt.
"""

import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import polars as pl

from common import ML, OUT

GRID = [0.5, 1, 1.5, 2, 3, 4, 6]
PARALLEL = int(os.environ.get("ML_WORKERS", "8"))


def sim(faction, window, k):
    tag = f"{faction}_{window}_k{k:g}"
    env = dict(os.environ, OMP_NUM_THREADS="2", POLARS_MAX_THREADS="2")
    subprocess.run([sys.executable, "s5_sim.py", "--faction", faction, "--window", window, "--demand-scale", str(k),
                    "--tag", tag], cwd=ML, env=env, check=True, stdout=subprocess.DEVNULL)
    return json.loads((OUT / "s5" / f"sim_{tag}.json").read_text())


def loss(r):
    liq, all_ = r["scores"]["liquid"], r["scores"]["all"]
    return (abs(liq["units"]["median"]) + abs(liq["price"]["median"]) + abs(liq["min_med"]["median"])
            + abs(all_["presence"]["median"]))


def row(r):
    out = {"faction": r["faction"], "window": r["window"], "k": r["demand_scale"], "loss": loss(r),
           "bots": r["bots"], "items": r["items"], "sold": r["totals"]["sold"], "posted": r["totals"]["posted"],
           "unserved": r["totals"]["buyers_unserved"] / max(r["totals"]["buyers"], 1)}
    for tier in ("liquid", "all"):
        for m in ("units", "price", "min_med", "vol", "presence"):
            out[f"{tier}_{m}_bias"] = r["scores"][tier][m]["median"]
            out[f"{tier}_{m}_rank"] = r["scores"][tier][m]["rank"]
    return out


def main():
    runs = [(f, "tune", k) for f in ("horde", "alliance") for k in GRID]
    with ThreadPoolExecutor(PARALLEL) as ex:
        tuned = list(ex.map(lambda a: sim(*a), runs))
    best = {}
    for r in tuned:
        if r["faction"] not in best or loss(r) < loss(best[r["faction"]]):
            best[r["faction"]] = r
    with ThreadPoolExecutor(2) as ex:
        final = list(ex.map(lambda f: sim(f, "holdout", best[f]["demand_scale"]), best))
    table = pl.DataFrame([row(r) for r in tuned + final]).sort("faction", "window", "k")
    table.write_parquet(OUT / "s5" / "tune.parquet")
    cols = ["faction", "window", "k", "loss", "bots", "items", "unserved"] + \
        [f"liquid_{m}_{s}" for m in ("units", "price", "min_med", "vol") for s in ("bias", "rank")] + \
        ["all_presence_bias", "all_presence_rank"]
    with pl.Config(tbl_rows=-1, tbl_cols=-1, tbl_width_chars=250, float_precision=3):
        report = "\n".join(["demand-scale grid on the tune window, best scale on the holdout",
                            "bias = median log(sim/real) (presence: sim - real); rank = rank correlation across items",
                            "", str(table.select(cols))])
    (OUT / "s5" / "tune_report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
