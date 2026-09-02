#pragma once
#include <cstddef>
#include <ctime>
#include <vector>
#include "Define.h"

// Pure pricing/selection math for AH listing and buying decisions. No DB or
// AuctionHouseMgr dependency, so this can be exercised with plain values.
namespace AuctionPricing
{
    // Fixed scan cadence -- previously user-configurable via AuctionSim.UpdateInterval.
    constexpr uint32 kScanIntervalSeconds = 3600;

    // How full to make a (class, quality) category this scan: a random point in
    // the observed [q1, median] band. Kept toward the lower end on purpose --
    // successive scans are noisy, and overshoot lingers on the AH for hours.
    // Bounds are order-tolerant.
    uint32 RollCategoryTarget(uint32 q1, uint32 median);

    int CalculateItemsToList(int targetCount, int existingCount);

    // Picks an index into `weights` with probability proportional to each entry.
    // Returns weights.size() when every weight is zero (i.e. "nothing to pick").
    size_t WeightedPick(std::vector<uint32> const& weights);

    // As above, but takes a precomputed sum of `weights`. The listing fill loop
    // keeps this total running as it zeroes entries, so it never re-sums the whole
    // pool per pick. `total` must equal the current sum of `weights`.
    size_t WeightedPick(std::vector<uint32> const& weights, uint32 total);

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

    // Buy-side anti-cheese guard: players can acquire vendor-stocked goods cheaply
    // and relist them, so the bot must never pay more per unit than it would cost to
    // buy the same item straight from a vendor (ItemTemplate::BuyPrice, the merchant
    // purchase price). Equal price still buys; vendorBuyPrice == 0 (item has no
    // vendor purchase price -- e.g. a world drop or enchant) disables the check.
    bool IsWithinVendorBuyPrice(uint32 pricePerItem, uint32 vendorBuyPrice);

    // Buy-side quality gate: the bot never buys poor-quality (grey) items --
    // ITEM_QUALITY_POOR == 0 -- since they are vendor trash and only surface on the
    // AH as cheese bait.
    bool IsBuyableQuality(uint32 quality);
}
