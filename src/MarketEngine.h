#pragma once
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>
#include <vector>
#include "Define.h"
#include "MarketData.h"
#include "MarketRng.h"

namespace Market
{
    // One auction as the market step sees it. Filled by the caller in one pass over a
    // house (or by the self-tests from synthetic data).
    struct Listing
    {
        static constexpr uint32 kNoBuyout = UINT32_MAX;
        static constexpr uint8 kBuyable = 0x1;   // a market buyer may take it (see MarketService)
        static constexpr uint8 kBotOwned = 0x2;  // a market seller's listing (a fill counts these per item)

        uint32 itemIdx = 0;  // into Faction::items
        uint32 perUnit = kNoBuyout;
        uint32 count = 1;
        uint32 owner = 0;  // owner's low guid
        uint32 auctionId = 0;
        uint32 expire = 0;  // unix seconds
        uint8 flags = 0;
    };

    // The house's state for one item, from every listing of it (all owners).
    struct ItemState
    {
        uint32 cheapest = Listing::kNoBuyout;  // per unit, among buyout listings
        uint32 units = 0;
        uint32 sellers = 0;  // distinct owners
        uint32 cheapestSeller = Listing::kNoBuyout;  // per unit, among market sellers' buyout listings
        uint32 begin = 0;    // [begin, end) in the sorted listings, cheapest first
        uint32 end = 0;
    };

    // One posting event: `listings` identical auctions of `count` units at `unitPrice`.
    struct PostOrder
    {
        uint32 itemIdx = 0;
        uint32 botSlot = 0;  // basket row's bot mod bots-in-use
        uint32 count = 1;
        uint32 unitPrice = 1;
        uint32 hours = 12;
        uint32 listings = 1;
        uint32 expireAt = 0;  // unix seconds; 0 = now + hours (a fill sets it to the survivor's expiry)
    };

    // MARKET_FORMAT.md's price memory: a post can blend its fresh draw with the price
    // the same seller last posted the item at (`weight` on the remembered one).
    // kMemoryWeight = 0 in schema 1: memory is OFF (any memory flattened the market
    // against Lordaeron; the 0.05-0.95 draw trim alone gives real same-seller spread).
    // At weight 0 nothing touches a memory: no lookups, no own-listing fallback, no
    // writes, no pruning, no reserved capacity. Raise the constant to turn it back on.
    constexpr uint32 kMemorySeconds = 72 * 3600;
    constexpr double kMemoryWeight = 0.0;

    // exp(weight * ln remembered + (1 - weight) * ln fresh); `fresh` when nothing is
    // remembered (remembered <= 0) or the weight is 0.
    double BlendWithMemory(double fresh, double remembered, double weight);

    // Per house, (seller owner id, item index) -> that seller's last posted per-unit
    // price and when. One hash map with reserved capacity; only pairs that post get an
    // entry (tens of thousands at Lordaeron scale, ~16 bytes of payload each). Runtime
    // only: lost on restart, where the own-listing fallback takes over.
    class PriceMemory
    {
    public:
        struct Entry
        {
            uint32 unitPrice = 0;
            uint32 time = 0;  // unix seconds
        };

        void Reserve(size_t entries) { _entries.reserve(entries); }
        void Clear() { _entries.clear(); }
        size_t Size() const { return _entries.size(); }

        // The remembered price if it is younger than kMemorySeconds at `now`, else 0.
        uint32 Recent(uint32 owner, uint32 itemIdx, uint64 now) const;
        void Store(uint32 owner, uint32 itemIdx, uint32 unitPrice, uint64 now);
        // Drops entries older than kMemorySeconds; returns how many. Run about daily.
        size_t Prune(uint64 now);

    private:
        static uint64 Key(uint32 owner, uint32 itemIdx) { return (uint64(owner) << 32) | itemIdx; }
        std::unordered_map<uint64, Entry> _entries;
    };

    class Engine;

    // MARKET_FORMAT.md's player buyer: every player listing gets its own chance to sell
    // each step, set by the realm's settings rather than Market.Scale.
    struct PlayerBuyerParams
    {
        double sellHours = 24.0;  // 0 = off
        double liquidity = 0.5;   // exponent on buyersH / kLiquidBuyersH
        bool qualityBonus = false;
    };
    constexpr double kLiquidBuyersH = 0.5;

    // Demand multiplier by item quality with the quality bonus on; 0 for grey.
    double PlayerQualityFactor(uint8 quality);
    // L = min(1, (b / kLiquidBuyersH)^liquidity), b = buyersH x the quality factor when
    // the bonus is on. 0 for a grey item (never bought).
    double PlayerLiquidityFactor(double buyersH, uint8 quality, PlayerBuyerParams const& params);
    // F = 1 at or below the market price m; above it w(price / m) / w(1) on the item's
    // CURVE shares, clamped to [0, 1] (0 when w(1) is 0).
    double PlayerPriceFactor(Curve const& shares, double unitPrice, double marketPrice);
    // The market price m = min(ref, cheapest market-seller listing per unit up).
    double PlayerMarketPrice(double ref, uint32 cheapestSeller);
    // The chance a listing is bought this step: 1 - exp(-h0 x L x F x dt), h0 = -ln(0.05) / sellHours.
    double PlayerBuyChance(double sellHours, double liquidity, double priceFactor, double dtHours);

    // The lowest market-seller price per unit seen for each item over the last 24 h, as
    // two 12 h buckets (current and previous): the player buyer's market price uses it,
    // so buying out the cheapest seller copy doesn't raise the price a flipper is paid.
    // Two floats per item per house.
    class SellerLow
    {
    public:
        static constexpr uint64 kBucketSeconds = 12 * 3600;

        // Folds each item's cheapest seller listing (Engine::State().cheapestSeller) at
        // `now` into the current bucket, starting a new bucket first when one is due.
        void Fold(Engine const& engine, size_t itemCount, uint64 now);
        // Lowest seen over the two buckets, or Listing::kNoBuyout if none.
        uint32 Low(uint32 itemIdx) const;

    private:
        std::vector<float> _current;
        std::vector<float> _previous;
        uint64 _bucket = 0;
        bool _started = false;
    };

    // What the player buyer paid each character over a rolling window (24 h), for
    // AuctionSim.Market.PlayerGoldPerDay. A deque of payments per owner, pruned as it
    // is touched; owners with nothing in the window are dropped.
    class GoldLedger
    {
    public:
        static constexpr uint64 kWindowSeconds = 24 * 3600;

        // Records the payment and returns true when it keeps the owner's window total
        // within limitCopper (0 = no limit); otherwise records nothing and returns false.
        bool TryCharge(uint32 owner, uint64 copper, uint64 now, uint64 limitCopper);
        uint64 PaidWithin(uint32 owner, uint64 now);
        size_t Owners() const { return _payments.size(); }
        // Drops every owner whose payments have all left the window.
        void Prune(uint64 now);

    private:
        struct Payment
        {
            uint64 time;
            uint64 copper;
        };
        std::unordered_map<uint32, std::deque<Payment>> _payments;
    };

    // The market step's arithmetic, with no core dependency so the self-tests can run
    // and time it on synthetic houses. All buffers are members and only ever cleared,
    // so a running realm allocates nothing per step once they have grown.
    class Engine
    {
    public:
        std::vector<Listing>& Listings() { return _listings; }
        std::vector<Listing> const& Listings() const { return _listings; }

        // Sorts the listings by (item, per-unit buyout) and derives every item's state.
        // Call after filling Listings(), and again after appending new posts.
        void BuildState(size_t itemCount);

        ItemState const& State(uint32 itemIdx) const { return _state[itemIdx]; }

        // Posts per the contract's step 1, priced from the state BuildState saw (this
        // step's own posts don't move it). Bot slot i posts as owner slotOwners[i]; no
        // slots posts nothing. `memory` is the house's price memory at time `now`.
        void PlanPosts(
            Faction const& fac,
            std::vector<uint32> const& slotOwners,
            PriceMemory& memory,
            uint64 now,
            Rng& rng,
            std::vector<PostOrder>& out) const;

        // Price memory weight for this engine's posts (default kMemoryWeight, i.e. off).
        // The self-tests set it to exercise the mechanism.
        void SetMemoryWeight(double weight) { _memoryWeight = weight; }
        double MemoryWeight() const { return _memoryWeight; }

        // One post's per-unit price: the POLICY draw -- with a memory weight above 0,
        // blended with what `owner` remembers (memory, else its cheapest own listing of
        // the item up) -- then the vendor and CRAFT floors; with a weight above 0 the
        // result is stored back into `memory`. Exposed for the self-tests.
        uint32 DrawUnitPrice(
            Faction const& fac, BasketRow const& row, uint32 owner, PriceMemory& memory, uint64 now, Rng& rng) const;
        // What `owner` remembers for the item: memory younger than kMemorySeconds, else
        // its cheapest own buyout listing of the item in the state BuildState saw; 0 = none.
        uint32 RememberedPrice(uint32 itemIdx, uint32 owner, PriceMemory const& memory, uint64 now) const;
        uint32 DrawCount(Faction const& fac, BasketRow const& row, Rng& rng) const;
        double CraftFloor(Faction const& fac, Item const& item) const;

        // Buyers per the contract's step 2: arrivals ~ Poisson(buyersH x demandScale x
        // scale x weekday multiplier x dt) per item, each taking the cheapest buyable
        // listing at or under its reservation. Appends the chosen listings' indices
        // (into Listings()) to `out` and returns how many buyers arrived.
        uint32 PlanBuys(Faction const& fac, double scaleTimesDt, size_t weekday, Rng& rng, std::vector<uint32>& out);

        // The player buyer, after the regular buyers: rolls every buyable player listing
        // (kBuyable, not kBotOwned, not already in `taken`, quality > 0) and appends the
        // ones bought this step to `out` (indices into Listings()). The market price is
        // min(ref, sellerLow's 24 h low).
        void PlanPlayerBuys(
            Faction const& fac,
            PlayerBuyerParams const& params,
            SellerLow const& sellerLow,
            double dtHours,
            std::vector<uint32> const& taken,
            Rng& rng,
            std::vector<uint32>& out);

        // The house's per-item state, for the self-tests.
        size_t ItemCount() const { return _state.size(); }

    private:
        double _memoryWeight = kMemoryWeight;
        std::vector<Listing> _listings;
        std::vector<ItemState> _state;
        std::vector<uint32> _owners;  // scratch: one item's owners, for the distinct count
        std::vector<char> _taken;     // scratch: listings the regular buyers already took

        // Per-item buyer rate, rebuilt only when the scale/dt or weekday changes.
        std::vector<float> _buyerLambda;
        std::vector<float> _buyerExp;
        Faction const* _rateFaction = nullptr;
        double _rateScale = -1.0;
        size_t _rateWeekday = SIZE_MAX;
    };

    // MARKET_FORMAT.md's Fill: the runtime over the kFillHours before now on virtual
    // bot listings only, with the real house as static competitors that are never
    // bought. Runs a few steps at a time (Advance) so a fill never stalls one tick.
    class FillRun
    {
    public:
        static constexpr uint32 kFillHours = 48;

        // `house`: every real listing of the faction's items (flags: kBotOwned on the
        // market sellers' own). `slotOwners[i]`: owner id of bot slot i (virtual
        // listings' owner, so distinct-seller counts merge with the real sellers').
        // `weekdays[k]`: WEEKDAY column for simulated step k. Steps end at `endClock`.
        void Begin(
            Faction const& fac,
            std::vector<Listing> const& house,
            std::vector<uint32> const& slotOwners,
            std::vector<uint8> const& weekdays,
            double scale,
            double dtHours,
            uint64 endClock);

        // Runs up to `steps` simulated steps; true once all are done.
        bool Advance(uint32 steps, Rng& rng);
        bool Done() const { return _step >= _steps; }
        uint32 StepsDone() const { return _step; }
        uint32 StepsTotal() const { return _steps; }
        uint64 Posted() const { return _posted; }
        uint64 Sold() const { return _sold; }

        // After Done(), MARKET_FORMAT.md's Fill step 3: of the S survivors still up at
        // endClock, add at most S - E (E = the sellers' listings already up): per item
        // the excess of survivors over the sellers' own (its earliest-expiring survivors
        // dropped), items taken in random order until S - E are chosen (the last item
        // partly, latest-expiring first). `expire` is absolute; owner is the virtual
        // poster's owner id.
        void Result(std::vector<Listing>& out, Rng& rng) const;
        uint64 Survivors() const;    // S
        uint64 SellersUp() const;    // E

    private:
        Faction const* _fac = nullptr;
        std::vector<Listing> _static;
        std::vector<Listing> _virtual;
        std::vector<uint32> _slotOwners;
        std::vector<uint8> _weekdays;
        std::vector<uint32> _botUp;  // per item: the sellers' real listings up
        double _scale = 0.0;
        double _dtHours = 0.5;
        uint64 _startClock = 0;
        uint64 _endClock = 0;
        uint32 _steps = 0;
        uint32 _step = 0;
        uint64 _posted = 0;
        uint64 _sold = 0;
        uint32 _nextId = 0;
        Engine _engine;
        PriceMemory _memory;  // the fill's own, seeded by the sellers' listings already up
        std::vector<PostOrder> _orders;
        std::vector<uint32> _claims;
        std::vector<char> _gone;
    };
}
