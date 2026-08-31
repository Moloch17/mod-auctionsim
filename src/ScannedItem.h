#pragma once

#include <optional>
#include <string_view>
#include "Define.h"

// One (faction, itemID, suffix) bucket of compiled market data. Each line of
// auctionsim.dat is 16 colon-separated fields, produced by data/compile-data.cpp
// from real Auctioneer scans:
//
//   faction : itemID : suffix : sampleCount
//     : rawLow : rawHigh : rawMean : rawMedian : rawMode
//     : q1 : q3
//     : adjLow : adjHigh : adjMean : adjMedian : adjMode
//
//   sampleCount  number of individual buyout listings behind this row.
//   raw*         central-tendency stats over every listing -- outliers and all.
//   q1 / q3      25th / 75th percentile of the raw list (the middle-50% band).
//   adj*         the same stats recomputed after dropping listings outside
//                Tukey's [Q1 - 1.5*IQR, Q3 + 1.5*IQR] fences -- i.e. the market
//                with one-off troll / fat-finger listings removed.
//
// The module drives every listing and buying decision off the adjusted stats and
// the interquartile band, never the raw min/max, so a single 100x listing can't
// move the bot's idea of what an item is worth.
class ScannedItem
{
    uint8 factionNum = 0;
    uint32 itemID = 0;
    int32 suffixID = 0;  // signed: negative = enchantment/suffix table, positive = random property table
    uint32 sampleCount = 0;

    uint32 rawLow = 0, rawHigh = 0, rawMean = 0, rawMedian = 0, rawMode = 0;
    uint32 q1 = 0, q3 = 0;
    uint32 adjLow = 0, adjHigh = 0, adjMean = 0, adjMedian = 0, adjMode = 0;

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
    // chasing a listing priced past the upper-middle of the market. (This used to
    // be the raw max, where one troll listing at 50x dragged the ceiling with it.)
    uint32 GetBuyCeiling() const;

    // Parses one 16-field auctionsim.dat line. std::nullopt on malformed input.
    static std::optional<ScannedItem> TryParse(std::string_view dataLine);
};
