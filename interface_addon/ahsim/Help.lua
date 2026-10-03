-- Text for the Bot Manager's Help button. It's plain text between the [[ and ]]
-- markers below -- edit it however you like.

AHSim = AHSim or {}

AHSim.helpText = [[
AuctionSim - First-Time Setup
=============================

AuctionSim runs a bot character that lists and buys items on the Auction House 
so a low-population realm still has a busy AH.

Everything in this window is saved to auctionsim.conf. You do not need to edit
that file by hand.


1. Requirements
---------------
- A character for the bot to use. A dedicated character on its own account is
  best, but any character works - including the one you are logged in on right
  now.
- Two-side auction interaction must be OFF in worldserver.conf:
      AllowTwoSide.Interaction.Auction = 0
  The module refuses to start otherwise.
- You must be a GM to open this window (/auctionsim or /ahsim).


2. Point the module at the bot character
----------------------------------------
- Click "Set Bot Char".
- Type the character's name and click Okay.
- The server looks the character up, writes its character id and account id to
  auctionsim.conf, and - if the module is already enabled - switches the running
  bot to it right away. No restart needed.
- Success, or the reason it failed, shows in the Results box.


3. Enable the module
--------------------
- Tick "Enabled". It saves immediately and starts the bot.
- Optionally tick "Startup Scan" to run a scan automatically on every server
  start.


4. Choose what the bot lists
----------------------------
In the "Listing Multipliers" grid on the right:

- Max Required Level / Max Item Level: the bot skips items above these. 0 means
  no limit.
- The bot works out how full to keep each category and quality from real auction
  scan data. Each grid cell scales that target: 1 matches the real market, 1.5
  keeps it 50% fuller, 0.5 half as full, 0 disables that cell. Enter a plain
  number like 1, 1.5 or 0.25.
- The scan data was taken from a realm with a saturated auction house, so the
  default values are scaled down.
- When editing a cell you must press enter to apply the change.
- Click "Apply" to send your changes, then "Save To File" to write them to
  auctionsim.conf.
- "Refresh" reloads the values from the server and discards unsaved edits.

5. First run
------------
- Click "Scan". The bot lists new auctions (each with a buyout and a lower
  starting bid) and queues actions: items to buy outright, plus bids on players'
  auctions it values (never in an auction's last 30 minutes). Queued actions are spread over
  time, not done all at once - click "Run Queue" to force them all through now.
- Note: Searching the auction house after running a scan while logged in as the
  bot character can take a little while for the auction db to update if there are
  a lot of new auctions.
- After that the module scans on its own on a timer.


6. Market mode (optional)
-------------------------
"Market Mode" switches the module from replaying scanned prices to a market
learned from Warmane - Lordaeron (auctionsim_market.dat). Restart the
worldserver after ticking or unticking it.
- Named seller bots post the auctions; their names show as the seller in the
  AH. The module creates their characters itself on first start, on accounts
  AHSIMMKTA01.. (Alliance) and AHSIMMKTH01.. (Horde) that nobody can log into.
  Names already taken on your realm are skipped.
- Buyers buy the cheapest listing they find worth it - players' listings too,
  which is how players sell to the market. The buys still go through the bot
  character from step 2 and the queue.
- Market Bots: sellers per faction (default 100). Applies at restart or with
  ".auctionsim market reload".
- Market Scale: market size as a fraction of Lordaeron's (default 0.1, about
  6000 auctions per faction). Applies from the next step. Your realm's
  population doesn't matter.
- "Scan" runs one market step. ".auctionsim market status" shows the sellers,
  the last step's numbers and what is still waiting to post.
- If auctionsim_market.dat is missing or out of date the module refuses to run
  and tells GMs at login; untick Market Mode to go back to Replay.
- "Market Fill" (or ".auctionsim market fill [alliance|horde]") fills the
  house at once, as if the sellers had been running for the last 48 hours,
  instead of waiting a day or two. Auctions appear at up to 100 per server tick;
  ".auctionsim market status" shows the progress.
- "Market Purge" (or ".auctionsim market purge") deletes the seller accounts,
  characters and auctions the module created. It first shows what it would
  delete, then asks again before doing it. Bidders on a seller auction get their
  gold back by mail. The market stops until a restart; with Market Mode still
  on, the restart creates the sellers again, so untick Market Mode first if you
  want them gone for good.


Button reference
----------------
Scan            List new auctions and queue buys and bids now (Market mode: run
                one market step).
Delete          Remove every bot auction nobody has bid on, market sellers'
                included (bid-on ones are left to expire so the bidder's gold
                isn't stranded).
Show Queue      Show the queue size and when the next and last action are due.
Run Queue       Execute every queued buy and bid right now.
Clean Over Cap  Remove bot auctions now above the level caps (again skipping
                any that have a bid).
Run Tests       Run the module's built-in self-tests; output goes to Results.
Set Bot Char    Choose which character the bot uses (see step 2).
Market Fill     Market mode: fill the house now (see step 6).
Market Purge    Delete the market sellers the module created (asks first).
Help            This window.


Version notices
--------------
After a module update the Results box (and a GM's chat at login) may show:
- "auctionsim.conf is out of date" - your config is missing keys the new module
  version added. A current auctionsim.conf.dist is kept in etc/modules/; copy the
  new keys into your auctionsim.conf. The module keeps running on defaults for the
  missing keys until you do; it never edits auctionsim.conf itself.
- "auctionsim.dat is out of date" - the data file's format changed. Pull the
  latest changes and rebuild the module; the current auctionsim.dat ships with the
  repo and is redeployed on build (you do not need the raw scans or
  data/compile-data). The module has no market data until then.
- "auctionsim_market.dat can't be used" - Market mode is on but its data file is
  missing or from another format version. Rebuild the module to redeploy it, or
  switch back to Replay.
- "addon / module version mismatch" - the AHSim addon and the server module ship
  as a pair; update whichever the message says is older.
Each notice shows once per version, so a fixed problem stops repeating.


Notes
-----
- Mail the bot (and every market seller) would get from its own auctions is
  discarded automatically.
- Everything set here is written to auctionsim.conf, so it survives a restart.
]]
