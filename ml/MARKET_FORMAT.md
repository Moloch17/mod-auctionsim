# auctionsim_market.dat — the Market mode contract

`ml/s6_export.py` writes it; the module's Market mode reads it; `ml/s6_market_sim.py` is the reference implementation
the C++ must match. Everything the module needs is a lookup table: no models, no Python at runtime. Both factions are
in one file, like `auctionsim.dat`. All prices are per-unit copper.

## Layout

Plain text, `:`-separated fields, one record per line.

```
AUCTIONSIM_MARKET <schemaVersion>
<SECTION> <rowCount>
<rows...>
<SECTION> <rowCount>
...
```

Sections can come in any order; each appears once. Unknown sections are skipped (count tells how many lines). The
module refuses a file whose `schemaVersion` differs from the one it was built for.

## Constants (fixed by schemaVersion 1)

Ratio bins (price / reference) used by `CURVE`, 10 edges, 9 bins:

```
0.25, 0.5, 0.7, 0.85, 1.0, 1.15, 1.4, 2.0, 3.0, 5.0
```

Policy context bins, used by `POLICY`:

- `ub` units-up bin: `min(floor(log1p(units of the item up on the house)), 7)` → 0..7
- `sb` sellers bin: distinct owners with the item up: 0 → 0, 1 → 1, 2 → 2, 3-4 → 3, 5-8 → 4, 9+ → 5
- `mb` cheapest bin: `-1` when nothing of the item is up with a buyout; otherwise `x = ln(cheapest / ref)` binned on
  edges `-1.0, -0.5, -0.25, -0.1, 0, 0.1, 0.25, 0.5, 1.0` → 0..9 (below -1.0 → 0, ≥ 1.0 → 9)

Quantile sampling (`POLICY`, `STACK`): 7 stored quantiles at p = 0.02, 0.10, 0.25, 0.50, 0.75, 0.90, 0.98. Draw
`u ~ U(0,1)`; for u between two stored p, interpolate linearly; below 0.02 or above 0.98 use the end value.

## Sections

### META — 1 row per faction
`faction:demandScale:supplyScale:refListings:refSellers`
- `faction` — AuctionHouseId (2 Alliance, 6 Horde), as everywhere below
- `demandScale`, `supplyScale` — tuned multipliers on buyer and posting rates (floats)
- `refListings` — Lordaeron's typical auction count for this faction, for display/logging
- `refSellers` — sellers the bots stand for

### ITEM — 1 row per item the faction's bots trade
`faction:item:class:ref:conv:maxc:vendor:buyersH`
- `class` — item class code (index into `CURVE`/`POLICY`/`STACK`, see `CLASS`)
- `ref` — reference price (rolling 30-day median of Lordaeron posting prices at export)
- `conv` — conventional stack size; `maxc` — largest stack posted (≤ the item's max stack)
- `vendor` — `item_template.SellPrice` (0 = none): bots never post below it
- `buyersH` — buyer arrivals per hour on Lordaeron (before `demandScale` and the realm's `Market.Scale`)

### CLASS — item class names, for logs
`code:name`

### CURVE — buyer reservation prices, 1 row per (faction, class); class `-1` is the fallback
`faction:class:w0:w1:...:w8`
`w[b]` = share of buyers willing to pay at least bin b's lower edge × ref, non-increasing, `w0 = 1`.
Draw a reservation: bin b with probability `w[b] - w[b+1]` (`w[9] = 0`), then a uniform ratio inside that bin's edges;
reservation price = ratio × `ref`.

### WEEKDAY — buyer rate multipliers, 1 row per (faction, class); class `-1` is the fallback
`faction:class:m0:...:m6` — Monday = 0, server time. Mean 1.

### POLICY — posting price, log(ratio) quantiles
`faction:type:class:ub:sb:mb:q02:q10:q25:q50:q75:q90:q98:offset`
- When `mb ≥ 0`: price = cheapest × exp(draw + offset). When `mb = -1`: price = ref × exp(draw + offset).
- `type` = the posting bot's seller type; `offset` = learned (RL) adjustment, 0 if none.
- Lookup with fallback, first hit wins: `(type, class, ub, sb, mb)`, `(type, class, -1, -1, mb)`,
  `(type, -1, -1, -1, mb)`, `(-1, -1, -1, -1, mb)`. The last level exists for every `mb`.

### STACK — stack size, log(count / conv) quantiles
`faction:type:class:q02:q10:q25:q50:q75:q90:q98`
Lookup `(type, class)`, then `(type, -1)`, then `(-1, -1)`. count = clamp(round(conv × exp(draw)), 1, maxc).

### CRAFT — crafted items' reagents
`faction:item:reagent:qty:margin`
A crafted item's posting price is also never below `margin` × Σ(qty × reagent price), reagent price = cheapest of
that reagent up, else its `ref`. `margin` repeats on each of the item's rows.

### BOT — the named sellers, 1 row per bot
`faction:bot:name:type`
`bot` is 0-based per faction, in the order a realm should use them; a realm configured for N bots takes the first N
names that are free on it. Names are generated (letters only, 2-12 chars, WoW rules), never a scanned player's name.

### BASKET — what each bot posts
`faction:bot:item:rateH:tl4:batch`
- `rateH` — posting events per hour on Lordaeron (before `supplyScale` and `Market.Scale`), for a 100-bot set
- `tl4` — share of its posts that are long (48 h / 24 h, equal odds); the rest are 12 h
- `batch` — mean listings per posting event; listings per event = 1 + Poisson(batch - 1), all at the same price/stack

## Runtime (what both implementations do)

Every step of `dt` hours (module: its 30-minute timer, dt = 0.5), per faction house, with `S = Market.Scale ×
(100 / bots in use)` applied to posting rates and `Market.Scale` to buyer rates:

1. **Posts.** For each basket row, events ~ Poisson(rateH × supplyScale × S × dt). For each event: count listings,
   draw stack from `STACK`, price from `POLICY` using the house's current state for the item (all owners, players
   included), floor at `vendor` and the `CRAFT` floor, duration from `tl4`. Deposit as the core charges it.
2. **Buyers.** For each item, arrivals ~ Poisson(buyersH × demandScale × Market.Scale × weekday × dt). Each draws a
   reservation and buys the cheapest listing with buyout ≤ reservation, whole, whoever owns it (players' listings
   included: that is how players sell to the market); none → it leaves.
3. Expiry is the core's.
