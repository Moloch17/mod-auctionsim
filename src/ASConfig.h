#pragma once
#include <cstddef>
#include <vector>
#include "AuctionHouseMgr.h"
#include "ItemTemplate.h"
#include "ScannedItem.h"

class ASConfig
{
public:
    // Sized to allow indexing directly by an AuctionHouseId's raw numeric value
    // (Alliance=2, Horde=6) without remapping to a dense range; must be at least
    // Horde+1=7. AuctionHouseId::Neutral(7) is never used as an index here --
    // auctionsim.dat only ever contains faction values 2 and 6.
    static constexpr size_t kAuctionHouseIndexBound = 7;

    ASConfig(std::string _filepath, bool& _isEnabled);

    // Level caps for newly-listed items; 0 disables the respective check. Read once
    // from AuctionSim.MaxRequiredLevel / AuctionSim.MaxItemLevel at construction.
    uint32 maxRequiredLevel = 0;
    uint32 maxItemLevel = 0;

    // Raw pointers into ScanData. Valid only because ScanData.reserve() is called
    // before any pointer is taken and ScanData is never resized after construction --
    // do not push_back/emplace_back into ScanData once this table is built.
    std::vector<ScannedItem*> ItemSelectionTable[kAuctionHouseIndexBound][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];
    float ItemSelectionMask[MAX_ITEM_CLASS][MAX_ITEM_QUALITY];
    std::vector<ScannedItem> ScanData;

    std::vector<ScannedItem*> const& ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const;

    // Linear search within one (houseId, itemClass, quality) bucket for a specific item's price data.
    ScannedItem const* FindScannedItem(AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const;

private:
    void UnpackQualityString(std::string_view qualityString, int itemClass);
};
