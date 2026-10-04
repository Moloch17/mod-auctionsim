#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>
#include "Define.h"

// One 12-value statistics block as emitted by data/compile-data.cpp: raw
// low/high/mean/median/mode, the raw quartiles, then the five central-tendency
// values recomputed after Tukey 1.5*IQR outlier trimming. The field order here
// MUST match compile-data.cpp's emitStats(); ParseStatBlock() relies on it.
struct StatBlock
{
    uint32 low = 0, high = 0, mean = 0, median = 0, mode = 0;
    uint32 q1 = 0, q3 = 0;
    uint32 adjLow = 0, adjHigh = 0, adjMean = 0, adjMedian = 0, adjMode = 0;
};

// Parses kStatsPerBlock consecutive fields starting at `offset` into `out`.
// Values from the int64 pipeline that overflow uint32 are clamped, not rejected.
// False if any field is non-numeric or `fields` is too short.
bool ParseStatBlock(std::vector<std::string_view> const& fields, size_t offset, StatBlock& out);

// One (faction, itemID, suffix) bucket of compiled market data, produced by
// data/compile-data.cpp from real Auctioneer scans. Every item row is a FIXED
// kRowFields (54) fields:
//
//   faction : itemID : suffix : priceSampleCount
//     : <12 price stats> : <12 stack-size stats> : <12 listing-count stats>
//     : <12 bid-ratio stats> : bidRatioSampleCount : listingSnapshotCount
//
//   price     per-unit buyout price. priceSampleCount = # buyout listings seen.
//   stack     listing stack size. Same record set as price. All-1 for gear
//             (written out anyway to keep every row one width).
//   listing   how many auctions of this item exist per AH snapshot (every
//             auction, buyout or bid-only). listingSnapshotCount = # snapshots.
//   bidRatio  starting bid as a fraction of buyout (MINBID / BUYOUT), in basis
//             points (fraction * 10000). bidRatioSampleCount = # listings that
//             had both a buyout and a min-bid. Reads 10000 (ratio 1.0) for
//             items with no observed min-bid data.
//
// Every listing/buying decision runs off the adjusted stats and the quartile
// band, never the raw min/max, so one extreme listing can't move the bot's idea
// of an item's price, stack size or market depth.
class ScannedItem
{
public:
    static constexpr size_t kIdentityFields = 4;  // faction, itemID, suffix, priceSampleCount
    static constexpr size_t kStatsPerBlock = 12;  // one StatBlock
    static constexpr size_t kStatBlockCount = 4;  // price, stack, listing-count, bid-ratio
    // + 2 trailing counts: bidRatioSampleCount, then listingSnapshotCount (last).
    static constexpr size_t kRowFields = kIdentityFields + kStatsPerBlock * kStatBlockCount + 2;  // 54

private:
    uint8 factionNum = 0;
    uint32 itemID = 0;
    int32 suffixID = 0;  // signed: negative = enchantment/suffix table, positive = random property table
    uint32 sampleCount = 0;

    StatBlock price;
    StatBlock stack;
    StatBlock listing;
    StatBlock bidRatio;  // MINBID / BUYOUT per listing, in basis points (fraction * 10000)
    uint32 bidRatioSampleCount = 0;
    uint32 listingSnapshotCount = 0;

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

    // Low end of the band a bid valuation is rolled from: the 25th percentile,
    // never above GetMarketPrice(). See AuctionPricing::RollBidValuation.
    uint32 GetBidValuationLow() const;

    // The stack size the market conventionally lists this item at -- outlier-
    // trimmed mode, i.e. "the size sellers actually use", not an average that can
    // fall between two conventional sizes. Always 1 for equippable gear.
    uint32 GetTypicalStackSize() const;

    // Outlier-trimmed observed stack range, used as the jitter band so a minority
    // of listings vary in size and the AH isn't wall-to-wall identical stacks.
    uint32 GetStackLow() const;
    uint32 GetStackHigh() const;

    // How many concurrent auctions of this item the market typically carries --
    // the outlier-trimmed median snapshot count. Used both to weight item
    // selection and to cap how many of this item the bot keeps listed.
    uint32 GetTypicalListingCount() const;

    // Number of AH snapshots this item appeared in (confidence for the count stat).
    uint32 GetListingSnapshotCount() const { return listingSnapshotCount; }

    // Starting bid as a fraction of buyout, in basis points (bp / 10000 = fraction).
    // The listing path stays in bp end-to-end (see AuctionPricing::RollStartBid).
    // Typical is the outlier-trimmed median with the same fallback chain as the
    // price getters; an all-zero bucket yields 10000 (ratio 1.0 -> startbid ==
    // buyout). Low/high fall back to typical so a thin bucket collapses toward it
    // and RollStartBid widens it into a small synthetic band.
    uint32 GetBidRatioTypicalBp() const;
    uint32 GetBidRatioLowBp() const;
    uint32 GetBidRatioHighBp() const;
    uint32 GetBidRatioSampleCount() const { return bidRatioSampleCount; }

    // Typical bid ratio as a plain fraction (bp / 10000). For tests / potential UI;
    // the listing path uses the *Bp accessors directly.
    float GetBidRatioTypical() const;

    // Parses one fixed kRowFields-field auctionsim.dat item row. std::nullopt if malformed.
    static std::optional<ScannedItem> TryParse(std::string_view dataLine);
};
