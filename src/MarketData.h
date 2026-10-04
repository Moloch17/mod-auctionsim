#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>
#include "Define.h"

// Market mode's lookup tables, loaded once from auctionsim_market.dat. The format is
// ml/MARKET_FORMAT.md; ml/s6_market_sim.py is the reference runtime. Nothing in here
// touches the core: Parse() reads text, Resolve() asks an ItemFactsFn about the
// realm's items, so both run in the self-tests on an in-memory fixture.
namespace Market
{
    // --- Constants fixed by schema version 1 -------------------------------------
    constexpr size_t kQuantiles = 7;
    constexpr std::array<double, kQuantiles> kQuantileP = {0.02, 0.10, 0.25, 0.50, 0.75, 0.90, 0.98};

    // CURVE ratio bins (price / ref): 10 edges, 9 bins.
    constexpr size_t kCurveBins = 9;
    constexpr std::array<double, kCurveBins + 1> kRatioEdges = {0.25, 0.5, 0.7, 0.85, 1.0, 1.15, 1.4, 2.0, 3.0, 5.0};

    // POLICY cheapest bin: ln(cheapest / ref) against these edges -> 0..9.
    constexpr std::array<double, 9> kCheapestEdges = {-1.0, -0.5, -0.25, -0.1, 0.0, 0.1, 0.25, 0.5, 1.0};
    constexpr int32 kMaxUnitsBin = 7;
    constexpr int32 kMaxCheapestBin = 9;

    constexpr size_t kWeekdays = 7;

    // Both factions live in one file, keyed by AuctionHouseId (2 Alliance, 6 Horde).
    constexpr uint32 kAllianceHouse = 2;
    constexpr uint32 kHordeHouse = 6;
    constexpr size_t kFactions = 2;
    // 0 for Alliance, 1 for Horde, kFactions for anything else.
    size_t FactionSlot(uint32 houseId);
    uint32 FactionHouse(size_t slot);

    // --- Bin functions (the contract's "Policy context bins") --------------------
    // min(floor(log1p(units)), 7).
    int32 UnitsBin(uint64 units);
    // 0 -> 0, 1 -> 1, 2 -> 2, 3-4 -> 3, 5-8 -> 4, 9+ -> 5.
    int32 SellersBin(uint32 sellers);
    // -1 when nothing of the item is up with a buyout. Otherwise the number of
    // kCheapestEdges <= ln(cheapest / ref): below -1.0 -> 0, exactly -1.0 -> 1, ...,
    // >= 1.0 -> 9 (upper_bound semantics, the same as numpy.digitize).
    int32 CheapestBin(bool hasCheapest, double cheapest, double ref);

    using Quantiles = std::array<float, kQuantiles>;

    // Draws from 7 stored quantiles: u below 0.02 or above 0.98 gives the end value,
    // otherwise linear interpolation between the two stored p around u.
    double QuantileDraw(Quantiles const& q, double u);

    // CURVE row: w[b] = share of buyers paying at least kRatioEdges[b] x ref.
    using Curve = std::array<float, kCurveBins>;

    // The bin masses w[b] - w[b+1] (w[9] = 0, negatives clipped to 0), normalised and
    // accumulated: what ReservationRatio samples from. Parse stores curves this way.
    Curve CumulativeCurve(Curve const& w);

    // Reservation as a ratio to ref, as ml/s6_market_sim.py draws it: bin b = the number
    // of cumulative masses below uBin (at most 8), then uniform in
    // [kRatioEdges[b], kRatioEdges[b+1]) by uIn. Always in [0.25, 5.0).
    double ReservationRatio(Curve const& cumulative, double uBin, double uIn);

    // --- Tables ----------------------------------------------------------------------
    struct Item
    {
        uint32 itemId = 0;
        int32 itemClass = -1;
        float ref = 0.0f;       // reference per-unit price
        uint32 conv = 1;        // conventional stack
        uint32 maxc = 1;        // largest stack posted
        uint32 vendor = 0;      // per-unit sell-to-vendor price: posts never go below it
        float buyersH = 0.0f;   // buyer arrivals per hour on Lordaeron
        bool listed = false;    // false: a CRAFT reagent with no ITEM row, tracked for its price only
        int32 curve = -1;       // index into Faction::curves, -1 = no buyers
        int32 weekday = -1;     // index into Faction::weekdays, -1 = flat
        uint32 craftBegin = 0;  // [craftBegin, craftEnd) in Faction::craft
        uint32 craftEnd = 0;
        float craftMargin = 0.0f;
        uint32 vendorBuyGuard = 0;  // vendor price of one of a vendor-stocked item, 0 = none
    };

    struct Reagent
    {
        uint32 itemIdx = 0;  // into Faction::items
        float qty = 0.0f;
    };

    struct PolicyRow
    {
        Quantiles q{};
        float offset = 0.0f;
    };

    struct BotName
    {
        uint32 index = 0;
        std::string name;
        int32 type = -1;
    };

    struct BasketRow
    {
        uint32 bot = 0;      // posted by bot (bot mod N)
        uint32 itemIdx = 0;  // into Faction::items
        int32 type = -1;     // seller type for POLICY / STACK
        int32 stack = -1;    // resolved STACK row
        float rateH = 0.0f;
        float tl4 = 0.0f;
        float batch = 1.0f;
        // Cached per step length / scale by Data::SetRates: events ~ Poisson(lambda).
        float lambda = 0.0f;
        float expNegLambda = 1.0f;
        float expNegBatch = 1.0f;  // exp(-(batch - 1)), extra listings per event
    };

    struct Faction
    {
        bool present = false;
        float demandScale = 1.0f;
        float supplyScale = 1.0f;
        uint32 refListings = 0;
        uint32 refSellers = 0;

        std::vector<Item> items;
        std::unordered_map<uint32, uint32> itemIndex;  // itemId -> index into items
        std::vector<Reagent> craft;
        std::vector<Curve> curves;  // cumulative (CumulativeCurve), not the file's shares
        std::vector<std::array<float, kWeekdays>> weekdays;
        std::vector<PolicyRow> policies;
        std::unordered_map<uint64, uint32> policyIndex;  // PolicyKey -> index into policies
        std::vector<Quantiles> stacks;
        std::unordered_map<uint64, uint32> stackIndex;  // StackKey -> index into stacks
        std::vector<BotName> bots;                      // by index
        std::vector<BasketRow> basket;

        int32 FindItem(uint32 itemId) const;

        // The contract's fallback chain: (type, class, ub, sb, mb), (type, class, -1, -1, mb),
        // (type, -1, -1, -1, mb), (-1, -1, -1, -1, mb). Null only if the file lacks the last
        // level, which Parse refuses.
        PolicyRow const* FindPolicy(int32 type, int32 itemClass, int32 ub, int32 sb, int32 mb) const;

        // (type, class), (type, -1), (-1, -1); -1 if none.
        int32 FindStack(int32 type, int32 itemClass) const;
    };

    // Packs a POLICY / STACK key; every part may be -1.
    uint64 PolicyKey(int32 type, int32 itemClass, int32 ub, int32 sb, int32 mb);
    uint64 StackKey(int32 type, int32 itemClass);

    // What Resolve needs to know about one item on this realm.
    struct ItemFacts
    {
        bool exists = false;
        bool postable = true;  // false when over AuctionSim.MaxRequiredLevel / MaxItemLevel
        uint32 sellPrice = 0;
        uint32 maxStack = 1;
        uint32 vendorBuyGuard = 0;  // vendor price of one when a vendor stocks it, else 0
    };
    using ItemFactsFn = std::function<ItemFacts(uint32 itemId)>;

    struct LoadStats
    {
        size_t rows = 0;
        size_t skippedRows = 0;  // malformed rows, logged and skipped
        size_t droppedItems = 0;  // not in this realm's item_template
        size_t droppedBasket = 0;  // basket rows whose item can't be posted here
    };

    class Data
    {
    public:
        std::array<Faction, kFactions> factions;
        std::unordered_map<int32, std::string> classNames;
        uint32 foundVersion = 0;  // from line 1; 0 if the file had no stamp
        LoadStats stats;

        // Reads a whole market file. False (with `error` set) on a wrong schema stamp,
        // a truncated section, a duplicate section, or missing fallback rows; a single
        // malformed row is skipped and counted. Cross-references (CURVE/WEEKDAY per item,
        // CRAFT reagents, BASKET items, STACK rows) are linked before returning.
        bool Parse(std::istream& in, std::string& error);

        // Applies this realm's item_template: drops items it doesn't have, raises the
        // vendor floor to the realm's SellPrice, caps maxc at the item's max stack,
        // drops basket rows for unpostable items and records the vendor buy guard.
        void Resolve(ItemFactsFn const& facts);

        // Recomputes each basket row's Poisson rate for a step of dtHours at this scale:
        // rateH x supplyScale x scale x dt.
        void SetRates(double scale, double dtHours);

        // Approximate heap held by the tables, for the load log.
        size_t MemoryBytes() const;
    };
}
