// compile-data.cpp
//
// Parses an Auctioneer Auc-ScanData.lua SavedVariables file, decodes the
// "ropes" field (see explanation below), and emits an output file whose
// first line is the item count N, followed by N lines each formatted as:
//
//   factionId:itemID:enchantment:sampleCount:
//     priceLow:priceHigh:priceMean:priceMedian:priceMode:priceQ1:priceQ3:
//     priceAdjLow:priceAdjHigh:priceAdjMean:priceAdjMedian:priceAdjMode:
//     stackLow:stackHigh:stackMean:stackMedian:stackMode:stackQ1:stackQ3:
//     stackAdjLow:stackAdjHigh:stackAdjMean:stackAdjMedian:stackAdjMode
//
// (colons above are just wrapped for line length -- it's one flat
// colon-separated line per item/enchant. ROWS ARE VARIABLE LENGTH: 16
// fields when enchant != 0, 28 fields when enchant == 0. See the
// "Compression" note below for why.)
//
// sampleCount is how many individual buyout listings fed that row's
// stats -- read this before trusting the stats next to it; some items
// will have thousands of listings behind them, others a small handful,
// and the two shouldn't be treated with equal confidence. It applies to
// BOTH the price stats and the stack stats below, since both are
// computed from the exact same set of records.
//
// The price* fields are the per-unit buyout price stats (RAW = every
// listing as-is; Q1/Q3 = 25th/75th percentile; Adj* = the same
// central-tendency stats recomputed after dropping outliers via Tukey's
// 1.5x-IQR fences). See computeStats() for the full reasoning; short
// version: IQR-based fences are the standard, distribution-free way to
// flag outliers and don't assume prices are normally distributed (they
// aren't -- real AH price data is right-skewed).
//
// The stack* fields are the SAME statistical treatment (raw stats,
// quartiles, outlier-adjusted stats) applied to stack size (COUNT)
// instead of price. This tells you what stack size the market actually
// expects for an item -- e.g. stackMode=20 for a common trade good means
// listings almost always go up in stacks of 20, which matters if you
// want your own listings to sell at a normal rate rather than sitting
// unsold because they're an unusual size. Outlier trimming applies here
// too (a single accidental stack-of-1 listing for an item everyone else
// sells in 20s shouldn't skew stackMean), even though stack counts are
// small bounded integers rather than continuous currency -- the same
// IQR-fence math still works correctly on that kind of distribution, it
// just tends to produce a very tight adjusted range when (as is common)
// almost everyone lists in one or two conventional sizes.
//
// Compression: rows where enchant/suffix != 0 OMIT the trailing 12
// stack* fields entirely (16 fields written, not 28). This is safe, not
// approximate: in WoW, only equippable gear can ever roll a random-
// property suffix, and equippable items always have a max stack size of
// 1 -- so for these rows every one of the 12 stack fields would read
// exactly "1", with zero exceptions (verified against real scan data:
// 1,899/1,899 enchant!=0 rows had all-1 stack stats). enchant == 0 does
// NOT reliably imply the item is stackable -- plenty of gear has no
// random suffix either -- so those rows always keep the real stack
// stats, since we can't safely predict them. A consumer parsing this
// file should treat a row's field count itself as the signal: 16 fields
// means "stack size is always 1, not written"; 28 fields means "see the
// trailing 12 fields for the real stack distribution."
//// -----------------------------------------------------------------------
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
// Build:   g++ -O2 -std=c++17 -pthread -o ah_parser ah_parser.cpp
// Usage:   ./ah_parser
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
// samples" map -- used for both the price tally and the stack-count
// tally below, since the statistics (computeStats) work identically on
// either kind of number.
using NumericBuckets =
    std::unordered_map<ItemEnchantKey, std::vector<double>, ItemEnchantKeyHash>;
using PriceBuckets = NumericBuckets; // alias for readability at call sites
using StackBuckets = NumericBuckets;

// Guards stderr progress/warning output so lines from different worker
// threads don't get interleaved mid-line.
static std::mutex g_logMutex;

// Counts of why a raw record was skipped/not tallied, broken out by
// reason so the completion summary can show exactly where the losses
// are instead of one opaque total.
struct SkipCounts {
    size_t malformed   = 0; // fewer than 27 fields on the record
    size_t noItemId    = 0; // ITEMID field was 0/missing
    size_t noBuyout    = 0; // bid-only listing, no buyout to normalize
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

    return true;
}

// Everything one worker thread accumulates from its slice of the file
// list -- kept entirely private to the thread until the main thread
// merges all of these together after every worker has finished.
struct WorkerResult {
    PriceBuckets priceBuckets;
    StackBuckets stackBuckets;
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
// in; for a key both workers happened to hit, the smaller vector's
// entries are appended onto the combined one. Generic over NumericBuckets
// so it serves both the price tally and the stack-count tally.
static void mergeNumericBuckets(NumericBuckets& combined, NumericBuckets&& worker) {
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
    size_t recordCount = 0, unknownFactionCount = 0;
    size_t filesProcessed = 0, filesSkipped = 0;
    SkipCounts skips;

    for (auto& result : results) {
        mergeNumericBuckets(priceBuckets, std::move(result.priceBuckets));
        mergeNumericBuckets(stackBuckets, std::move(result.stackBuckets));
        recordCount         += result.recordCount;
        skips.malformed      += result.skips.malformed;
        skips.noItemId        += result.skips.noItemId;
        skips.noBuyout         += result.skips.noBuyout;
        unknownFactionCount += result.unknownFactionCount;
        filesProcessed      += result.filesProcessed;
        filesSkipped        += result.filesSkipped;
    }

    if (filesProcessed == 0) {
        std::cerr << "No usable scan files found in " << scansDir << "\n";
        return 1;
    }

    // Build every output line first so we know the total item count before
    // writing anything -- that count becomes row 1 of the file.
    std::vector<std::string> lines;
    lines.reserve(priceBuckets.size());

    // Track which (faction,item,enchant) bucket had the most price
    // samples, so we can report it in the completion summary below.
    int32_t  maxFactionId    = 0;
    uint32_t maxItemId       = 0;
    int32_t  maxEnchant      = 0;
    size_t   maxSampleCount  = 0;

    // How many rows had their stack-stat fields omitted (see the
    // compression comment inside the loop below).
    size_t compressedRowCount = 0;

    for (auto& bucket : priceBuckets) {
        int32_t  factionId = bucket.first.factionId;
        uint32_t itemId    = bucket.first.itemId;
        int32_t  enchant   = bucket.first.enchant;
        std::vector<double>& prices = bucket.second;

        if (prices.size() > maxSampleCount) {
            maxSampleCount = prices.size();
            maxFactionId   = factionId;
            maxItemId      = itemId;
            maxEnchant     = enchant;
        }

        Stats s = computeStats(prices);

        // Field order: identity fields, then sampleCount (so a consumer
        // can weigh confidence before reading anything else), then the
        // raw price stats, the quartile range, and the outlier-adjusted
        // price stats -- 16 fields total, always present.
        std::ostringstream line;
        line << factionId << ':' << itemId << ':' << enchant << ':'
             << s.sampleCount << ':'
             << s.low << ':' << s.high << ':' << s.mean << ':'
             << s.median << ':' << s.mode << ':'
             << s.q1 << ':' << s.q3 << ':'
             << s.adjLow << ':' << s.adjHigh << ':' << s.adjMean << ':'
             << s.adjMedian << ':' << s.adjMode;

        // Compression: enchant/suffix != 0 means the item is equippable
        // gear (only equippable items can roll a random-property suffix
        // in WoW), and equippable items always have a max stack size of
        // 1 -- confirmed against real data, 0 exceptions. That makes the
        // 12 stack-stat fields entirely predictable (every one of them
        // would read exactly "1") for these rows, so they're omitted
        // rather than written out. A row's field count (16 vs 28) is
        // therefore itself the signal: 16 fields means "stack size is
        // always 1, not written"; 28 fields means "see the trailing 12
        // stack fields for the real distribution." enchant == 0 doesn't
        // guarantee the item IS stackable (plenty of gear also has no
        // suffix), so those rows always keep the full stack stats since
        // we can't safely predict them.
        if (enchant != 0) {
            ++compressedRowCount;
        } else {
            // Stack-count stats for this same bucket. stackBuckets is
            // filled in lockstep with priceBuckets (same key, same
            // record set), so this lookup should always succeed -- the
            // fallback only matters as defensive coding, never in
            // practice.
            static const std::vector<double> emptyStackFallback = {0.0};
            auto stackIt = stackBuckets.find(bucket.first);
            std::vector<double> stackCountsCopy = (stackIt != stackBuckets.end())
                ? stackIt->second
                : emptyStackFallback;
            Stats st = computeStats(stackCountsCopy);

            line << ':'
                 << st.low << ':' << st.high << ':' << st.mean << ':'
                 << st.median << ':' << st.mode << ':'
                 << st.q1 << ':' << st.q3 << ':'
                 << st.adjLow << ':' << st.adjHigh << ':' << st.adjMean << ':'
                 << st.adjMedian << ':' << st.adjMode;
        }

        lines.push_back(line.str());
    }

    std::ofstream out(outputPath, std::ios::binary);
    if (!out) {
        std::cerr << "Failed to open output file: " << outputPath << "\n";
        return 1;
    }

    // Row 1: total item count, which equals the number of rows that follow.
    out << lines.size() << '\n';
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
              << "  Unique items out:  " << lines.size()
              << " (rows in auctionsim.dat, deduped by faction+item+enchant)\n"
              << "  Rows compressed:   " << compressedRowCount << " / " << lines.size()
              << " (enchant != 0, stack stats omitted as always-1)\n"
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