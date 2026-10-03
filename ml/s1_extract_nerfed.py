#!/usr/bin/env python3
"""Stage 1b: nerfed/raw item pages -> per-item price/volume history.

Output: ml/out/nerfed_history.parquet with columns faction (2/6), item, time
(unix s), quantity (units on the AH), bid_mean, bid_median, buyout_median,
buyout_min, cost_price (per-unit copper; cost_price null where the site has
none), plus ml/out/nerfed_items.parquet (item, name, faction, points).

Reads the first pass's pages (raw/<faction>/items/) and every refresh pass
(raw/<faction>/refresh-*/items/), keeping each (faction, item, time) once:
the site thins old points, so an earlier pass may hold points a later one
dropped. Rerun after the scrape or a refresh finishes; it rebuilds the table.

Buyout 0 means only bid-only auctions were up at that snapshot; it is stored
as null, like any other missing price.
"""

import gzip
import json
import re

import polars as pl

from common import ALLIANCE, HORDE, NERFED, OUT, pool

SERIES = ["quantity", "bid_mean", "bid_median", "buyout_median", "buyout_min", "cost_price"]
ALL_DATA = re.compile(rb"var all_data = (\{.*?\});\n", re.S)
TITLE = re.compile(rb"<title>(.*?) Price Analysis on", re.S)
FACTIONS = {"horde": HORDE, "alliance": ALLIANCE}


def parse(args):
    faction, path = args
    html = gzip.decompress(path.read_bytes())
    m = ALL_DATA.search(html)
    if not m:
        return None
    item = int(path.name.split(".")[0])
    data = json.loads(m.group(1))
    points = {}
    for key in SERIES:
        for t, v in data.get(key, {}).get("data", []):
            if v is None:
                continue
            # Prices are gold floats on the site.
            v = int(v) if key == "quantity" else round(v * 10000)
            points.setdefault(t, {})[key] = v if key == "quantity" or v > 0 else None
    title = TITLE.search(html)
    name = title.group(1).decode("utf-8", "replace").strip() if title else None
    return faction, item, name, [(t, *(p.get(k) for k in SERIES)) for t, p in points.items()]


def main():
    jobs = [(FACTIONS[f.name], p) for f in NERFED.iterdir() if f.name in FACTIONS
            for p in sorted(f.glob("items/*.html.gz")) + sorted(f.glob("refresh-*/items/*.html.gz"))]
    print(f"{len(jobs)} pages")
    rows, names = [], {}
    with pool() as workers:
        for res in workers.imap_unordered(parse, jobs, chunksize=64):
            if res is None:
                continue
            faction, item, name, points = res
            names[(faction, item)] = name
            rows.extend((faction, item, *p) for p in points)
    schema = {"faction": pl.Int8, "item": pl.Int32, "time": pl.Int64, **{k: pl.Int64 for k in SERIES}}
    hist = (pl.DataFrame(rows, schema=schema, orient="row")
            .unique(["faction", "item", "time"], keep="first")
            .sort("faction", "item", "time"))
    hist.write_parquet(OUT / "nerfed_history.parquet")
    items = (hist.group_by("faction", "item").agg(pl.len().alias("points"), pl.col("time").min().alias("first"),
                                                  pl.col("time").max().alias("last"))
             .with_columns(pl.struct("faction", "item").map_elements(
                 lambda s: names.get((s["faction"], s["item"])), return_dtype=pl.String).alias("name")))
    items.sort("faction", "item").write_parquet(OUT / "nerfed_items.parquet")
    print(hist.group_by("faction").agg(pl.col("item").n_unique().alias("items"), pl.len().alias("points"))
          .sort("faction"))


if __name__ == "__main__":
    main()
