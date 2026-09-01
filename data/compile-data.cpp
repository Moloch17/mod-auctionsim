// compile-data.cpp
//
// Parses Auctioneer Auc-ScanData.lua SavedVariables files, decodes their
// "ropes" fields (see explanation below), and emits auctionsim.dat.
//
// LINE 1:  "<itemRowCount> <categoryRowCount>"
//
// Then <categoryRowCount> CATEGORY ROWS, one per (faction, item class,
// quality) that appeared in the scans -- 16 fields:
//
//   faction:itemClass:quality:snapshotCount:
//     low:high:mean:median:mode:q1:q3:adjLow:adjHigh:adjMean:adjMedian:adjMode
//
// where the stats are over one sample per scan snapshot = the total number
// of auctions in that category in that snapshot. This is what the module
// uses to decide how full each category should be.
//
// Then <itemRowCount> ITEM ROWS, one per (faction, itemID, suffix), all a
// FIXED 41 fields -- no compression:
//
//   faction:itemID:suffix:priceSampleCount:
//     <12 price stats> : <12 stack-size stats> : <12 listing-count stats> :
//     listingSnapshotCount
//
// Each 12-stat block is: raw low/high/mean/median/mode, then q1/q3, then the
// 5 outlier-adjusted (Tukey 1.5x-IQR trimmed) central-tendency values. See
// computeStats(); short version: IQR fences are the standard distribution-
// free way to drop one-off troll / fat-finger listings, and real AH data is
// right-skewed so a plain mean is a poor "typical" signal.
//
//   price*   per-unit buyout price. priceSampleCount = # buyout listings.
//   stack*   listing stack size (COUNT). Same record set as price (so its
//            sampleCount == priceSampleCount). Reads all-1 for equippable
//            gear; written out anyway to keep every row the same width.
//   listing* how many auctions of this item exist per snapshot, counting
//            EVERY auction (buyout or bid-only). Its sample basis is
//            snapshots, not records, so it carries its own trailing
//            listingSnapshotCount.
//
// -----------------------------------------------------------------------
// Background: "ropes" is an array of Lua *source code* strings. Each one
// is a chunk like  return {{...},{...},...}  that Auctioneer feeds through
// loadstring()/pcall() to reconstitute a table of raw auction records.
// Getting there from the raw file requires undoing TWO layers of standard
// Lua backslash-escaping:
//   1) The outer SavedVariables file escapes the rope source as a normal
//      Lua string literal (\" -> ", \\ -> \).
//   2) The rope source itself is Lua, so string fields inside a record
//      (item link, name, texture path, etc.) use the same escaping rules
//      again.
// This program undoes layer 1 to recover the real rope source text, then
// walks that text with a Lua-string-literal-aware scanner (the same
// escape logic reused) to split it into records and fields.
//
// Each record is a flat, POSITIONAL array of 27 fields, matching
// Auctioneer's Const.lua ScanPosLabels order:
//   1 LINK 2 ILEVEL 3 ITYPE 4 ISUB 5 IEQUIP 6 PRICE 7 TLEFT 8 TIME
//   9 NAME 10 DEP2(texture) 11 COUNT 12 QUALITY 13 CANUSE 14 ULEVEL
//   15 MINBID 16 MININC 17 BUYOUT 18 CURBID 19 AMHIGH 20 SELLER 21 FLAG
//   22 BONUSES 23 ITEMID 24 SUFFIX(*) 25 FACTOR 26 ENCHANT 27 SEED
//   (*) SUFFIX is what this program uses for the output "enchantment"
//   column -- the random-property ID (e.g. "... of the Bear"), not the
//   permanent-enchant field 26.
//
// Build:   g++ -O2 -std=c++17 -pthread -o compile-data compile-data.cpp
// Usage:   ./compile-data
//          Scans every regular file (skipping subdirectories, symlinks,
//          etc.) in a "scans" subdirectory next to the executable's
//          working directory, running a tally of per-unit buyout prices
//          in memory across all of them, and writes the combined result
//          to "auctionsim.dat" once every file has been read.
//          Faction (Horde/Alliance) is detected per ropes block by
//          scanning upward in the file for the nearest ["Horde"] or
//          ["Alliance"] key -- AucScanData.scans.<Realm>.<Faction>
//          always has that key sitting above its ["ropes"] table -- and
//          mapped to FACTION_ID_HORDE / FACTION_ID_ALLIANCE below. A
//          single file with both a Horde and an Alliance section (e.g.
//          a Neutral-AH export) is handled correctly since detection is
//          per ropes block, not per file.
//
// Multithreading: the file list is split round-robin across
// std::thread::hardware_concurrency() worker threads. Each worker reads
// and parses its own files into its OWN private tally map -- no locking,
// no shared-map contention during the hot parsing loop. Once every
// worker finishes, the main thread merges the per-worker maps into one
// combined tally before writing the output file. The only thing workers
// share while running is a small mutex around the progress log line, so
// "Scanning ..." output from different threads doesn't get interleaved.
//
// Pricing: every price that goes into the stats is a PER-UNIT BUYOUT
// price -- (listing's total BUYOUT) / (stack COUNT). Bid-only listings
// (no buyout set) are skipped, since there's no buyout to normalize.
// Bucketing is by the composite key (faction, itemID, enchantment), where
// "enchantment" is the item link's random-property/SUFFIX ID (Const.lua
// field 24 -- e.g. "... of the Bear"), NOT the permanent player-applied
// ENCHANT field (field 26), which is almost always 0 on raw AH listings
// and barely differentiates anything. See the comment at the enchant
// extraction line in accumulateFile() for details.

#include <cstdint>
#include <cstdlib>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <cmath>

// Every price bucketed for the output file is a per-unit BUYOUT price:
// (listing's BUYOUT field) / (stack COUNT). Listings with no buyout set
// (buyout == 0, i.e. bid-only auctions) are skipped entirely, since there
// is no buyout price to normalize for them.
static constexpr bool SKIP_NO_BUYOUT = true;

// -----------------------------------------------------------------------
// Reads a Lua double-quoted string literal starting at text[i] (which
// must be '"'), honoring backslash-escapes, and returns the *unescaped*
// content. Advances i to just past the closing quote.
// -----------------------------------------------------------------------
static std::string readLuaString(const std::string& text, size_t& i) {
    std::string out;
    // text[i] == '"'
    ++i;
    while (i < text.size() && text[i] != '"') {
        if (text[i] == '\\' && i + 1 < text.size()) {
            char c = text[i + 1];
            switch (c) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case '"': out += '"';  break;
                case '\\': out += '\\'; break;
                default: out += c; break; // covers \', escaped digits, etc.
            }
            i += 2;
        } else {
            out += text[i];
            ++i;
        }
    }
    if (i < text.size()) ++i; // skip closing quote
    return out;
}

static void skipWhitespaceAndCommas(const std::string& text, size_t& i) {
    while (i < text.size() &&
           (std::isspace(static_cast<unsigned char>(text[i])) || text[i] == ',')) {
        ++i;
    }
}

// Like skipWhitespaceAndCommas, but also skips Lua "-- ..." line comments.
// WoW's SavedVariables writer puts a "-- [N]" index comment after every array
// element, so the ["ropes"] table looks like:  "return {...}", -- [1]
//                                              "return {...}", -- [2]
// Without eating those, extractRopes would stop after the very first rope and
// silently drop the rest of every multi-rope scan file.
static void skipTrivia(const std::string& text, size_t& i) {
    for (;;) {
        skipWhitespaceAndCommas(text, i);
        if (i + 1 < text.size() && text[i] == '-' && text[i + 1] == '-') {
            size_t nl = text.find('\n', i);
            i = (nl == std::string::npos) ? text.size() : nl + 1;
            continue;
        }
        break;
    }
}

static void skipWhitespace(const std::string& text, size_t& i) {
    while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
        ++i;
    }
}

// -----------------------------------------------------------------------
// Parses one already-unescaped rope source, e.g.:
//   return {{"link",219,"Armor",...,0,},{"link",...},}
// into a list of records, each a flat vector of raw field strings
// (numbers/nil/true/false are returned as their literal text; quoted
// strings are returned unescaped).
// -----------------------------------------------------------------------
static std::vector<std::vector<std::string>> parseRopeRecords(const std::string& rope) {
    std::vector<std::vector<std::string>> records;

    size_t i = rope.find('{');
    if (i == std::string::npos) return records;
    ++i; // past outer '{'

    while (true) {
        skipWhitespaceAndCommas(rope, i);
        if (i >= rope.size() || rope[i] == '}') break; // end of outer table

        if (rope[i] != '{') break; // malformed, bail out gracefully
        ++i; // past record '{'

        std::vector<std::string> fields;
        while (true) {
            skipWhitespace(rope, i);
            if (i >= rope.size()) break;
            if (rope[i] == '}') { ++i; break; } // end of record

            if (rope[i] == '"') {
                fields.push_back(readLuaString(rope, i));
            } else {
                size_t start = i;
                while (i < rope.size() && rope[i] != ',' && rope[i] != '}') ++i;
                std::string tok = rope.substr(start, i - start);
                // trim trailing whitespace
                while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back())))
                    tok.pop_back();
                fields.push_back(tok);
            }

            skipWhitespace(rope, i);
            if (i < rope.size() && rope[i] == ',') { ++i; continue; }
            if (i < rope.size() && rope[i] == '}') { ++i; break; }
        }

        records.push_back(std::move(fields));
    }

    return records;
}

// Faction IDs used in the output. Auc-ScanData.lua nests each faction's
// data under a ["Horde"] or ["Alliance"] key (see AucScanData.scans.
// <Realm>.<Faction> in the file), which appears above that faction's
// ["ropes"] block. These constants are the IDs written to the output file
// and MUST match AzerothCore's AuctionHouseId enum (Alliance = 2, Horde = 6),
// since the module indexes its per-faction tables straight by this value.
static constexpr int FACTION_ID_ALLIANCE = 2;
static constexpr int FACTION_ID_HORDE    = 6;
static constexpr int FACTION_ID_UNKNOWN  = 0;

// Looks backward from a ["ropes"] block's position to find whichever of
// ["Horde"] / ["Alliance"] appears closest above it in the file -- that's
// the faction that block's ropes belong to.
static int detectFactionAbove(const std::string& fileText, size_t beforePos) {
    size_t hordePos = fileText.rfind("[\"Horde\"]", beforePos);
    size_t allyPos  = fileText.rfind("[\"Alliance\"]", beforePos);

    if (hordePos == std::string::npos && allyPos == std::string::npos)
        return FACTION_ID_UNKNOWN;
    if (allyPos == std::string::npos) return FACTION_ID_HORDE;
    if (hordePos == std::string::npos) return FACTION_ID_ALLIANCE;
    return (hordePos > allyPos) ? FACTION_ID_HORDE : FACTION_ID_ALLIANCE;
}

struct RopeBlock {
    std::string source;
    int factionId;
};

// Extracts every rope source string from every ["ropes"] = { ... } block
// found in the raw SavedVariables file text, tagging each with the
// Horde/Alliance faction found directly above that block.
static std::vector<RopeBlock> extractRopes(const std::string& fileText) {
    std::vector<RopeBlock> ropes;
    size_t searchFrom = 0;
    const std::string marker = "\"ropes\"";

    while (true) {
        size_t markerPos = fileText.find(marker, searchFrom);
        if (markerPos == std::string::npos) break;

        int factionId = detectFactionAbove(fileText, markerPos);

        size_t bracePos = fileText.find('{', markerPos);
        if (bracePos == std::string::npos) break;

        size_t i = bracePos + 1;
        while (true) {
            skipTrivia(fileText, i);
            if (i >= fileText.size() || fileText[i] == '}') { ++i; break; }
            if (fileText[i] != '"') break; // malformed, stop this block
            ropes.push_back({readLuaString(fileText, i), factionId});
        }

        searchFrom = i;
    }

    return ropes;
}

// Central-tendency stats (low/high/mean/median/mode) computed from any
// already-sorted, non-empty price vector. Shared by both the raw stats
// and the outlier-adjusted stats below, since the math is identical --
// only the input list differs.
struct CentralStats {
    int64_t low = 0, high = 0, mean = 0, median = 0, mode = 0;
};

static CentralStats computeCentralStats(const std::vector<double>& sortedPrices) {
    CentralStats c;
    size_t n = sortedPrices.size();
    if (n == 0) return c; // defensive; callers never pass an empty vector

    c.low  = static_cast<int64_t>(std::llround(sortedPrices.front()));
    c.high = static_cast<int64_t>(std::llround(sortedPrices.back()));

    double sum = 0.0;
    for (double p : sortedPrices) sum += p;
    c.mean = static_cast<int64_t>(std::llround(sum / n));

    double med = (n % 2 == 1)
        ? sortedPrices[n / 2]
        : (sortedPrices[n / 2 - 1] + sortedPrices[n / 2]) / 2.0;
    c.median = static_cast<int64_t>(std::llround(med));

    // Mode: most frequent rounded price, ties broken by lowest price. On
    // small/all-distinct samples this often just picks the lowest price
    // (every value tied at frequency 1) -- that's mathematically correct,
    // just not a meaningful "typical price" signal when n is small.
    std::unordered_map<int64_t, int> freq;
    for (double p : sortedPrices) freq[static_cast<int64_t>(std::llround(p))]++;
    int bestCount = -1;
    for (auto& kv : freq) {
        if (kv.second > bestCount ||
            (kv.second == bestCount && kv.first < c.mode)) {
            bestCount = kv.second;
            c.mode = kv.first;
        }
    }
    return c;
}

// Linear-interpolation percentile (the "R-7" method -- matches Excel's
// QUARTILE.INC and most stats packages). sortedPrices must already be
// sorted ascending. p is in [0,1] (0.25 for Q1, 0.75 for Q3).
static double percentile(const std::vector<double>& sortedPrices, double p) {
    size_t n = sortedPrices.size();
    if (n == 1) return sortedPrices[0];
    double idx = p * static_cast<double>(n - 1);
    size_t lo = static_cast<size_t>(std::floor(idx));
    size_t hi = std::min(lo + 1, n - 1);
    double frac = idx - static_cast<double>(lo);
    return sortedPrices[lo] + frac * (sortedPrices[hi] - sortedPrices[lo]);
}

// Full stats for one item/enchant bucket: raw central-tendency stats,
// the Q1/Q3 quartile range, and a second set of central-tendency stats
// recomputed after trimming outliers via Tukey's 1.5x-IQR fences. See
// the top-of-file comment for the reasoning behind these choices.
//
// emitStats() below writes these in a fixed order that the module parses back
// field-for-field: keep it in sync with `struct StatBlock` in
// modules/mod-auctionsim/src/ScannedItem.h.
struct Stats {
    size_t sampleCount = 0;

    // Raw, unfiltered stats -- what actually happened in the market.
    int64_t low = 0, high = 0, mean = 0, median = 0, mode = 0;

    // Quartile range of the raw (untrimmed) price list.
    int64_t q1 = 0, q3 = 0;

    // Central-tendency stats recomputed after dropping listings outside
    // [Q1 - 1.5*IQR, Q3 + 1.5*IQR] -- what the market normally looks
    // like once one-off troll/mistake listings are thrown out.
    int64_t adjLow = 0, adjHigh = 0, adjMean = 0, adjMedian = 0, adjMode = 0;
};

static Stats computeStats(std::vector<double>& prices) {
    Stats s;
    std::sort(prices.begin(), prices.end());
    s.sampleCount = prices.size();

    CentralStats raw = computeCentralStats(prices);
    s.low = raw.low; s.high = raw.high;
    s.mean = raw.mean; s.median = raw.median; s.mode = raw.mode;

    double q1 = percentile(prices, 0.25);
    double q3 = percentile(prices, 0.75);
    s.q1 = static_cast<int64_t>(std::llround(q1));
    s.q3 = static_cast<int64_t>(std::llround(q3));

    double iqr = q3 - q1;
    double fenceLow  = std::max(0.0, q1 - 1.5 * iqr); // prices can't be negative
    double fenceHigh = q3 + 1.5 * iqr;

    std::vector<double> trimmed;
    trimmed.reserve(prices.size());
    for (double p : prices) {
        if (p >= fenceLow && p <= fenceHigh) trimmed.push_back(p);
    }
    // Guard against ever producing an empty adjusted set (shouldn't
    // happen with valid fence math, but float precision on edge cases
    // is cheap insurance against a bucket with no adjusted stats at all).
    if (trimmed.empty()) trimmed = prices;

    CentralStats adj = computeCentralStats(trimmed);
    s.adjLow = adj.low; s.adjHigh = adj.high;
    s.adjMean = adj.mean; s.adjMedian = adj.median; s.adjMode = adj.mode;

    return s;
}

// Composite key: one unique bucket per (faction, itemID, enchant) combo.
struct ItemEnchantKey {
    int32_t factionId;
    uint32_t itemId;
    int32_t enchant;
    bool operator==(const ItemEnchantKey& o) const {
        return factionId == o.factionId && itemId == o.itemId &&
               enchant == o.enchant;
    }
};
struct ItemEnchantKeyHash {
    size_t operator()(const ItemEnchantKey& k) const {
        // Combine the three fields into one hash (simple, well-distributed
        // enough for this data size).
        size_t h = static_cast<size_t>(k.factionId);
        h = h * 1000003u ^ static_cast<size_t>(k.itemId);
        h = h * 1000003u ^ static_cast<size_t>(static_cast<uint32_t>(k.enchant));
        return h;
    }
};
// Generic "one bucket per item/enchant, holding a list of raw numeric
// samples" map -- used for the price tally, the stack-count tally and the
// per-item listing-count tally below, since computeStats works identically
// on any kind of number.
using NumericBuckets =
    std::unordered_map<ItemEnchantKey, std::vector<double>, ItemEnchantKeyHash>;
using PriceBuckets = NumericBuckets; // alias for readability at call sites
using StackBuckets = NumericBuckets;
using ListingCountBuckets = NumericBuckets;

// Composite key: one bucket per (faction, item class, quality). Holds one
// sample per scan snapshot = the total number of auctions in that category
// in that snapshot. Drives the module's per-category listing depth.
struct CategoryKey {
    int32_t factionId;
    int32_t itemClass;
    int32_t quality;
    bool operator==(const CategoryKey& o) const {
        return factionId == o.factionId && itemClass == o.itemClass && quality == o.quality;
    }
};
struct CategoryKeyHash {
    size_t operator()(const CategoryKey& k) const {
        size_t h = static_cast<size_t>(k.factionId);
        h = h * 1000003u ^ static_cast<size_t>(k.itemClass);
        h = h * 1000003u ^ static_cast<size_t>(k.quality);
        return h;
    }
};
using CategoryCountBuckets =
    std::unordered_map<CategoryKey, std::vector<double>, CategoryKeyHash>;

// Scan records carry ITYPE as a localized string (Const.lua field 3). Map
// the enUS values to AzerothCore's ITEM_CLASS_* numeric ids so category
// buckets can be keyed the same way the module indexes them. Obsolete
// classes (Generic/Money/Permanent) never appear in real AH data and are
// left out. A non-enUS scan simply won't match here -- those records still
// feed the per-item stats, they're just left out of the category totals
// (reported as "unrecognized item type" in the summary).
static const std::unordered_map<std::string, int> kItypeToClass = {
    {"Consumable", 0},   {"Container", 1},   {"Weapon", 2},   {"Gem", 3},
    {"Armor", 4},        {"Reagent", 5},     {"Projectile", 6}, {"Trade Goods", 7},
    {"Recipe", 9},       {"Quiver", 11},     {"Quest", 12},    {"Key", 13},
    {"Miscellaneous", 15}, {"Glyph", 16},
};

// Guards stderr progress/warning output so lines from different worker
// threads don't get interleaved mid-line.
static std::mutex g_logMutex;

// Counts of why a raw record was skipped/not tallied, broken out by
// reason so the completion summary can show exactly where the losses
// are instead of one opaque total.
struct SkipCounts {
    size_t malformed   = 0; // fewer than 27 fields on the record
    size_t noItemId    = 0; // ITEMID field was 0/missing
    size_t noBuyout    = 0; // bid-only listing, no buyout to normalize (price/stack only)
    size_t unknownItype = 0; // ITYPE didn't map to a class -> left out of category totals
    size_t total() const { return malformed + noItemId + noBuyout; }
};

// Reads one Auc-ScanData.lua file, decodes its ropes, and folds every
// valid record's per-unit buyout price (and raw stack COUNT) into the
// given priceBuckets/stackBuckets tallies (keyed by faction+itemID+
// enchant, faction detected from the ["Horde"]/["Alliance"] key found
// above each ropes block). Both maps are owned by whichever single
// thread calls this -- accumulateFile itself does no locking on them,
// only on the stderr log lines. Returns false if the file couldn't be
// read or had no ropes data at all (caller just skips it and moves on).
static bool accumulateFile(const std::string& path,
                            PriceBuckets& priceBuckets,
                            StackBuckets& stackBuckets,
                            ListingCountBuckets& listingCountBuckets,
                            CategoryCountBuckets& categoryCountBuckets,
                            size_t& recordCount,
                            SkipCounts& skips,
                            size_t& unknownFactionCount) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "  (skipping, could not open) " << path << "\n";
        return false;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    std::string fileText = buf.str();

    std::vector<RopeBlock> ropes = extractRopes(fileText);
    if (ropes.empty()) {
        std::lock_guard<std::mutex> lock(g_logMutex);
        std::cerr << "  (skipping, no ropes data) " << path << "\n";
        return false;
    }

    auto toLong = [](const std::string& s, long fallback) -> long {
        if (s.empty() || s == "nil" || s == "true" || s == "false")
            return fallback;
        try { return std::stol(s); } catch (...) { return fallback; }
    };

    // One file == one AH snapshot. Count every auction in it, per item and per
    // category (buyout or bid-only alike -- it's about how full the AH is), then
    // fold one sample per key into the shared tallies at the end.
    std::unordered_map<ItemEnchantKey, uint32_t, ItemEnchantKeyHash> perFileItemCounts;
    std::unordered_map<CategoryKey, uint32_t, CategoryKeyHash> perFileCatCounts;

    for (const RopeBlock& block : ropes) {
        if (block.factionId == FACTION_ID_UNKNOWN) {
            std::lock_guard<std::mutex> lock(g_logMutex);
            std::cerr << "  (warning: could not find Horde/Alliance above "
                          "a ropes block in) " << path << "\n";
        }

        auto records = parseRopeRecords(block.source);
        for (auto& fields : records) {
            if (fields.size() < 27) { ++skips.malformed; continue; }

            uint32_t itemId = static_cast<uint32_t>(toLong(fields[22], 0));
            if (itemId == 0) { ++skips.noItemId; continue; }

            // "Enchantment" here means the item link's random-property /
            // suffix ID (Const.lua SUFFIX, field 24) -- the ID behind
            // variants like "... of the Bear" / "... of the Whale". This
            // is NOT the same as the permanent player-applied ENCHANT
            // field (26), which is almost always 0 on raw AH listings and
            // barely differentiates anything. SUFFIX is what actually
            // produces the ~10+ variants per base item you see in real
            // auction data.
            int32_t enchant = static_cast<int32_t>(toLong(fields[23], 0));

            long count = toLong(fields[10], 1);
            if (count <= 0) count = 1;

            // Listing-depth tally: every auction counts, before the buyout
            // filter below. Per item...
            ++perFileItemCounts[ItemEnchantKey{block.factionId, itemId, enchant}];
            // ...and per (class, quality), if ITYPE maps to a known class.
            {
                auto itypeIt = kItypeToClass.find(fields[2]);
                long quality = toLong(fields[11], -1);
                if (itypeIt == kItypeToClass.end() || quality < 0 || quality >= 8) {
                    ++skips.unknownItype;
                } else {
                    ++perFileCatCounts[CategoryKey{
                        block.factionId, itypeIt->second, static_cast<int32_t>(quality)}];
                }
            }

            double buyout = static_cast<double>(toLong(fields[16], 0));
            if (SKIP_NO_BUYOUT && buyout <= 0) { ++skips.noBuyout; continue; }

            // Per-unit buyout price: total buyout divided by stack size.
            double chosen = buyout / static_cast<double>(count);

            if (block.factionId == FACTION_ID_UNKNOWN) ++unknownFactionCount;
            ItemEnchantKey key{block.factionId, itemId, enchant};
            priceBuckets[key].push_back(chosen);
            // Stack-count tally uses the SAME set of records as the price
            // tally (i.e. only records that actually contributed a price),
            // so both distributions share an identical sampleCount and
            // stay directly comparable row to row.
            stackBuckets[key].push_back(static_cast<double>(count));
            ++recordCount;
        }
    }

    // Flush this snapshot's per-key totals: one sample per item / category.
    for (auto& kv : perFileItemCounts)
        listingCountBuckets[kv.first].push_back(static_cast<double>(kv.second));
    for (auto& kv : perFileCatCounts)
        categoryCountBuckets[kv.first].push_back(static_cast<double>(kv.second));

    return true;
}

// Everything one worker thread accumulates from its slice of the file
// list -- kept entirely private to the thread until the main thread
// merges all of these together after every worker has finished.
struct WorkerResult {
    PriceBuckets priceBuckets;
    StackBuckets stackBuckets;
    ListingCountBuckets listingCountBuckets;
    CategoryCountBuckets categoryCountBuckets;
    size_t recordCount = 0;
    SkipCounts skips;
    size_t unknownFactionCount = 0;
    size_t filesProcessed = 0;
    size_t filesSkipped = 0;
};

static void workerThreadMain(const std::vector<std::string>& paths,
                              WorkerResult& result) {
    for (const std::string& path : paths) {
        {
            std::lock_guard<std::mutex> lock(g_logMutex);
            std::cerr << "Scanning " << path << "...\n";
        }
        if (accumulateFile(path, result.priceBuckets, result.stackBuckets,
                            result.listingCountBuckets, result.categoryCountBuckets,
                            result.recordCount, result.skips,
                            result.unknownFactionCount)) {
            ++result.filesProcessed;
        } else {
            ++result.filesSkipped;
        }
    }
}

// Folds one worker's private tally into the combined tally. Cheap: for a
// key the combined map hasn't seen yet, the whole sample vector is moved
// in; for a key both workers happened to hit, the smaller vector's entries
// are appended. Templated so it serves the item-keyed tallies (price,
// stack, per-item listing count) and the category-keyed tally alike.
template <typename BucketMap>
static void mergeBuckets(BucketMap& combined, BucketMap&& worker) {
    for (auto& kv : worker) {
        auto it = combined.find(kv.first);
        if (it == combined.end()) {
            combined.emplace(kv.first, std::move(kv.second));
        } else {
            auto& dst = it->second;
            dst.insert(dst.end(),
                       std::make_move_iterator(kv.second.begin()),
                       std::make_move_iterator(kv.second.end()));
        }
    }
}

int main() {
    const auto startTime = std::chrono::steady_clock::now();

    const std::string scansDir = "scans";
    const std::string outputPath = "auctionsim.dat";

    if (!std::filesystem::is_directory(scansDir)) {
        std::cerr << "Directory not found: " << scansDir << "\n";
        return 1;
    }

    // Gather the full file list up front so it can be split across
    // worker threads. This also lets us report early if there's simply
    // nothing to do.
    std::vector<std::string> filePaths;
    for (const auto& entry : std::filesystem::directory_iterator(scansDir)) {
        // directory_iterator never yields "." or ".."; this check just
        // makes sure we skip subdirectories, symlinks-to-dirs, etc. and
        // only read plain files.
        if (!entry.is_regular_file()) continue;
        filePaths.push_back(entry.path().string());
    }

    if (filePaths.empty()) {
        std::cerr << "No files found in " << scansDir << "\n";
        return 1;
    }

    unsigned int hwThreads = std::thread::hardware_concurrency();
    if (hwThreads == 0) hwThreads = 4; // hardware_concurrency() can return 0
    const unsigned int numThreads =
        std::min<unsigned int>(hwThreads, static_cast<unsigned int>(filePaths.size()));

    // Split the file list round-robin across workers. Round-robin (rather
    // than contiguous chunks) spreads out any files that happen to be
    // clustered together by size/complexity more evenly than a straight
    // slice would.
    std::vector<std::vector<std::string>> workerFiles(numThreads);
    for (size_t i = 0; i < filePaths.size(); ++i) {
        workerFiles[i % numThreads].push_back(filePaths[i]);
    }

    std::vector<WorkerResult> results(numThreads);
    std::vector<std::thread> threads;
    threads.reserve(numThreads);
    for (unsigned int t = 0; t < numThreads; ++t) {
        threads.emplace_back(workerThreadMain,
                              std::cref(workerFiles[t]),
                              std::ref(results[t]));
    }
    for (auto& th : threads) th.join();

    // Merge every worker's private tally into one combined tally, and
    // sum up their counters.
    PriceBuckets priceBuckets;
    StackBuckets stackBuckets;
    ListingCountBuckets listingCountBuckets;
    CategoryCountBuckets categoryCountBuckets;
    size_t recordCount = 0, unknownFactionCount = 0;
    size_t filesProcessed = 0, filesSkipped = 0;
    SkipCounts skips;

    for (auto& result : results) {
        mergeBuckets(priceBuckets, std::move(result.priceBuckets));
        mergeBuckets(stackBuckets, std::move(result.stackBuckets));
        mergeBuckets(listingCountBuckets, std::move(result.listingCountBuckets));
        mergeBuckets(categoryCountBuckets, std::move(result.categoryCountBuckets));
        recordCount         += result.recordCount;
        skips.malformed      += result.skips.malformed;
        skips.noItemId        += result.skips.noItemId;
        skips.noBuyout         += result.skips.noBuyout;
        skips.unknownItype   += result.skips.unknownItype;
        unknownFactionCount += result.unknownFactionCount;
        filesProcessed      += result.filesProcessed;
        filesSkipped        += result.filesSkipped;
    }

    if (filesProcessed == 0) {
        std::cerr << "No usable scan files found in " << scansDir << "\n";
        return 1;
    }

    // Helper: append computeStats' 12 stat values (raw low/high/mean/median/
    // mode, q1/q3, then the 5 outlier-adjusted values) to a line stream.
    auto emitStats = [](std::ostringstream& line, const Stats& s) {
        line << s.low << ':' << s.high << ':' << s.mean << ':'
             << s.median << ':' << s.mode << ':'
             << s.q1 << ':' << s.q3 << ':'
             << s.adjLow << ':' << s.adjHigh << ':' << s.adjMean << ':'
             << s.adjMedian << ':' << s.adjMode;
    };

    // Category header rows: faction:class:quality:snapshotCount + 12 stats.
    std::vector<std::string> categoryLines;
    categoryLines.reserve(categoryCountBuckets.size());
    for (auto& bucket : categoryCountBuckets) {
        Stats cs = computeStats(bucket.second);
        std::ostringstream line;
        line << bucket.first.factionId << ':' << bucket.first.itemClass << ':'
             << bucket.first.quality << ':' << cs.sampleCount << ':';
        emitStats(line, cs);
        categoryLines.push_back(line.str());
    }

    // Item rows -- fixed width, always 41 fields:
    //   faction:itemID:enchant:priceSampleCount
    //   + 12 price stats + 12 stack-size stats + 12 listing-count stats
    //   + listingSnapshotCount
    std::vector<std::string> lines;
    lines.reserve(priceBuckets.size());

    int32_t  maxFactionId   = 0;
    uint32_t maxItemId      = 0;
    int32_t  maxEnchant     = 0;
    size_t   maxSampleCount = 0;

    static const std::vector<double> onesFallback = {1.0};

    for (auto& bucket : priceBuckets) {
        int32_t  factionId = bucket.first.factionId;
        uint32_t itemId    = bucket.first.itemId;
        int32_t  enchant   = bucket.first.enchant;

        Stats priceStats = computeStats(bucket.second);
        if (priceStats.sampleCount > maxSampleCount) {
            maxSampleCount = priceStats.sampleCount;
            maxFactionId = factionId; maxItemId = itemId; maxEnchant = enchant;
        }

        auto stackIt = stackBuckets.find(bucket.first);
        std::vector<double> stackCopy =
            (stackIt != stackBuckets.end()) ? stackIt->second : onesFallback;
        Stats stackStats = computeStats(stackCopy);

        auto listIt = listingCountBuckets.find(bucket.first);
        std::vector<double> listCopy =
            (listIt != listingCountBuckets.end()) ? listIt->second : onesFallback;
        Stats listStats = computeStats(listCopy);

        std::ostringstream line;
        line << factionId << ':' << itemId << ':' << enchant << ':'
             << priceStats.sampleCount << ':';
        emitStats(line, priceStats);
        line << ':'; emitStats(line, stackStats);
        line << ':'; emitStats(line, listStats);
        line << ':' << listStats.sampleCount;

        lines.push_back(line.str());
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputPath << "\n";
        return 1;
    }

    // Row 1: "<item row count> <category row count>". Category rows follow,
    // then the item rows.
    out << lines.size() << ' ' << categoryLines.size() << '\n';
    for (const auto& line : categoryLines) out << line << '\n';
    for (const auto& line : lines) out << line << '\n';
    out.close();

    const auto endTime = std::chrono::steady_clock::now();
    const double elapsedSeconds =
        std::chrono::duration<double>(endTime - startTime).count();

    std::cout << "Done.\n"
              << "  Threads used:      " << numThreads << "\n"
              << "  Files read:        " << filesProcessed
              << " (" << filesSkipped << " skipped)\n"
              << "  Raw listings read: " << recordCount
              << " (" << skips.total() << " skipped total)\n"
              << "    - malformed record (< 27 fields):  " << skips.malformed << "\n"
              << "    - missing/zero ITEMID:              " << skips.noItemId << "\n"
              << "    - bid-only, no BUYOUT" << (SKIP_NO_BUYOUT ? "" : " (kept, SKIP_NO_BUYOUT=false)")
              << ":         " << skips.noBuyout << "\n"
              << "    - unrecognized item type (no cat):  " << skips.unknownItype << "\n"
              << "  Item rows out:     " << lines.size()
              << " (deduped by faction+item+enchant)\n"
              << "  Category rows out: " << categoryLines.size()
              << " (deduped by faction+class+quality)\n"
              << "  Most price data:   faction " << maxFactionId
              << ", item " << maxItemId << ", enchant " << maxEnchant
              << " (" << maxSampleCount << " listings)\n"
              << "  Time taken:        " << elapsedSeconds << " s\n"
              << "  Output file:       " << outputPath << "\n";

    if (unknownFactionCount > 0) {
        std::cout << "  Warning: " << unknownFactionCount
                  << " records had no detectable Horde/Alliance faction "
                     "above their ropes block (see stderr for which "
                     "files) and were written with factionId "
                  << FACTION_ID_UNKNOWN << ".\n";
    }

    return 0;
}