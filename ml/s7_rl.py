#!/usr/bin/env python3
"""Stage 7b: reinforcement learning on top of imitation -- per-type price offsets.

Each seller type t gets one learned log-price offset d_t added to every price
it draws from POLICY (the imitation policy). The types learn at once, each
maximising its own gold per day in the simulated market (proceeds less
deposits), minus a penalty for leaving the imitation policy:

    F_t = gold_t / |gold_t at d = 0|  -  LAMBDA * d_t^2 / (2 * SIGMA^2)

with evolution strategies: antithetic perturbations of the whole offset vector
under common random numbers, each type's gradient taken from its own
fitness (independent learners in one market). Runs on the tune window, Horde
and Alliance separately, at Market.Scale 1.

The offsets are then exported as of the holdout and accepted only if the
holdout loss and the realism AUC do not get worse beyond TOLERANCE; otherwise
the shipped offsets stay zero. Output: out/s7/offsets.parquet (accepted or
zero), out/s7/rl_report.txt.
"""

import json
import os
import subprocess
import sys
from concurrent.futures import ProcessPoolExecutor
from multiprocessing import get_context

import numpy as np
import polars as pl

from common import ML, OUT

S6, S7 = OUT / "s6", OUT / "s7"
POP_PAIRS, GENERATIONS = 4, 6
NOISE, LR = 0.05, 0.05
LAMBDA, SIGMA = 1.0, 0.3
DAYS = 14
TOLERANCE = {"loss": 0.05, "auc": 0.02}


def fitness(args):
    market, faction, offsets, seed = args
    from s6_market_sim import run
    r = run(market, faction, "tune", 1.0, 100, DAYS, 0, None, None, seed, "rl", offsets=offsets, write=False)
    return {int(k): v / DAYS for k, v in r["type_gold"].items()}


def types_of(market):
    types = set()
    with open(market) as f:
        f.readline()
        while header := f.readline().split():
            for _ in range(int(header[1])):
                line = f.readline()
                if header[0] == "BASKET":
                    types.add(int(line.split(":")[3]))
    return sorted(types)


def train(pool, market, faction, types, log):
    rng = np.random.default_rng(1)
    d = np.zeros(len(types))
    base = pool.submit(fitness, (market, faction, {}, 0)).result()
    scale = np.array([max(abs(base.get(t, 0.0)), 1.0) for t in types])
    for g in range(GENERATIONS):
        eps = rng.normal(size=(POP_PAIRS, len(types)))
        jobs = []
        for k in range(POP_PAIRS):
            for sign in (1, -1):
                o = d + sign * NOISE * eps[k]
                jobs.append((market, faction, dict(zip(types, o.tolist())), 100 + g * POP_PAIRS + k))
        res = list(pool.map(fitness, jobs))
        grad = np.zeros(len(types))
        for k in range(POP_PAIRS):
            for j, t in enumerate(types):
                fp = res[2 * k].get(t, 0.0) / scale[j] - LAMBDA * (d[j] + NOISE * eps[k, j]) ** 2 / (2 * SIGMA ** 2)
                fm = res[2 * k + 1].get(t, 0.0) / scale[j] - LAMBDA * (d[j] - NOISE * eps[k, j]) ** 2 / (2 * SIGMA ** 2)
                grad[j] += (fp - fm) * eps[k, j] / (2 * NOISE * POP_PAIRS)
        d += LR * grad
        log.append(f"{faction} gen {g}: offsets " + ", ".join(f"{t}:{x:+.3f}" for t, x in zip(types, d)))
        print(log[-1], flush=True)
    after = pool.submit(fitness, (market, faction, dict(zip(types, d.tolist())), 0)).result()
    return d, base, after


def py(*args):
    env = dict(os.environ, OMP_NUM_THREADS="2", POLARS_MAX_THREADS="2")
    return subprocess.run([sys.executable, *args], cwd=ML, env=env, check=True, capture_output=True, text=True).stdout


def holdout_check(market, faction, tag):
    py("s6_market_sim.py", "--market", str(market), "--faction", faction, "--window", "holdout", "--scale", "1",
       "--tag", tag)
    r = json.loads((S6 / f"sim_{tag}.json").read_text())
    liq, all_ = r["scores"]["liquid"], r["scores"]["all"]
    loss = sum(abs(liq[m]["median"]) for m in ("units", "price", "min_med", "vol")) + abs(all_["presence"]["median"])
    py("s6_realism.py", str(S6 / f"sim_{tag}_daily.parquet"), "--faction", faction)
    auc = json.loads((S6 / f"realism_sim_{tag}_daily.json").read_text())["auc"]
    return loss, auc


def main():
    S7.mkdir(parents=True, exist_ok=True)
    tune_market = S6 / "market_tune_final.dat"
    meta = (S6 / "tune_report.txt").read_text().split("\n")[0]
    tilt = float(meta.split("best tilt ")[1].split(";")[0])
    py("s6_export.py", "--asof", "tune", "--out", str(tune_market), "--tilt", str(tilt))
    # Same per-faction scales as the holdout/shipping files.
    held = (S6 / "market_holdout_final.dat").read_text().split("\n")
    meta_rows = held[held.index(next(l for l in held if l.startswith("META "))) + 1:][:2]
    lines = tune_market.read_text().split("\n")
    k = lines.index(next(l for l in lines if l.startswith("META ")))
    for i, src in zip(range(k + 1, k + 3), meta_rows):
        f, d, s, *_ = src.split(":")
        lines[i] = ":".join([f, d, s, *lines[i].split(":")[3:]])
    tune_market.write_text("\n".join(lines))

    types = types_of(tune_market)
    log, rows = [], []
    with ProcessPoolExecutor(int(os.environ.get("ML_WORKERS", "8")), mp_context=get_context("spawn")) as pool:
        for faction, fid in (("horde", 6), ("alliance", 2)):
            d, base, after = train(pool, str(tune_market), faction, types, log)
            for t, x in zip(types, d):
                rows.append({"faction": fid, "type": t, "offset": float(x), "gold_day_before": base.get(t, 0.0),
                             "gold_day_after": after.get(t, 0.0)})
    learned = pl.DataFrame(rows)
    offsets = (learned.select("faction", "type", "offset").join(pl.DataFrame({"mb": list(range(-1, 10))}), how="cross")
               .with_columns(**{"class": pl.lit(-1)}))
    offsets.write_parquet(S7 / "offsets_learned.parquet")

    # Accept only if the holdout does not get worse.
    rl_market = S6 / "market_holdout_rl.dat"
    py("s6_export.py", "--asof", "holdout", "--out", str(rl_market), "--tilt", str(tilt),
       "--offsets", str(S7 / "offsets_learned.parquet"))
    lines = rl_market.read_text().split("\n")
    k = lines.index(next(l for l in lines if l.startswith("META ")))
    for i, src in zip(range(k + 1, k + 3), meta_rows):
        f, d, s, *_ = src.split(":")
        lines[i] = ":".join([f, d, s, *lines[i].split(":")[3:]])
    rl_market.write_text("\n".join(lines))
    verdict = {}
    for faction in ("horde", "alliance"):
        before = holdout_check(S6 / "market_holdout_final.dat", faction, f"rlcheck_base_{faction}")
        after = holdout_check(rl_market, faction, f"rlcheck_rl_{faction}")
        verdict[faction] = {"loss_before": before[0], "loss_after": after[0], "auc_before": before[1],
                            "auc_after": after[1],
                            "accept": after[0] <= before[0] + TOLERANCE["loss"] and after[1] <= before[1] + TOLERANCE["auc"]}
    accept = all(v["accept"] for v in verdict.values())
    (offsets if accept else offsets.with_columns(offset=pl.lit(0.0))).write_parquet(S7 / "offsets.parquet")
    with pl.Config(tbl_rows=-1, tbl_cols=-1, float_precision=3):
        report = "\n".join(log + ["", str(learned), "", json.dumps(verdict, indent=1),
                                  f"offsets {'ACCEPTED' if accept else 'REJECTED -- shipping zero offsets'}"])
    (S7 / "rl_report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
