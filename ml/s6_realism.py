#!/usr/bin/env python3
"""Stage 6d: can a classifier tell the simulated market from the real one?

Per item and day, from the holdout window: units up (divided by the market
scale for the simulation), cheapest/median, day-to-day change of the median
price and of units, and the median price over the item's reference -- for the
real market from the nerfed history, for the simulation from its daily
snapshots. A gradient-boosting classifier is trained on half of the items and
tested on the other half (items never split across the two). ROC AUC 0.5 means
the two cannot be told apart; 1.0 means always.

  ./s6_realism.py out/s6/sim_holdout_horde_s1_daily.parquet --faction horde [--scale 1]

Prints overall AUC and each feature's own AUC; writes out/s6/realism_<tag>.json.
"""

import argparse
import json
from pathlib import Path

import numpy as np
import polars as pl
from sklearn.ensemble import HistGradientBoostingClassifier
from sklearn.metrics import roc_auc_score

from common import FACTION_NAMES, OUT

FEATURES = ["log_units", "min_med", "d_price", "d_units", "price_ref"]


def features(df):
    return (df.sort("item", "day")
            .with_columns(log_units=pl.col("units").log1p(), min_med=pl.col("min_unit") / pl.col("med_unit"),
                          d_price=(pl.col("med_unit").log() - pl.col("med_unit").log().shift(1)).over("item").abs(),
                          d_units=(pl.col("units").log1p() - pl.col("units").log1p().shift(1)).over("item").abs(),
                          price_ref=(pl.col("med_unit") / pl.col("med_unit").median().over("item")).log())
            .select("item", *FEATURES))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("daily")
    ap.add_argument("--faction", choices=["horde", "alliance"], required=True)
    ap.add_argument("--scale", type=float, default=1.0)
    args = ap.parse_args()
    faction = {v: k for k, v in FACTION_NAMES.items()}[args.faction]

    sim = pl.read_parquet(args.daily).with_columns(pl.col("units") / args.scale)
    start = (pl.read_parquet(OUT / "s5" / "targets.parquet")
             .filter(pl.col("faction") == faction, pl.col("window") == "holdout")["window_start"][0])
    real = (pl.read_parquet(OUT / "nerfed_history.parquet")
            .filter(pl.col("faction") == faction, pl.col("time") >= start, pl.col("item").is_in(sim["item"].unique().implode()))
            .with_columns(day=((pl.col("time") - start) // 86400).cast(pl.Int32))
            .sort("time").group_by("item", "day").agg(pl.all().last())
            .select("item", "day", units=pl.col("quantity").cast(pl.Float64), min_unit=pl.col("buyout_min"),
                    med_unit=pl.col("buyout_median"))
            .drop_nulls())
    data = pl.concat([features(real).with_columns(y=pl.lit(0)),
                      features(sim.select("item", "day", "units", "min_unit", "med_unit")).with_columns(y=pl.lit(1))])
    data = data.drop_nulls().filter(pl.all_horizontal(pl.col(FEATURES).is_finite()))
    items = data["item"].unique().sort().to_numpy()
    rng = np.random.default_rng(0)
    test_items = set(rng.choice(items, len(items) // 2, replace=False).tolist())
    is_test = data["item"].is_in(list(test_items)).to_numpy()
    x, y = data.select(FEATURES).to_numpy(), data["y"].to_numpy()
    clf = HistGradientBoostingClassifier(max_iter=200, random_state=0).fit(x[~is_test], y[~is_test])
    auc = float(roc_auc_score(y[is_test], clf.predict_proba(x[is_test])[:, 1]))
    per = {}
    for k, f in enumerate(FEATURES):
        a = roc_auc_score(y[is_test], x[is_test, k])
        per[f] = float(max(a, 1 - a))
    out = {"daily": args.daily, "faction": args.faction, "rows_real": int((y == 0).sum()), "rows_sim": int((y == 1).sum()),
           "auc": auc, "feature_auc": per}
    (OUT / "s6" / f"realism_{Path(args.daily).stem}.json").write_text(json.dumps(out, indent=2))
    print(json.dumps(out, indent=1))


if __name__ == "__main__":
    main()
