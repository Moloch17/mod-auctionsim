#pragma once
#include <cstddef>
#include <ctime>
#include "Define.h"

// Pure pricing/selection math for AH listing and buying decisions. No DB or
// AuctionHouseMgr dependency, so this can be exercised with plain values.
namespace AuctionPricing
{
    // Fixed scan cadence -- previously user-configurable via AuctionSim.UpdateInterval.
    constexpr uint32 kScanIntervalSeconds = 3600;

    int CalculateTargetListingCount(float mask, size_t poolSize);
    int CalculateItemsToList(int targetCount, int existingCount);
    uint32 RollQuantity(uint32 maxStackSize);
    bool IsListablePrice(uint32 meanPrice);
    uint32 RollBuyoutPrice(uint32 minPrice, uint32 meanPrice, uint32 maxPrice, uint32 quantity);
    uint32 RollAuctionDuration();

    // A scan pass's randomized willingness to buy above the mean price, mimicking
    // demand variance between real players. Roll once per scan pass.
    struct BuyTolerance
    {
        float boundaryPercent;  // where the near tier gives way to the far tier, in [0.5, 0.7]
    };

    BuyTolerance RollBuyTolerance();

    // How many more scans (at the fixed interval) will see this auction before it
    // expires. Always >= 1.
    uint32 CalculateRemainingScans(time_t remainingSeconds);

    // True if a purchase should be made now. Always buys at/under meanPrice; never
    // buys above maxPrice; otherwise probabilistic based on where pricePerItem falls
    // between meanPrice and maxPrice against this scan's tolerance boundary. The 50%/10%
    // chances are cumulative over the auction's full remaining lifetime, so this is
    // amortized per scan using remainingScans.
    bool ShouldBuyAtPrice(
        uint32 pricePerItem,
        uint32 meanPrice,
        uint32 maxPrice,
        BuyTolerance const& tolerance,
        uint32 remainingScans);

    // Rolls when (as an absolute time) a queued purchase should execute, capped at 45
    // minutes out so it always fires before the next scan reconsiders the auction.
    time_t RollBuyTime(time_t expireTime, time_t now);

    // True if the item is listable under the configured level caps. A cap of 0 means
    // that particular check is disabled.
    bool IsWithinLevelCap(uint32 itemRequiredLevel, uint32 itemLevel, uint32 maxRequiredLevel, uint32 maxItemLevel);
}
