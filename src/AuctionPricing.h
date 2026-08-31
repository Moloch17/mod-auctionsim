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

    // Rolls the stack size for a new listing from the item's observed stack-size
    // distribution: most listings use `typical` (the size the market conventionally
    // posts this item in), a minority draw from the outlier-trimmed observed range
    // [spreadLow, spreadHigh]. Everything is clamped to [1, itemMaxStack]; an item
    // that can't stack past 1 always returns 1.
    uint32 RollStackSize(uint32 typical, uint32 spreadLow, uint32 spreadHigh, uint32 itemMaxStack);

    bool IsListablePrice(uint32 marketPrice);

    // Rolls a full-stack buyout by drawing a per-unit price from a split-normal
    // curve across [low, high] that peaks at marketPrice, then multiplying by
    // quantity. low/high are the item's outlier-trimmed range. When sampleCount is
    // too small (or the trimmed range has no width) there is no real distribution
    // to sample, so a small synthetic band is placed around marketPrice instead --
    // enough that repeat listings of the same item aren't priced identically.
    uint32 RollBuyoutPrice(uint32 low, uint32 marketPrice, uint32 high, uint32 quantity, uint32 sampleCount);
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

    // True if a purchase should be made now. Always buys at/under marketPrice (the
    // robust typical price); never buys above ceilingPrice (the 75th percentile);
    // otherwise probabilistic based on where pricePerItem falls between the two
    // against this scan's tolerance boundary. The 50%/10% chances are cumulative
    // over the auction's full remaining lifetime, so this is amortized per scan
    // using remainingScans.
    bool ShouldBuyAtPrice(
        uint32 pricePerItem,
        uint32 marketPrice,
        uint32 ceilingPrice,
        BuyTolerance const& tolerance,
        uint32 remainingScans);

    // Rolls when (as an absolute time) a queued purchase should execute, capped at 45
    // minutes out so it always fires before the next scan reconsiders the auction.
    time_t RollBuyTime(time_t expireTime, time_t now);

    // True if the item is listable under the configured level caps. A cap of 0 means
    // that particular check is disabled.
    bool IsWithinLevelCap(uint32 itemRequiredLevel, uint32 itemLevel, uint32 maxRequiredLevel, uint32 maxItemLevel);
}
