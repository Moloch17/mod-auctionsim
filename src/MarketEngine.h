#pragma once
#include <cstddef>
#include <cstdint>
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
        // step's own posts don't move it). bots == 0 posts nothing.
        void PlanPosts(Faction const& fac, uint32 bots, Rng& rng, std::vector<PostOrder>& out) const;

        // One PostOrder's per-unit price and count, exposed for the self-tests.
        uint32 DrawUnitPrice(Faction const& fac, BasketRow const& row, Rng& rng) const;
        uint32 DrawCount(Faction const& fac, BasketRow const& row, Rng& rng) const;
        double CraftFloor(Faction const& fac, Item const& item) const;

        // Buyers per the contract's step 2: arrivals ~ Poisson(buyersH x demandScale x
        // scale x weekday multiplier x dt) per item, each taking the cheapest buyable
        // listing at or under its reservation. Appends the chosen listings' indices
        // (into Listings()) to `out` and returns how many buyers arrived.
        uint32 PlanBuys(Faction const& fac, double scaleTimesDt, size_t weekday, Rng& rng, std::vector<uint32>& out);

        // The house's per-item state, for the self-tests.
        size_t ItemCount() const { return _state.size(); }

    private:
        std::vector<Listing> _listings;
        std::vector<ItemState> _state;
        std::vector<uint32> _owners;  // scratch: one item's owners, for the distinct count

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
        std::vector<PostOrder> _orders;
        std::vector<uint32> _claims;
        std::vector<char> _gone;
    };
}
