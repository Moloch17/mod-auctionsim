-- Text for the Help windows, between the [[ and ]] markers below -- edit it however you like.
--
-- It is a small subset of Markdown, drawn by AHSimPanel.lua's help renderer (and copied into README.md):
--   # Title, ## Section, ### Item     headings
--   - item  /    - item (2 spaces)   bullets, nested one level
--   **text**  `text`                 highlighted words, commands and settings
-- A blank line ends a paragraph or list item; lines in between are joined, so wrap them anywhere.

AHSim = AHSim or {}

AHSim.helpText = [[
# AuctionSim - First-Time Setup

AuctionSim runs a bot character that lists and buys items on the auction house, so a low-population realm still has
a busy AH.

Everything in this window is saved to `auctionsim.conf`. You do not need to edit that file by hand.

## 1. Requirements

- A character for the bot to use. A dedicated character on its own account is best, but any character works,
  including the one you are logged in on right now.
- Two-side auction interaction must be off in `worldserver.conf`: `AllowTwoSide.Interaction.Auction = 0`. The module
  refuses to start otherwise.
- You must be a GM to open this window (`/auctionsim` or `/ahsim`).

## 2. Point the module at the bot character

- Click **Set Bot Char**.
- Type the character's name and click **Okay**.
- The server looks the character up and writes its character id and account id to `auctionsim.conf`. If the module
  is already enabled, the running bot switches to it right away. No restart needed.
- Success, or the reason it failed, shows in the **Results** box.

## 3. Enable the module

- Tick **Enabled**. It saves immediately and starts the bot.
- Optionally tick **Startup Scan** to run a scan automatically every time the server starts.

## 4. Choose what the bot lists

These settings are on the right of the **Main** tab.

- **Max Required Level** and **Max Item Level**: the bot skips items above these. 0 means no limit.
- **Listing Multipliers**: the bot works out how full to keep each category and quality from real auction scan data.
  Each cell scales that target: 1 matches the real market, 1.5 keeps it 50% fuller, 0.5 half as full, 0 turns the
  cell off. Enter a plain number such as 1, 1.5 or 0.25, and press Enter to apply each cell.
- The scan data comes from a realm with a very full auction house, so the default values are scaled down.
- Click **Apply** to send your changes, then **Save To File** to write them to `auctionsim.conf`.
- **Refresh** reloads the values from the server and discards unsaved edits.

## 5. First run

- Click **Scan**. The bot lists new auctions, each with a buyout and a lower starting bid, and queues actions: items
  to buy outright, plus bids on players' auctions it values (never in an auction's last 30 minutes).
- Queued actions are spread over time rather than done all at once. Click **Run Queue** to force them all through now.
- After a scan, searching the auction house while logged in as the bot character can take a while to update if many
  auctions were added.
- From then on the module scans by itself every 30 minutes.

## 6. Experimental Features tab

The **EXPERIMENTAL FEATURES** tab at the bottom of the window holds features that work but may still change between
releases:

- **Market Mode**: named seller bots and buyers learned from a real market, instead of the Replay bot.
- Its settings, **Market Bots** and **Market Scale**, and its commands: **Market Status**, **Fill**, **Reload** and
  **Purge**.
- **Replay Bidding**: whether the Replay bot bids. Greyed out while Market Mode is ticked.

Ticking or unticking Market Mode offers to restart the worldserver, since a mode change needs a restart. The tab's own
**Help** button explains every control in detail.

## Buttons on the Main tab

- **Scan**: list new auctions and queue buys and bids now. In Market mode, runs one market step.
- **Delete**: remove every bot auction nobody has bid on, market sellers' included. Auctions with a bid are left to
  expire, so the bidder's gold is never stranded.
- **Show Queue**: show the queue size and when the next and last actions are due.
- **Run Queue**: carry out every queued buy and bid right now.
- **Clean Over Cap**: remove bot auctions now above the level caps, again skipping any that have a bid.
- **Run Tests**: run the module's built-in self-tests. Output goes to the Results box.
- **Set Bot Char**: choose which character the bot uses (see step 2).
- **Help**: this window.

## Version notices

After a module update, the Results box (and a GM's chat at login) may show:

- **auctionsim.conf is out of date**: your config is missing keys the new version added. A current
  `auctionsim.conf.dist` is kept in `etc/modules/`; copy the new keys into your `auctionsim.conf`. Until you do, the
  module runs on defaults for the missing keys. It never edits `auctionsim.conf` itself.
- **auctionsim.dat is out of date**: the data file's format changed. Pull the latest changes and rebuild the module;
  the current `auctionsim.dat` ships with the module and is redeployed on build. The module has no price data until
  then.
- **auctionsim_market.dat can't be used**: Market mode is on but its data file is missing or from another format
  version. Rebuild the module to redeploy it, or switch back to Replay.
- **addon / module version mismatch**: the addon and the server module ship as a pair. Update whichever the message
  says is older.

Each notice shows once per version, so a fixed problem stops repeating.

## Notes

- Auction-house mail to the bot, and to every market seller, is discarded automatically. Other mail to them, from a
  player or a GM, is delivered as usual and returns to its sender when it expires.
- Everything set here is written to `auctionsim.conf`, so it survives a restart.
]]

-- Text for the EXPERIMENTAL FEATURES tab's Help button (same format as above).
AHSim.experimentalHelpText = [[
# AuctionSim - Experimental Features

These features work, but their behaviour may still change between releases.

## Market mode

By default the module runs in Replay mode: one bot lists items from a table of real prices, follows the auction
scans and buys what is cheap. Market mode replaces that with a market learned from Warmane's Lordaeron realm.

- **Named sellers.** Each faction gets its own seller bots, and their names show as the seller in the auction house.
  The module creates their characters itself the first time Market mode starts, on accounts `AHSIMMKTA01`,
  `AHSIMMKTA02` and so on (Alliance) and `AHSIMMKTH01` and so on (Horde), ten characters per account. Nobody can log
  into them. Names already taken on your realm are skipped.
- **What each seller posts.** Each seller has its own list of items, taken from what real sellers of its kind posted
  on Lordaeron, with a posting rate for each item: Trade Goods for some sellers, glyphs or gear for others. Every 30
  minutes the module rolls, item by item, whether the seller posts it this time, so a busy item may go up several
  times a day and a rare one once a week. One item is usually spread over one or two sellers.
- **Prices.** A post is priced against the cheapest listing of the same item already up, players' listings included.
  It is never below the vendor price, and crafted goods are never below what their reagents cost. Crafted gear and
  bags carry the seller's name as their maker once bought. Durations, deposits and expiry are the game's own.
- **Buyers.** Buyers arrive for each item at the rates seen on Lordaeron, with a weekday pattern. Each is willing to
  pay up to some price and buys the cheapest listing at or under it, whoever listed it. That is how players sell to
  the market: list at a fair price and a buyer will come. Purchases go through the bot character set with **Set Bot
  Char** on the Main tab, and are spread over about 20 minutes. Buyers never bid: they buy outright.
- **The market data file.** All of this comes from `auctionsim_market.dat`, which ships with the module and is copied
  next to `auctionsim.conf` when the module is built. If it is missing or from another format version, the module
  refuses to run in Market mode and tells GMs at login.
- **Gold.** The buyer bot is never charged, so gold enters the economy only when it buys a player's listing.
  Everything paid to a seller bot is discarded, so gold players spend on seller listings leaves the economy.

## Settings

### Market Mode (checkbox)

Switches between Replay (unticked) and Market (ticked). The change is saved at once but only takes effect when the
worldserver restarts. Ticking or unticking it asks **"Restart the worldserver now to apply the change?"**

- **Yes** restarts the server the standard way: a 10 second countdown that players see, like `.server restart 10`.
  The server only comes back by itself if whatever runs it restarts the process (a Docker restart policy, a service
  manager or the restarter script). Refused if a shutdown or restart is already pending.
- **No** leaves it for your next restart and says so in chat: "Market mode cannot be initiated until the worldserver
  is restarted." (or "Replay mode ..." when unticking).

### Replay Bidding (checkbox)

Whether the Replay bot bids. Ticked (the default), it outbids players who bid on an auction and sometimes opens the
bidding on a player's auction, as a player would. Unticked, it never bids: it queues no new bids and drops the bids it
had already queued. Buying outright is unaffected. Takes effect at once, from the next scan.

It only matters in Replay mode, because Market mode never bids. While Market Mode is ticked the checkbox is greyed out
and can't be changed; its saved value is kept for when you switch back, and the server refuses to change it.

### Market Bots (box)

How many named sellers each faction gets (default 100), taken from the names in the data file; fewer if names are
taken. The market's size doesn't depend on it, only how many names the posts are spread over. Applies at restart or
with **Market Reload**.

### Market Scale (box)

How big the market is, as a fraction of Lordaeron's: the default 0.1 is about 6,000 auctions per faction, and 1 is
about 60,000. It scales both posting and buying. Your realm's population doesn't matter. Applies from the next market
step.

## Commands

### Market Status

Button, or `.auctionsim market status`. Shows whether the market is running (and why not if it isn't), the scale,
the sellers in use per faction, auctions still waiting to be posted, the buy queue, the last step's numbers per auction
house, and fill progress.

### Market Fill

Button, or `.auctionsim market fill [alliance|horde]`. Fills the auction house at once instead of waiting a day or
two.

- It simulates the last 48 hours of the market, with the auctions already up as competition, and posts what the
  sellers would have up now, each with its remaining time.
- It never adds more than a full house minus what the sellers already have up, so a fill on a full house adds almost
  nothing.
- Auctions appear at up to 100 per server tick. Both factions by default; the chat command takes `alliance` or
  `horde` for one.
- Refused while the market isn't running, its sellers are still being set up, or a fill is already running.

### Market Reload

Button, or `.auctionsim market reload`. Re-reads `auctionsim.conf` (Market Bots, Market Scale, Replay Bidding) and
`auctionsim_market.dat` and restarts the market with them, without restarting the server.

- Only works when the server started in Market mode; switching modes needs a restart.
- The buyer bot is reloaded too, which empties its buy queue.

### Market Purge

Button, or `.auctionsim market purge [confirm]`. Removes everything Market mode created.

- The first click only reports what would be deleted: the seller accounts and characters, their auctions (and how
  many have bids) and their mail. If there is anything, a second popup asks you to confirm.
- Confirmed (`.auctionsim market purge confirm`), it:
  - stops the market until a restart or Market Reload, and drops the queued market purchases;
  - removes every seller auction; anyone who bid on one gets the bid back by mail, as when an auction is cancelled;
  - deletes the seller characters and the `AHSIMMKT` accounts the standard way.
- It refuses entirely, deleting nothing, if a seller account holds a character the module didn't create (levelled,
  played, wrong race or class, online) or has GM access. It never touches the buyer bot.
- With Market Mode still ticked, the next restart creates the sellers again. Untick it first if you want them gone
  for good. Running it again finds nothing and says so.

### Help

This window.
]]
