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
    // distance from the mean to its bound (min or max) is treated as this many
    // standard deviations, so draws land at the true edge only in the rare tail.
    constexpr float kBuyoutSigmaDivisor = 3.0f;
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

    bool IsListablePrice(uint32 meanPrice)
    {
        return meanPrice >= kMinListablePrice;
    }

    uint32 RollBuyoutPrice(uint32 minPrice, uint32 meanPrice, uint32 maxPrice, uint32 quantity)
    {
        if (maxPrice <= minPrice)
        {
            return quantity * meanPrice;
        }

        // Split-normal (asymmetric bell) curve spanning the item's full scanned
        // min..max range, peaking at meanPrice. Each side gets its own standard
        // deviation so an off-center mean still produces a natural-looking curve
        // rather than a lopsided clamp. Draws are rejected and re-rolled instead of
        // clamped, so the shape isn't distorted by piling probability up at the edges;
        // with min/max sitting ~3 sigma out, a redraw is needed only rarely.
        float meanF = static_cast<float>(meanPrice);
        float sigmaLow = std::max((meanF - static_cast<float>(minPrice)) / kBuyoutSigmaDivisor, 1.0f);
        float sigmaHigh = std::max((static_cast<float>(maxPrice) - meanF) / kBuyoutSigmaDivisor, 1.0f);
        std::normal_distribution<float> standardNormal(0.0f, 1.0f);

        float price;
        uint32 attempts = 0;
        do
        {
            float z = standardNormal(RandomEngine::Instance());
            price = meanF + z * (z < 0.0f ? sigmaLow : sigmaHigh);
        } while ((price < minPrice || price > maxPrice) && ++attempts < 64);

        // Only reachable with malformed scan data (e.g. mean outside [min, max]);
        // guards against spinning forever rather than shaping the normal case.
        price = std::clamp(price, static_cast<float>(minPrice), static_cast<float>(maxPrice));

        return quantity * static_cast<uint32>(price);
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
        uint32 meanPrice,
        uint32 maxPrice,
        BuyTolerance const& tolerance,
        uint32 remainingScans)
    {
        if (pricePerItem <= meanPrice)
        {
            return true;
        }
        if (pricePerItem > maxPrice || maxPrice <= meanPrice)
        {
            return false;
        }

        float percentPosition =
            static_cast<float>(pricePerItem - meanPrice) / static_cast<float>(maxPrice - meanPrice);
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
