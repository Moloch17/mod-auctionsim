#!/usr/bin/env python3
"""Stage 3: seller behaviour -- posting decisions, seller types, v1 policy.

Inputs: out/transitions, out/market, out/auctions (stages 1-2).

Every listing key that is new at t1 (new > 0) is a posting decision, made
against the market at t0. Daily snapshots miss anything posted and gone
within the gap, and an own-repost of an identical key looks like a persisted
auction; both are v1 limits.

Outputs (out/s3/):
  items.parquet     item -> itype, isub, quality, ilevel, ulevel, name
  posts.parquet     one row per (t1, seller, item, count, minbid, buyout) with
                    new count, price ratios and market context:
                      ratio_min  unit buyout / cheapest unit buyout at t0
                      ratio_ref  unit buyout / the item's reference price
                                 (median of its snapshot medians)
                      bid_ratio  minbid / buyout
  sellers.parquet   per named seller: volume, breadth, pricing and stack
                    habits, and `type` (k-means cluster, the v1 type embedding)
  types.parquet     per type x item class: posts per snapshot per seller,
                    stack/duration/bid-only habits (the v1 volume model)
  price_model.pkl   gradient boosting, with residuals for sampling, for
                      price_min  log(ratio_min) when the item has a buyout up:
                                 sellers price against the cheapest listing
                                 (median post 0.99x it), not a long-run level
                      price_ref  log(ratio_ref) when nothing is up (features
                                 without the last, log_min_ref)
                      stack      log(count / the item's stack convention)
  report.txt        cluster summary and held-out scores
"""

import pickle
import sys

import numpy as np
import polars as pl
from sklearn.cluster import KMeans
from sklearn.ensemble import HistGradientBoostingRegressor
from sklearn.preprocessing import StandardScaler

from common import FACTION_NAMES, OUT

S3 = OUT / "s3"
TYPES = 6
HOLDOUT_DAYS = 60
MIN_POSTS = 20  # sellers with fewer posts are left out of the clustering


def items_dim():
    return (pl.scan_parquet(OUT / "auctions" / "*" / "*.parquet")
            .select("item", "itype", "isub", "quality", "ilevel", "ulevel", "name")
            .group_by("item").agg(pl.all().first()).collect())


def posts_for(faction):
    name = FACTION_NAMES[faction]
    tr = pl.scan_parquet(OUT / "transitions" / name / "*.parquet")
    mk = pl.scan_parquet(OUT / "market" / name / "*.parquet")
    ref = mk.group_by("item").agg(ref_unit=pl.col("med_unit").median())
    own = (tr.filter(pl.col("n0") > 0, pl.col("buyout") > 0)
           .group_by("t0", "seller", "item")
           .agg(own_n0=pl.col("n0").sum(), own_min0=(pl.col("buyout") / pl.col("count")).min()))
    posts = (tr.filter(pl.col("new") > 0)
             .select("t0", "t1", "gap_h", "seller", "item", "suffix", "count", "minbid", "buyout", "new",
                     "n1_tl3", "n1_tl4")
             .join(mk.rename({"t": "t0"}), on=["t0", "item"], how="left")
             .join(ref, on="item", how="left")
             .join(own, on=["t0", "seller", "item"], how="left")
             .with_columns(
                 faction=pl.lit(faction, pl.Int8),
                 unit=pl.when(pl.col("buyout") > 0).then(pl.col("buyout") / pl.col("count")),
                 own_n0=pl.col("own_n0").fill_null(0),
                 weekday=pl.from_epoch("t1").dt.weekday(),
             )
             .with_columns(
                 ratio_min=pl.col("unit") / pl.col("min_unit"),
                 ratio_ref=pl.col("unit") / pl.col("ref_unit"),
                 bid_ratio=pl.when(pl.col("buyout") > 0).then(pl.col("minbid") / pl.col("buyout")),
                 undercut=pl.col("unit") < pl.col("min_unit"),
             ))
    return posts.collect()


def seller_features(posts, items):
    p = posts.filter(pl.col("seller") != "").join(items.select("item", "itype"), on="item", how="left")
    snaps = p.group_by("faction").agg(pl.col("t1").n_unique().alias("snaps"))
    f = (p.group_by("faction", "seller").agg(
            posts=pl.col("new").sum(),
            active=pl.col("t1").n_unique(),
            items=pl.col("item").n_unique(),
            classes=pl.col("itype").n_unique(),
            top_class=pl.col("itype").mode().first(),
            med_ratio_min=pl.col("ratio_min").median(),
            med_ratio_ref=pl.col("ratio_ref").median(),
            undercut_share=pl.col("undercut").mean(),
            bid_only_share=(pl.col("buyout") == 0).mean(),
            med_bid_ratio=pl.col("bid_ratio").median(),
            med_count=pl.col("count").median(),
            tl4_share=(pl.col("n1_tl4").sum() / pl.col("new").sum()),
            med_unit=pl.col("unit").median(),
         ).join(snaps, on="faction")
         .with_columns(active_share=pl.col("active") / pl.col("snaps"),
                       posts_per_active=pl.col("posts") / pl.col("active")))
    return f


CLUSTER_COLS = ["posts", "items", "classes", "active_share", "posts_per_active", "med_ratio_min",
                "med_ratio_ref", "undercut_share", "bid_only_share", "med_count", "tl4_share", "med_unit"]
LOG_COLS = {"posts", "items", "classes", "posts_per_active", "med_ratio_min", "med_ratio_ref", "med_count",
            "med_unit"}


def cluster(sellers):
    s = sellers.filter(pl.col("posts") >= MIN_POSTS)
    x = np.column_stack([
        np.log1p(s[c].fill_null(0).to_numpy().clip(0, None)) if c in LOG_COLS else s[c].fill_null(0).to_numpy()
        for c in CLUSTER_COLS])
    x = np.nan_to_num(x)
    scaler = StandardScaler().fit(x)
    km = KMeans(TYPES, n_init=10, random_state=0).fit(scaler.transform(x))
    s = s.with_columns(type=pl.Series(km.labels_, dtype=pl.Int8))
    return sellers.join(s.select("faction", "seller", "type"), on=["faction", "seller"], how="left"), \
        {"scaler": scaler, "kmeans": km, "cols": CLUSTER_COLS, "log": LOG_COLS}


FEATURES = ["type", "itype_code", "quality", "ilevel", "log_ref", "log_units0", "sellers0", "own_n0", "weekday",
            "faction", "log_min_ref"]
CATEGORICAL = [0, 1, 9]


def model_frame(posts, sellers, items):
    itype_codes = {v: i for i, v in enumerate(sorted(items["itype"].drop_nulls().unique()))}
    stack_conv = posts.group_by("faction", "item").agg(stack_conv=pl.col("count").mode().first())
    return (posts.filter(pl.col("buyout") > 0, pl.col("ratio_ref").is_finite())
            .join(sellers.select("faction", "seller", "type"), on=["faction", "seller"], how="left")
            .join(items.select("item", "itype", "quality", "ilevel"), on="item", how="left")
            .join(stack_conv, on=["faction", "item"], how="left")
            .with_columns(
                type=pl.col("type").fill_null(-1),
                itype_code=pl.col("itype").replace_strict(itype_codes, default=-1),
                log_ref=pl.col("ref_unit").log(), log_units0=pl.col("units").fill_null(0).log1p(),
                sellers0=pl.col("sellers").fill_null(0),
                log_min_ref=(pl.col("min_unit") / pl.col("ref_unit")).log(),
                y_price_min=pl.col("ratio_min").log(),
                y_price_ref=pl.col("ratio_ref").log(),
                y_stack=(pl.col("count") / pl.col("stack_conv")).log(),
            )), itype_codes


def fit(df, target, cutoff, features=FEATURES):
    train, test = df.filter(pl.col("t1") < cutoff), df.filter(pl.col("t1") >= cutoff)
    if train.height > 2_000_000:
        train = train.sample(2_000_000, seed=0)
    xtr, ytr = train.select(features).to_numpy(), train[target].to_numpy()
    model = HistGradientBoostingRegressor(max_iter=300, learning_rate=0.1, categorical_features=CATEGORICAL,
                                          random_state=0).fit(xtr, ytr)
    xte, yte = test.select(features).to_numpy(), test[target].to_numpy()
    pred = model.predict(xte)
    resid = ytr - model.predict(xtr)
    base = np.mean(np.abs(yte - np.median(ytr)))
    return model, resid, {"train": train.height, "test": test.height, "mae": float(np.mean(np.abs(yte - pred))),
                          "mae_median_baseline": float(base), "r2": float(model.score(xte, yte))}


def main():
    S3.mkdir(parents=True, exist_ok=True)
    items = items_dim()
    items.write_parquet(S3 / "items.parquet")
    posts = pl.concat([posts_for(f) for f in FACTION_NAMES], how="vertical_relaxed")
    posts.write_parquet(S3 / "posts.parquet")
    print(f"{posts.height} posting rows, {posts['new'].sum()} new auctions", file=sys.stderr)

    sellers, km = cluster(seller_features(posts, items))
    sellers.write_parquet(S3 / "sellers.parquet")

    df, itype_codes = model_frame(posts, sellers, items)
    cutoff = df["t1"].max() - HOLDOUT_DAYS * 86400
    price_min, price_min_resid, price_min_score = fit(df.filter(pl.col("y_price_min").is_finite()), "y_price_min",
                                                      cutoff)
    # log_min_ref is always missing here (nothing up), which HistGradientBoosting's binning rejects.
    price_ref, price_ref_resid, price_ref_score = fit(df.filter(pl.col("min_unit").is_null()), "y_price_ref", cutoff,
                                                      FEATURES[:-1])
    stack, stack_resid, stack_score = fit(df.filter(pl.col("y_stack").is_finite()), "y_stack", cutoff)

    types = (df.group_by("faction", "type", "itype")
             .agg(posts=pl.col("new").sum(), sellers=pl.col("seller").n_unique(),
                  tl4_share=pl.col("n1_tl4").sum() / pl.col("new").sum(),
                  med_bid_ratio=pl.col("bid_ratio").median(), med_ratio_min=pl.col("ratio_min").median(),
                  undercut_share=pl.col("undercut").mean())
             .join(df.group_by("faction").agg(snaps=pl.col("t1").n_unique()), on="faction")
             .with_columns(posts_per_snapshot_per_seller=pl.col("posts") / pl.col("snaps") / pl.col("sellers")))
    types.write_parquet(S3 / "types.parquet")
    with open(S3 / "price_model.pkl", "wb") as f:
        pickle.dump({"features": FEATURES, "itype_codes": itype_codes, "price_min": price_min,
                     "price_min_resid": price_min_resid, "price_ref": price_ref, "price_ref_resid": price_ref_resid,
                     "stack": stack, "stack_resid": stack_resid, "cluster": km}, f)

    summary = (sellers.filter(pl.col("type").is_not_null()).group_by("type").agg(
        pl.len().alias("sellers"), pl.col("posts").sum(), pl.col("posts").median().alias("med_posts"),
        pl.col("items").median().alias("med_items"), pl.col("active_share").median(),
        pl.col("med_ratio_min").median(), pl.col("med_ratio_ref").median(), pl.col("undercut_share").median(),
        pl.col("bid_only_share").median(), pl.col("med_count").median(), pl.col("tl4_share").median(),
        pl.col("top_class").mode().first()).sort("posts", descending=True))
    with pl.Config(tbl_cols=-1, tbl_width_chars=200):
        report = "\n".join([
            f"posting rows {posts.height}, new auctions {posts['new'].sum()}",
            f"named sellers {sellers.height}, clustered (>= {MIN_POSTS} posts) {sellers['type'].is_not_null().sum()}",
            "", "seller types:", str(summary), "",
            f"held out: the last {HOLDOUT_DAYS} days",
            f"price model log(unit/cheapest at t0): {price_min_score}",
            f"price model log(unit/ref), nothing up at t0: {price_ref_score}",
            f"stack model log(count/convention): {stack_score}",
        ])
    (S3 / "report.txt").write_text(report + "\n")
    print(report)


if __name__ == "__main__":
    main()
