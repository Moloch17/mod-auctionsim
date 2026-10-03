#!/usr/bin/env python3
"""Stage 6c: tune Market mode's supply and demand scales and curve tilt, score
the result on the holdout, and write the shipping export.

1. Export as of the `tune` window's start for each tilt in TILTS.
2. Run s6_market_sim on the tune window at Market.Scale 1 for every (tilt,
   supply, demand) in the grid, both factions, in parallel.
3. Per faction, pick the lowest loss: on liquid items |median log sim/real| of
   units, price, cheapest/median and volatility, plus |median sim - real| of
   presence over all items. Tilt is shared (one curve file), chosen on the
   summed loss; scales are per faction (META).
4. Export as of the holdout start with those values; score on the holdout at
   Market.Scale 1 (comparable with Lordaeron) and 0.1 (the module default).
5. Export as of the newest scan with those values: out/s6/auctionsim_market.dat,
   the file to ship.

Output: out/s6/tune.parquet, out/s6/tune_report.txt, the market files.
"""

import itertools
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import polars as pl

from common import ML, OUT

S6 = OUT / "s6"
TILTS = [1.0, 1.5, 2.0]
SUPPLY = [1.0, 1.5, 2.0]
DEMAND = [1.0, 2.0, 3.0]
PARALLEL = int(os.environ.get("ML_WORKERS", "8"))
ENV = dict(os.environ, OMP_NUM_THREADS="1", POLARS_MAX_THREADS="2")


def py(*args):
    subprocess.run([sys.executable, *args], cwd=ML, env=ENV, check=True, stdout=subprocess.DEVNULL)


def export(asof, out, tilt, demand=2.0, supply=1.0):
    py("s6_export.py", "--asof", asof, "--out", str(out), "--tilt", str(tilt), "--demand-scale", str(demand),
       "--supply-scale", str(supply))


def sim(market, faction, window, scale, demand=None, supply=None, tag=None):
    args = ["s6_market_sim.py", "--market", str(market), "--faction", faction, "--window", window,
            "--scale", str(scale), "--tag", tag]
    if demand is not None:
        args += ["--demand-scale", str(demand), "--supply-scale", str(supply)]
    py(*args)
    return json.loads((S6 / f"sim_{tag}.json").read_text())


def loss(r):
    liq, all_ = r["scores"]["liquid"], r["scores"]["all"]
    return sum(abs(liq[m]["median"]) for m in ("units", "price", "min_med", "vol")) + abs(all_["presence"]["median"])


def row(r, tilt):
    out = {"faction": r["faction"], "window": r["window"], "scale": r["scale"], "tilt": tilt,
           "supply": r["supply_scale"], "demand": r["demand_scale"], "loss": loss(r),
           "listings": r["listings_median"], "sold_share": r["totals"].get("sold", 0) / max(r["totals"]["buyers"], 1)}
    for tier in ("liquid", "all"):
        for m in ("units", "price", "min_med", "vol", "presence"):
            out[f"{tier}_{m}_bias"] = r["scores"][tier][m]["median"]
            out[f"{tier}_{m}_rank"] = r["scores"][tier][m]["rank"]
    return out


def main():
    S6.mkdir(parents=True, exist_ok=True)
    with ThreadPoolExecutor(len(TILTS)) as ex:
        list(ex.map(lambda t: export("tune", S6 / f"market_tune_t{t:g}.dat", t), TILTS))
    grid = list(itertools.product(TILTS, SUPPLY, DEMAND, ("horde", "alliance")))
    with ThreadPoolExecutor(PARALLEL) as ex:
        runs = list(ex.map(lambda g: (g[0], sim(S6 / f"market_tune_t{g[0]:g}.dat", g[3], "tune", 1.0, g[2], g[1],
                                                f"tune_{g[3]}_t{g[0]:g}_s{g[1]:g}_d{g[2]:g}")), grid))
    rows = [row(r, t) for t, r in runs]
    table = pl.DataFrame(rows)
    best_tilt = (table.group_by("tilt", "faction").agg(pl.col("loss").min()).group_by("tilt")
                 .agg(pl.col("loss").sum()).sort("loss")["tilt"][0])
    best = {f: table.filter(pl.col("tilt") == best_tilt, pl.col("faction") == f).sort("loss").row(0, named=True)
            for f in ("horde", "alliance")}

    # One file carries both factions' scales: export with horde's, override alliance's in the sim; then fix META.
    def export_final(asof, out):
        export(asof, out, best_tilt, best["horde"]["demand"], best["horde"]["supply"])
        lines = out.read_text().split("\n")
        k = lines.index(next(l for l in lines if l.startswith("META ")))
        for i in range(k + 1, k + 3):
            f, _, _, *rest = lines[i].split(":")
            b = best["horde" if f == "6" else "alliance"]
            lines[i] = ":".join([f, f"{b['demand']}", f"{b['supply']}", *rest])
        out.write_text("\n".join(lines))

    export_final("holdout", S6 / "market_holdout_final.dat")
    final = [(f, s) for f in ("horde", "alliance") for s in (1.0, 0.1)]
    with ThreadPoolExecutor(4) as ex:
        held = list(ex.map(lambda a: sim(S6 / "market_holdout_final.dat", a[0], "holdout", a[1],
                                         tag=f"holdout_{a[0]}_s{a[1]:g}"), final))
    rows += [row(r, best_tilt) for r in held]
    export_final("latest", S6 / "auctionsim_market.dat")

    table = pl.DataFrame(rows).sort("window", "faction", "loss")
    table.write_parquet(S6 / "tune.parquet")
    cols = ["faction", "window", "scale", "tilt", "supply", "demand", "loss", "listings", "sold_share"] + \
        [f"liquid_{m}_{s}" for m in ("units", "price", "min_med", "vol") for s in ("bias", "rank")] + \
        ["all_presence_bias", "all_presence_rank"]
    with pl.Config(tbl_rows=-1, tbl_cols=-1, tbl_width_chars=260, float_precision=3):
        report = "\n".join([
            f"best tilt {best_tilt}; horde supply {best['horde']['supply']} demand {best['horde']['demand']}; "
            f"alliance supply {best['alliance']['supply']} demand {best['alliance']['demand']}",
            "bias = median log(sim/real) (presence: sim - real); rank = rank correlation across items",
            "", "holdout (never seen in tuning):", str(table.filter(pl.col("window") == "holdout").select(cols)),
            "", "tune window, 8 best per faction:",
            str(table.filter(pl.col("window") == "tune").sort("loss").group_by("faction", maintain_order=True)
                .head(8).select(cols)),
        ])
    (S6 / "tune_report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
