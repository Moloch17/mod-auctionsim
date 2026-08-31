#pragma once

#include <optional>
#include <string_view>
#include "Define.h"

// One (faction, itemID, suffix) bucket of compiled market data, produced by
// data/compile-data.cpp from real Auctioneer scans. Rows are VARIABLE LENGTH:
//
//   16 fields  suffix != 0 -- equippable gear, which can only ever stack to 1,
//              so the 12 stack-size fields are omitted and taken as all-1.
//   28 fields  suffix == 0 -- the trailing 12 fields carry the real stack-size
//              distribution.
//
//   faction : itemID : suffix : sampleCount
//     : priceLow : priceHigh : priceMean : priceMedian : priceMode : priceQ1 : priceQ3
//     : priceAdjLow : priceAdjHigh : priceAdjMean : priceAdjMedian : priceAdjMode
//     [ : stackLow : stackHigh : stackMean : stackMedian : stackMode : stackQ1 : stackQ3
//       : stackAdjLow : stackAdjHigh : stackAdjMean : stackAdjMedian : stackAdjMode ]
//
//   sampleCount  listings behind this row; applies to both stat groups.
//   *Mean/Median/Mode  central-tendency stats over every listing (raw).
//   *Q1 / *Q3          25th / 75th percentile of the raw list.
//   *Adj*             the same stats recomputed after dropping listings outside
//                     Tukey's [Q1 - 1.5*IQR, Q3 + 1.5*IQR] fences -- the market
//                     with one-off troll / mistake listings removed.
//
// Every listing and buying decision runs off the adjusted stats and the
// interquartile band, never the raw min/max, so one extreme listing can't move
// the bot's idea of an item's price or its normal stack size.
class ScannedItem
{
    uint8 factionNum = 0;
    uint32 itemID = 0;
    int32 suffixID = 0;  // signed: negative = enchantment/suffix table, positive = random property table
    uint32 sampleCount = 0;

    uint32 rawLow = 0, rawHigh = 0, rawMean = 0, rawMedian = 0, rawMode = 0;
    uint32 q1 = 0, q3 = 0;
    uint32 adjLow = 0, adjHigh = 0, adjMean = 0, adjMedian = 0, adjMode = 0;

    // Left at 1 for 16-field rows (gear -- never stacks past 1).
    uint32 stackLow = 1, stackHigh = 1, stackMean = 1, stackMedian = 1, stackMode = 1;
    uint32 stackQ1 = 1, stackQ3 = 1;
    uint32 stackAdjLow = 1, stackAdjHigh = 1, stackAdjMean = 1, stackAdjMedian = 1, stackAdjMode = 1;

    ScannedItem() = default;

public:
    uint8 GetFactionNum() const { return factionNum; }
    uint32 GetItemID() const { return itemID; }
    int32 GetSuffixID() const { return suffixID; }
    uint32 GetSampleCount() const { return sampleCount; }

    // Robust "typical" per-unit price: the outlier-trimmed median, the most stable
    // central estimate for the right-skewed distributions real AH data has. Falls
    // back through the other central stats only for a degenerate/empty bucket.
    uint32 GetMarketPrice() const;

    // Per-unit jitter band for a new listing -- the outlier-trimmed low/high, so
    // the spread reflects the normal market and not one-off extremes. For thin
    // samples these collapse toward GetMarketPrice(); RollBuyoutPrice widens them
    // into a small synthetic band in that case.
    uint32 GetListLow() const;
    uint32 GetListHigh() const;

    // Hard ceiling for a buy decision: the 75th percentile. The bot will sometimes
    // pay up toward there, but never above it -- so it can't be baited into
    // chasing a listing priced past the upper-middle of the market.
    uint32 GetBuyCeiling() const;

    // The stack size the market conventionally lists this item at -- outlier-
    // trimmed mode, i.e. "the size sellers actually use", not an average that can
    // fall between two conventional sizes. Always 1 for equippable gear.
    uint32 GetTypicalStackSize() const;

    // Outlier-trimmed observed stack range, used as the jitter band so a minority
    // of listings vary in size and the AH isn't wall-to-wall identical stacks.
    uint32 GetStackLow() const;
    uint32 GetStackHigh() const;

    // Parses one 16- or 28-field auctionsim.dat line. std::nullopt on malformed input.
    static std::optional<ScannedItem> TryParse(std::string_view dataLine);
};
