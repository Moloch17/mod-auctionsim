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
    constexpr uint32 kScanIntervalSeconds = 1800;  // 30 minutes

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

    // Rolls a per-stack starting bid for a new listing: draws a "fraction of
    // buyout" from a split-normal across [ratioLowBp, ratioHighBp] peaking at
    // ratioTypicalBp (basis points; bp / 10000 = fraction), then returns
    // clamp(round(buyout * ratio), 1, buyout). Thin samples (sampleCount <
    // kMinSamplesForSpread) or a zero-width band get a small synthetic band
    // around the typical ratio, mirroring RollBuyoutPrice. buyout is the full-
    // stack value already rolled by RollBuyoutPrice.
    uint32 RollStartBid(
        uint32 buyout, uint32 ratioLowBp, uint32 ratioTypicalBp, uint32 ratioHighBp, uint32 sampleCount);

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

    // True if the bot should bid on this scan. Hard gate: never bids when
    // nextBidPerUnit >= ceilingPerUnit (the auction's private valuation, capped by
    // the cheapest live buyout), so every bid the bot places leaves the price below
    // what it is worth to the bot. Below that it is a single per-scan roll -- eager
    // on a clear deal, less eager on a slim margin -- NOT a cumulative lifetime
    // chance. An opening bid (nobody has bid yet) only fires on a clear deal, and
    // less often than an outbid: an unbid auction is no contest yet.
    bool ShouldBidAtPrice(uint32 nextBidPerUnit, uint32 ceilingPerUnit, bool opening = false);

    // Rolls the most the bot will pay per unit for one auction: a point between the
    // item's lower quartile and its market price. Rolled once per auction and kept,
    // so each auction faces one bidder with one limit -- some bid wars the bot wins,
    // the rest it walks away from, instead of it chasing every one up to market.
    uint32 RollBidValuation(uint32 lowPerUnit, uint32 marketPerUnit);

    // Turns the game's minimum next bid into the amount a player would type: often
    // rounded up to a clean amount (whole silver, ten silver or whole gold, by
    // size), sometimes a step or two past it. Always >= minimumBid; the caller
    // falls back to minimumBid when the rolled amount breaks a cap.
    uint32 RollBidAmount(uint32 minimumBid);

    // The clean-amount unit RollBidAmount rounds to: 1g from 10g up, 10s from 1g, 1s
    // from 1s, otherwise copper. A rolled amount is below minimumBid + 3 steps.
    uint32 BidRoundingStep(uint32 minimumBid);

    // Replay bidding gate (AuctionSim.Replay.Bidding): whether a scan may consider a
    // bid on an auction at all -- an outbid where a player holds the high bid, or an
    // opening bid on a player's unbid auction -- and whether a queued bid may run.
    bool MayBid(bool biddingEnabled, bool playerHoldsBid, bool openable);

    // No sniping: the bot never bids in an auction's last 30 minutes (the client's
    // "Short" time-left band), so a player it outbids always has time to answer.
    constexpr time_t kNoBidBeforeExpirySeconds = 1800;
    bool IsTooLateToBid(time_t expireTime, time_t now);

    // Rolls when (as an absolute time) a queued purchase should execute, capped at 20
    // minutes out so it always fires before the next scan reconsiders the auction.
    time_t RollBuyTime(time_t expireTime, time_t now);

    // RollBuyTime for a bid: lands before the auction enters the no-bid window.
    time_t RollBidTime(time_t expireTime, time_t now);

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
