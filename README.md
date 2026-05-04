# AuctionSim: A Module For Azerothcore

AuctionSim uses scraped price data from the Lordaeron WoTLK realm on Warmane and uses it to populate your own auction house. The module can both buy and sell and does not have the same limitaions the classic AuctionHouseBot module does. It does not have any categories of items that it misses and the prices are not based on the vendor sell price. It uses different price tables for both Horde and Alliance (no neutral) and is heavily customizable in the config so you can mold how your auction house looks to your whims. It is fast and can list over 100,000 items on the auction house in 200ms.

# Installation:

AuctionSim is disabled by default and must be enabled in the config.

A specific character must be selected as a bot for the auction house operations. Do not use a character you intend to play. You must get the accound ID and character ID of the character you want and enter it into the config.

# Usage:

By default the module scans the auction house every hour to list and buy items. You can use the command .scan to run the scan manually and .delete to delete all the bot-created auctions on the auction house.

# Known issues and future plans:

Currently when the bot buys an auction from a player the player will receive a chat message that their auction expired. The auction was successful but the chat message is incorrect. Mail will arrive as expected.

The config options will be expanded soon to allow for further customization of the shape of your auction house.