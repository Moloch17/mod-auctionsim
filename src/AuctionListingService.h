#pragma once
#include "AuctionHouseMgr.h"
#include "DatabaseEnvFwd.h"
#include "ItemTemplate.h"

class ASConfig;
class Bot;
class ScannedItem;

// Orchestrates creating new bot-owned AH listings for one house. Does not
// touch the (separately WIP) buy-side queue/logic on AuctionSim.
class AuctionListingService
{
public:
    AuctionListingService(Bot& bot, ASConfig const& config);

    // existingCounts[itemClass][quality] = number of auctions of that class/quality
    // already on the house, as tabulated by the caller.
    void ListNewAuctions(AuctionHouseId houseId, int const (&existingCounts)[MAX_ITEM_CLASS][MAX_ITEM_QUALITY]);

    // Lists one scanned item immediately, with its own transaction. Used by the
    // ".auctionsim test" command to exercise the listing pipeline end-to-end.
    // Returns the created auction, or nullptr if the item's template couldn't be found.
    AuctionEntry* ListTestItem(ScannedItem const& scan, AuctionHouseId houseId);

private:
    AuctionEntry* ListOneItem(
        ScannedItem const& scan, AuctionHouseId houseId, SQLTransaction<CharacterDatabaseConnection>& trans);

    Bot& _bot;
    ASConfig const& _config;
};
