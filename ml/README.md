# ml/ — learning the auction house from real scans

Groundwork for bots that compete on the AH and produce a market with the same shape as Warmane - Lordaeron's.
Nothing here is built into the module, and nothing here writes outside `ml/out/`. Inputs are read only:
`../data/scans` (dl-data.sh) and `../nerfed/raw` (nerfed/scrape.py).

```
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
./run.sh            # all stages; ./run.sh 3 4 5 for some
```

`polars[rtcompat]` because moloch.cc's Xeons lack AVX2. `ML_WORKERS` caps worker processes (default 8); everything runs
under `nice` since the box also hosts the worldserver. Stage 0 reads AzerothCore's
`data/sql/base/db_world/item_template.sql`: set `AC_ROOT` when `ml/` is not inside an AzerothCore tree's `modules/`.

## Stages

| Stage | Script | Reads | Writes (`out/`) |
|---|---|---|---|
| 0 | `s0_item_template.py` | AC item_template.sql | `item_template.parquet` (vendor prices, stack sizes) |
| 1a | `s1_extract_scans.py` | scans | `auctions/<faction>/<scan_time>.parquet`, `snapshots.parquet` |
| 1b | `s1_extract_nerfed.py` | nerfed/raw | `nerfed_history.parquet`, `nerfed_items.parquet` |
| 2 | `s2_transitions.py` | 1a | `market/<faction>/<t>.parquet`, `transitions/<faction>/<t0>.parquet` |
| 3 | `s3_sellers.py` | 2 | `s3/`: posts, seller profiles and types, price/stack model |
| 4 | `s4_demand.py` | 2, 3 | `s4/`: sell-through hazard by class x price, check vs short gaps |
| 5 | `s5_targets.py`, `s5_sim.py`, `s5_tune.py` | 0, 1b, 3, 4 | `s5/`: targets (train/tune/holdout windows), simulator runs, demand-scale tuning |

1a checks every snapshot against Auctioneer's own `scanCount`. Scans Auctioneer marked incomplete are kept but
flagged `ok=false`, and later stages skip them: a partial snapshot would make unscanned auctions look gone.

Prices are measured against `common.add_reference`, the rolling 30-day median of the prices an item was *posted* at.
Built from listings instead, the reference was set by walls of identical listings that sit for weeks and rarely sell,
and put a false dip in the demand curve exactly at the reference.

The simulator runs every item a faction posted in the 60 days before it starts. Bots are those items' real sellers,
each posting at its own rates, with prices and stacks from the stage-3 models and errors drawn per seller type,
above the vendor price. Buyers arrive per item at stage 4's sales rate times a demand scale, and buy the cheapest
listing under a reservation price drawn from the class's demand curve. Deposits follow
`AuctionHouseMgr::GetAuctionDeposit`. The demand scale is tuned on the 90 days before the holdout, then scored once
on the holdout (the last 90 days).

## v1 limits

- **No auction ids.** Auctions are matched across snapshots by (seller, item, suffix, enchant, count, minbid,
  buyout). A seller reposting an identical listing looks like one that persisted.
- **Daily snapshots.** Anything posted and gone within a day is missed. The buyer model separates sales from
  expiries statistically: listings priced at 2x reference or more set the expiry baseline. The Sept 2025 scans,
  a few hours apart, are the check. The 6-hourly scans dl-data.sh now keeps will replace this once a few weeks
  have accumulated.
- **Seller types** are k-means clusters of each seller's habits. The price and stack models are gradient boosting
  plus sampled residuals, not a learned policy yet; bots don't react to their own sales or gold.
- **Sale levels**: daily snapshots put sales 2.5-3x below the Sept 2025 short gaps, hence the tuned demand scale.
- **The simulator** has no crafting links between items, and buyers do not substitute between items.
