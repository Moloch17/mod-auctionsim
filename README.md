# AuctionSim: A Module For Azerothcore WoTLK 3.3.5a

AuctionSim uses scraped price data from the Lordaeron WoTLK realm on Warmane and uses it to populate your own auction house. The module can both buy and sell and does not have the same limitaions the classic ah-bot module does. It does not have any categories of items that it misses and the prices are not based on the vendor sell price. It uses different price tables for both Horde and Alliance (no neutral) and is heavily customizable in the config so you can mold how your auction house looks to your whims. It is fast and can list over 100,000 items in under 100ms. 

# Installation:

Navigate to the azerothcore/modules directory and run ```git clone https://github.com/Moloch17/mod-auctionsim.git``` and then rebuild azerothcore. Then navigate to azerothcore/env/dist/etc/modules and duplicate auctionsim.conf.dist and rename it to auctionsim.conf. This is the config file you will edit.

If you have used ah-bot or ah-bot-plus previously, there should be no conflicts with this module. They cannot be run at the same time so be sure to disable other auction house bot modules before enabling this one.

This module is not intended to be used with combined auction house enabled. It is untested.

AuctionSim is disabled by default and must be enabled in the config.

A specific character must be selected as a bot for the auction house operations. Do not use a character you intend to play. You must get the accound ID and character ID of the character you want and enter it into the config. Reusing another character that was used for previous auction house bot modules is fine (preferred, even).

Everything else is pre-configured and won't need to be edited unless you want to.

# Usage:

By default the module scans the auction house at server start and every hour afterward to list and buy items. You can use the command .scan to run the scan manually and .delete to delete all the bot-created auctions on the auction house.

# Known issues and future plans:

Currently when the bot buys an auction from a player the player will receive a chat message that their auction expired. The auction was successful but the chat message is incorrect. Mail will arrive as expected.

The config options will be expanded soon to allow for further customization of the shape of your auction house.