#pragma once
#include <cstdint>
#include <unordered_map>
#include <vector>
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
    // already on the house; itemAuctionCount[itemTemplateId] = number of auctions of
    // that specific item already on the house. Both tabulated by the caller over
    // every auction (bot-owned and bid-only included).
    void ListNewAuctions(
        AuctionHouseId houseId,
        int const (&existingCounts)[MAX_ITEM_CLASS][MAX_ITEM_QUALITY],
        std::unordered_map<uint32, int> const& itemAuctionCount);

    // Lists one scanned item immediately, with its own transaction. Used by the
    // ".auctionsim test" command to exercise the listing pipeline end-to-end.
    // Returns the created auction, or nullptr if the item's template couldn't be found.
    AuctionEntry* ListTestItem(ScannedItem const& scan, AuctionHouseId houseId);

private:
    AuctionEntry* ListOneItem(
        ScannedItem const& scan, AuctionHouseId houseId, SQLTransaction<CharacterDatabaseConnection>& trans);

    Bot& _bot;
    ASConfig const& _config;

    // Per-item selection weights, reused across every (class, quality) pass of one
    // ListNewAuctions call so the fill loop doesn't heap-allocate per category.
    // ListNewAuctions is not re-entrant (single world thread), so one buffer is safe.
    std::vector<uint32> _weightBuffer;
};
