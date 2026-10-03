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
        static constexpr uint8 kBuyable = 0x1;  // a market buyer may take it (see MarketService)

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
}
