# AuctionSim: A Module For AzerothCore WoTLK 3.3.5a

AuctionSim populates and maintains your realm's auction house using price data scraped from a live WoW 3.3.5a economy (Lordaeron, Warmane). Unlike the classic ah-bot module, it isn't limited to a handful of item categories and doesn't price items off their vendor sell price -- it uses real observed mean/min/max prices, with separate price tables for Alliance and Horde (no neutral AH support). Listing behavior is fully configurable per item class and quality. It's fast: over 100,000 items can be listed in under 100ms.

The bot both **lists** items for sale and **buys** items it finds underpriced, so the auction house behaves like a live economy rather than a static shelf of vendor-bot listings.

## How it works

Every hour (fixed, not configurable), AuctionSim scans both auction houses:

- **Listing**: for each item class/quality bucket, it lists new items up to the percentage targets set in the config, picking a random quantity and a buyout price rolled around the item's known mean price.
- **Buying**: for each auction it doesn't already own, if the price is at or under the item's known mean, it's always queued to buy. If the price is above mean but still under the item's known maximum, it's queued with some probability (randomized each scan, to mimic natural demand variance between real players) rather than always or never. Queued purchases execute within 45 minutes of being queued, so a purchase decision is always acted on well before the next scan reconsiders that auction.
- Optional `MaxRequiredLevel`/`MaxItemLevel` caps stop it from listing gear above your realm's level, for progression servers running below the max level.

## Installation

1. From your AzerothCore `modules` directory:
   ```
   git clone https://github.com/Moloch17/mod-auctionsim.git
   ```
2. Rebuild AzerothCore.
3. In `azerothcore/env/dist/etc/modules`, duplicate `auctionsim.conf.dist` and rename the copy to `auctionsim.conf`. This is the file you'll edit.
4. In `auctionsim.conf`, set `AuctionSim.Enabled = 1` and fill in `AuctionSim.BotAccountID` / `AuctionSim.BotCharacterID` (see below). Everything else is pre-configured and only needs changing if you want to adjust listing behavior.

**Notes:**
- If you've previously used ah-bot or ah-bot-plus, there's no conflict, but the two cannot run at the same time -- disable other auction house bot modules before enabling this one.
- Not intended for use with combined auction house enabled; untested in that configuration.
- AuctionSim is disabled by default.

### Choosing a bot character

A specific character must be designated as the bot for all auction house operations -- **do not use a character you intend to play**. Get that character's account ID and character ID and enter them into `AuctionSim.BotAccountID`/`AuctionSim.BotCharacterID`. Reusing a character previously used for another AH bot module is fine, and preferred.

## Commands

All commands are nested under `.auctionsim` and require GM (Administrator) permissions. Usable from both in-game chat and the server console.

| Command | Description |
|---|---|
| `.auctionsim scan` | Manually triggers an immediate scan of both auction houses (the same operation that runs automatically every hour). Reports elapsed time and how many items were added to the buy queue. |
| `.auctionsim delete` | Deletes every auction currently listed by the bot, on both factions. |
| `.auctionsim showqueue` | Shows how many purchases are currently queued, and how long until the next and last queued purchases will execute. |
| `.auctionsim cleanovercap` | Removes existing bot listings that violate the currently configured `MaxRequiredLevel`/`MaxItemLevel` caps. Lowering a cap in the config doesn't retroactively remove items already listed above it -- run this after tightening a cap to clean those up. |
| `.auctionsim test` | Runs the module's built-in self-test suite (config/data sanity checks, pricing-math bounds, and live end-to-end listing/buying/level-cap checks) and reports pass/fail for each to both chat and the server log. Useful for verifying the module is healthy after install or a config change. |

## Configuration reference

`auctionsim.conf` covers:
- `AuctionSim.Enabled`, `AuctionSim.BotAccountID`, `AuctionSim.BotCharacterID`, `AuctionSim.StartupScan` -- core setup, see Installation above.
- `AuctionSim.MaxRequiredLevel` / `AuctionSim.MaxItemLevel` -- optional level caps for progression realms (`0` disables either check).
- `AuctionSim.<ItemClass>Percent` (17 lines, one per item class) -- the percentage of known items in each quality tier (Grey/White/Green/Blue/Purple/Orange/Yellow) to keep listed. Comments in the file explain the format.
