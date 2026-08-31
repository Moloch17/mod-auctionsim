#include "AuctionPricing.h"
#include <algorithm>
#include <cmath>
#include <random>
#include "Random.h"

namespace
{
    constexpr uint32 kLowQtyRollCeiling = 33;   // roll < this -> qty = 1
    constexpr uint32 kHighQtyRollFloor = 66;    // roll > this -> qty = maxStackSize
    constexpr uint32 kMinListablePrice = 3;

    // Std deviation divisor for RollBuyoutPrice's split-normal curve: each side's
    // distance from the market price to its bound is treated as this many standard
    // deviations, so draws land at the true edge only in the rare tail.
    constexpr float kBuyoutSigmaDivisor = 3.0f;

    // Below this many observed listings the trimmed low/high isn't a trustworthy
    // spread (with 1-3 samples it often collapses to a single point). RollBuyoutPrice
    // falls back to a small synthetic band of +/- kThinSampleJitter around the
    // market price so repeat listings of a rarely-seen item still vary a little.
    constexpr uint32 kMinSamplesForSpread = 4;
    constexpr float kThinSampleJitter = 0.08f;
    constexpr uint32 kMinDurationSeconds = 3600;   // 1 hour
    constexpr uint32 kMaxDurationSeconds = 43200;  // 12 hours

    constexpr float kToleranceBoundaryMin = 0.50f;
    constexpr float kToleranceBoundaryMax = 0.70f;
    constexpr float kNearTierBuyChance = 0.50f;  // cumulative, over the auction's full remaining lifetime
    constexpr float kFarTierBuyChance = 0.10f;   // cumulative, over the auction's full remaining lifetime

    // A queued purchase never waits longer than this before executing.
    constexpr time_t kMaxQueueDelaySeconds = 2700;  // 45 minutes

    // Converts a target cumulative chance (over remainingScans opportunities) into the
    // equivalent independent per-scan chance: 1 - (1 - target)^(1/remainingScans).
    float AmortizeOverScans(float targetCumulativeChance, uint32 remainingScans)
    {
        return 1.0f - std::pow(1.0f - targetCumulativeChance, 1.0f / static_cast<float>(remainingScans));
    }
}

namespace AuctionPricing
{
    int CalculateTargetListingCount(float mask, size_t poolSize)
    {
        return static_cast<int>(mask * poolSize);
    }

    int CalculateItemsToList(int targetCount, int existingCount)
    {
        return targetCount - existingCount;
    }

    uint32 RollQuantity(uint32 maxStackSize)
    {
        // Not enough headroom to roll a meaningful middle value.
        if (maxStackSize <= 2)
        {
            return maxStackSize;
        }

        uint32 roll = urand(0, 99);
        if (roll < kLowQtyRollCeiling)
        {
            return 1;
        }
        if (roll > kHighQtyRollFloor)
        {
            return maxStackSize;
        }
        return urand(2, maxStackSize - 1);
    }

    bool IsListablePrice(uint32 marketPrice)
    {
        return marketPrice >= kMinListablePrice;
    }

    uint32 RollBuyoutPrice(uint32 low, uint32 marketPrice, uint32 high, uint32 quantity, uint32 sampleCount)
    {
        if (marketPrice == 0)
        {
            return quantity;  // caller should have filtered via IsListablePrice
        }

        // No trustworthy spread to sample from: put a small symmetric band around
        // the market price so a handful of copies of the same item don't all show
        // the exact same number.
        if (sampleCount < kMinSamplesForSpread || high <= low)
        {
            low = static_cast<uint32>(static_cast<float>(marketPrice) * (1.0f - kThinSampleJitter));
            high = static_cast<uint32>(static_cast<float>(marketPrice) * (1.0f + kThinSampleJitter));
        }

        // Keep the market price inside the band even if the trimmed stats put it
        // slightly outside (rounding, an off-centre adjusted median, etc.).
        low = std::min(low, marketPrice);
        high = std::max(high, marketPrice);

        if (high <= low)
        {
            return quantity * marketPrice;
        }

        // Split-normal (asymmetric bell) across [low, high], peaking at marketPrice.
        // Each side gets its own standard deviation so an off-centre market price
        // still yields a natural curve. Draws outside the band are re-rolled rather
        // than clamped so the shape isn't distorted; with the bounds ~3 sigma out a
        // redraw is rare.
        float marketF = static_cast<float>(marketPrice);
        float sigmaLow = std::max((marketF - static_cast<float>(low)) / kBuyoutSigmaDivisor, 1.0f);
        float sigmaHigh = std::max((static_cast<float>(high) - marketF) / kBuyoutSigmaDivisor, 1.0f);
        std::normal_distribution<float> standardNormal(0.0f, 1.0f);

        float price;
        uint32 attempts = 0;
        do
        {
            float z = standardNormal(RandomEngine::Instance());
            price = marketF + z * (z < 0.0f ? sigmaLow : sigmaHigh);
        } while ((price < low || price > high) && ++attempts < 64);

        price = std::clamp(price, static_cast<float>(low), static_cast<float>(high));

        return quantity * std::max(1u, static_cast<uint32>(price));
    }

    uint32 RollAuctionDuration() { return urand(kMinDurationSeconds, kMaxDurationSeconds); }

    BuyTolerance RollBuyTolerance() { return {frand(kToleranceBoundaryMin, kToleranceBoundaryMax)}; }

    uint32 CalculateRemainingScans(time_t remainingSeconds)
    {
        if (remainingSeconds <= 0)
        {
            return 1;
        }
        return static_cast<uint32>((remainingSeconds + kScanIntervalSeconds - 1) / kScanIntervalSeconds);
    }

    bool ShouldBuyAtPrice(
        uint32 pricePerItem,
        uint32 marketPrice,
        uint32 ceilingPrice,
        BuyTolerance const& tolerance,
        uint32 remainingScans)
    {
        if (pricePerItem <= marketPrice)
        {
            return true;
        }
        if (pricePerItem > ceilingPrice || ceilingPrice <= marketPrice)
        {
            return false;
        }

        float percentPosition =
            static_cast<float>(pricePerItem - marketPrice) / static_cast<float>(ceilingPrice - marketPrice);
        float targetChance = percentPosition <= tolerance.boundaryPercent ? kNearTierBuyChance : kFarTierBuyChance;

        return roll_chance_f(AmortizeOverScans(targetChance, remainingScans) * 100.0f);
    }

    time_t RollBuyTime(time_t expireTime, time_t now)
    {
        time_t window = std::min(expireTime - now, kMaxQueueDelaySeconds);
        if (window <= 0)
        {
            return now;
        }
        return now + irand(0, static_cast<int32>(window - 1));
    }

    bool IsWithinLevelCap(uint32 itemRequiredLevel, uint32 itemLevel, uint32 maxRequiredLevel, uint32 maxItemLevel)
    {
        bool requiredLevelOk = maxRequiredLevel == 0 || itemRequiredLevel <= maxRequiredLevel;
        bool itemLevelOk = maxItemLevel == 0 || itemLevel <= maxItemLevel;
        return requiredLevelOk && itemLevelOk;
    }
}
