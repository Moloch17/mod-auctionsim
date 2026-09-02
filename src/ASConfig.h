#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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

    // A category-depth header row is faction:class:quality:snapshotCount followed
    // by one 12-value StatBlock -- 16 fields.
    static constexpr size_t kCategoryRowFields = ScannedItem::kIdentityFields + ScannedItem::kStatsPerBlock;

    ASConfig(std::string const& filepath, bool& outLoaded);

    // Level caps for newly-listed items; 0 disables the respective check. Read once
    // from AuctionSim.MaxRequiredLevel / AuctionSim.MaxItemLevel at construction.
    uint32 maxRequiredLevel = 0;
    uint32 maxItemLevel = 0;

    // Every item id stocked by at least one vendor (from npc_vendor). The buy-side
    // vendor-buy-price guard only applies to items in this set: an
    // ItemTemplate::BuyPrice left on an item that no vendor actually sells is stale
    // DB data and must not block an otherwise-good purchase.
    std::unordered_set<uint32> vendorSoldItems;
    bool IsVendorSold(uint32 itemId) const { return vendorSoldItems.count(itemId) > 0; }

    // ScannedItem storage. A std::deque, not a vector: the ScannedItem* kept in
    // ItemSelectionTable / ItemIndex must stay valid as rows are appended, and a
    // deque never relocates existing elements on growth (a vector would).
    std::deque<ScannedItem> ScanData;

    // [house][class][quality] -> the pool the listing service draws from.
    std::vector<ScannedItem*> ItemSelectionTable[kAuctionHouseIndexBound][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    // (house, class, quality, itemID) -> that item's row, so FindScannedItem is
    // O(1) during a scan instead of a linear bucket walk. First row wins on the
    // rare duplicate key (suffix is not part of the key, matching the old search).
    std::unordered_map<uint64_t, ScannedItem const*> ItemIndex;

    // Per-(itemClass, quality) listing multiplier vs. the real market, from the
    // AuctionSim.<Class>Percent config lines. Plain decimals (1, 1.5, 0.25, 0).
    float ItemSelectionMask[MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    // Observed per-(faction, class, quality) auction-count distribution, from the
    // category header rows of auctionsim.dat. Drives how full the bot keeps each
    // category: it only tops a category up while its live auction count is below
    // q1, and never past a random point in [q1, median].
    struct CategoryDepth
    {
        bool has = false;
        uint32 q1 = 0;
        uint32 median = 0;
        uint32 adjLow = 0;
        uint32 adjHigh = 0;
    };
    CategoryDepth categoryDepth[kAuctionHouseIndexBound][MAX_ITEM_CLASS][MAX_ITEM_QUALITY];

    CategoryDepth const& GetCategoryDepth(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const;

    std::vector<ScannedItem*> const& ItemsFor(AuctionHouseId houseId, uint32 itemClass, uint32 quality) const;

    // O(1) lookup of a specific item's row within one (houseId, itemClass, quality)
    // bucket. Returns nullptr if the coordinate is out of range or the item is not
    // in that bucket.
    ScannedItem const* FindScannedItem(AuctionHouseId houseId, uint32 itemClass, uint32 quality, uint32 itemID) const;

    // One (itemClass, quality) mask cell, addressed the same way the addon bridge's wire
    // protocol addresses it: percentConfigKey matches the config key suffix (e.g.
    // "ConsumablePercent"), qualityLabel matches the quality token (e.g. "GREY").
    struct MaskKeyEntry
    {
        std::string_view percentConfigKey;
        std::string_view qualityLabel;
        uint32 itemClass;
        uint32 quality;
    };

    // Resolves a wire key formatted "<percentConfigKey>.<qualityLabel>" (e.g.
    // "ConsumablePercent.GREY") to its ItemSelectionMask indices. False if unrecognized.
    static bool ResolveMaskKey(std::string_view key, uint32& outItemClass, uint32& outQuality);

    // Every (percentConfigKey, qualityLabel) pair -- MAX_ITEM_CLASS * 7 entries -- for
    // enumerating the full mask grid (e.g. to answer a GETCONFIG request).
    static std::vector<MaskKeyEntry> const& AllMaskKeys();

private:
    // Packs a bucket coordinate + itemID into an ItemIndex key.
    static uint64_t IndexKey(size_t house, uint32 itemClass, uint32 quality, uint32 itemID);

    // Constructor helpers, in call order. The line loaders skip an individual bad
    // row (logging it) and carry on.
    static bool ParseHeaderLine(std::string const& line, size_t& outItemRows, size_t& outCategoryRows);
    void LoadCategoryRow(std::string const& line, std::string const& filepath);
    void LoadItemRow(std::string const& line, std::string const& filepath);
    void BuildSelectionTables(std::string const& filepath);
    void LoadMasks();
    void LoadVendorItems();

    void UnpackQualityString(std::string_view qualityString, int itemClass);
};
