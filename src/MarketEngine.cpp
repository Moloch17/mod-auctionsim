#include "MarketEngine.h"
#include <algorithm>
#include <cmath>

namespace Market
{
    namespace
    {
        // Largest buyout the client and core handle (Player.h's MAX_MONEY_AMOUNT).
        constexpr double kMaxPrice = 2147483646.0;

        uint32 RoundPrice(double unit)
        {
            if (!(unit >= 1.0))  // also catches NaN
            {
                return 1;
            }
            return static_cast<uint32>(std::min(std::floor(unit + 0.5), kMaxPrice));
        }
    }

    void Engine::BuildState(size_t itemCount)
    {
        std::sort(_listings.begin(), _listings.end(), [](Listing const& a, Listing const& b) {
            return a.itemIdx != b.itemIdx ? a.itemIdx < b.itemIdx : a.perUnit < b.perUnit;
        });
        _state.assign(itemCount, ItemState{});

        size_t const n = _listings.size();
        size_t i = 0;
        while (i < n)
        {
            uint32 const itemIdx = _listings[i].itemIdx;
            size_t j = i;
            uint64 units = 0;
            _owners.clear();
            while (j < n && _listings[j].itemIdx == itemIdx)
            {
                units += _listings[j].count;
                _owners.push_back(_listings[j].owner);
                ++j;
            }
            if (itemIdx < itemCount)
            {
                std::sort(_owners.begin(), _owners.end());
                ItemState& state = _state[itemIdx];
                state.cheapest = _listings[i].perUnit;  // sorted: the first is the cheapest (kNoBuyout if none)
                state.units = static_cast<uint32>(std::min<uint64>(units, UINT32_MAX));
                state.sellers = static_cast<uint32>(std::unique(_owners.begin(), _owners.end()) - _owners.begin());
                state.begin = static_cast<uint32>(i);
                state.end = static_cast<uint32>(j);
            }
            i = j;
        }
    }

    double Engine::CraftFloor(Faction const& fac, Item const& item) const
    {
        if (item.craftBegin == item.craftEnd)
        {
            return 0.0;
        }
        double sum = 0.0;
        for (uint32 r = item.craftBegin; r < item.craftEnd; ++r)
        {
            Reagent const& reagent = fac.craft[r];
            ItemState const& state = _state[reagent.itemIdx];
            double price = state.cheapest != Listing::kNoBuyout
                ? static_cast<double>(state.cheapest)
                : static_cast<double>(fac.items[reagent.itemIdx].ref);
            sum += static_cast<double>(reagent.qty) * price;
        }
        return static_cast<double>(item.craftMargin) * sum;
    }

    uint32 Engine::DrawUnitPrice(Faction const& fac, BasketRow const& row, Rng& rng) const
    {
        Item const& item = fac.items[row.itemIdx];
        ItemState const& state = _state[row.itemIdx];
        bool const hasCheapest = state.cheapest != Listing::kNoBuyout;
        double const cheapest = static_cast<double>(state.cheapest);
        double const ref = static_cast<double>(item.ref);
        int32 const mb = CheapestBin(hasCheapest, cheapest, ref);

        PolicyRow const* policy = fac.FindPolicy(
            row.type, item.itemClass, UnitsBin(state.units), SellersBin(state.sellers), mb);
        double unit = mb >= 0 ? cheapest : ref;
        if (policy)
        {
            unit *= std::exp(QuantileDraw(policy->q, rng.Uniform()) + static_cast<double>(policy->offset));
        }
        unit = std::max({unit, static_cast<double>(item.vendor), CraftFloor(fac, item)});
        return RoundPrice(unit);
    }

    uint32 Engine::DrawCount(Faction const& fac, BasketRow const& row, Rng& rng) const
    {
        Item const& item = fac.items[row.itemIdx];
        double draw = row.stack >= 0 ? QuantileDraw(fac.stacks[row.stack], rng.Uniform()) : 0.0;
        // nearbyint: round half to even, as the reference's Python round() does.
        double count = std::nearbyint(static_cast<double>(item.conv) * std::exp(draw));
        count = std::clamp(count, 1.0, static_cast<double>(item.maxc));
        return static_cast<uint32>(count);
    }

    void Engine::PlanPosts(Faction const& fac, uint32 bots, Rng& rng, std::vector<PostOrder>& out) const
    {
        if (bots == 0)
        {
            return;
        }
        for (BasketRow const& row : fac.basket)
        {
            uint32 events = rng.Poisson(row.lambda, row.expNegLambda);
            for (uint32 e = 0; e < events; ++e)
            {
                PostOrder order;
                order.itemIdx = row.itemIdx;
                order.botSlot = row.bot % bots;
                order.listings = 1 + rng.Poisson(static_cast<double>(row.batch) - 1.0, row.expNegBatch);
                order.count = DrawCount(fac, row, rng);
                order.unitPrice = DrawUnitPrice(fac, row, rng);
                if (rng.Uniform() < static_cast<double>(row.tl4))
                {
                    order.hours = rng.Uniform() < 0.5 ? 24 : 48;
                }
                else
                {
                    order.hours = 12;
                }
                out.push_back(order);
            }
        }
    }

    uint32 Engine::PlanBuys(Faction const& fac, double scaleTimesDt, size_t weekday, Rng& rng, std::vector<uint32>& out)
    {
        size_t const itemCount = fac.items.size();
        if (_rateFaction != &fac || _rateScale != scaleTimesDt || _rateWeekday != weekday ||
            _buyerLambda.size() != itemCount)
        {
            _buyerLambda.assign(itemCount, 0.0f);
            _buyerExp.assign(itemCount, 1.0f);
            for (size_t i = 0; i < itemCount; ++i)
            {
                Item const& item = fac.items[i];
                if (item.curve < 0 || item.buyersH <= 0.0f)
                {
                    continue;
                }
                double multiplier = item.weekday >= 0 ? static_cast<double>(fac.weekdays[item.weekday][weekday]) : 1.0;
                double lambda = static_cast<double>(item.buyersH) * static_cast<double>(fac.demandScale) *
                                scaleTimesDt * multiplier;
                _buyerLambda[i] = static_cast<float>(lambda);
                _buyerExp[i] = static_cast<float>(std::exp(-lambda));
            }
            _rateFaction = &fac;
            _rateScale = scaleTimesDt;
            _rateWeekday = weekday;
        }

        uint32 buyers = 0;
        size_t const stateCount = std::min(itemCount, _state.size());
        for (size_t i = 0; i < stateCount; ++i)
        {
            if (_buyerLambda[i] <= 0.0f)
            {
                continue;
            }
            uint32 arrivals = rng.Poisson(_buyerLambda[i], _buyerExp[i]);
            if (arrivals == 0)
            {
                continue;
            }
            buyers += arrivals;

            Item const& item = fac.items[i];
            Curve const& curve = fac.curves[item.curve];
            ItemState const& state = _state[i];
            uint32 next = state.begin;
            for (uint32 a = 0; a < arrivals; ++a)
            {
                double reservation =
                    ReservationRatio(curve, rng.Uniform(), rng.Uniform()) * static_cast<double>(item.ref);
                while (next < state.end && !(_listings[next].flags & Listing::kBuyable))
                {
                    ++next;
                }
                if (next < state.end && static_cast<double>(_listings[next].perUnit) <= reservation)
                {
                    out.push_back(next);
                    ++next;
                }
            }
        }
        return buyers;
    }

    void FillRun::Begin(
        Faction const& fac,
        std::vector<Listing> const& house,
        std::vector<uint32> const& slotOwners,
        std::vector<uint8> const& weekdays,
        double scale,
        double dtHours,
        uint64 endClock)
    {
        _fac = &fac;
        _static.assign(house.begin(), house.end());
        _botUp.assign(fac.items.size(), 0);
        for (Listing& listing : _static)
        {
            listing.flags &= static_cast<uint8>(~Listing::kBuyable);  // competitors only
            if ((listing.flags & Listing::kBotOwned) && listing.itemIdx < _botUp.size())
            {
                _botUp[listing.itemIdx]++;
            }
        }
        _virtual.clear();
        _slotOwners = slotOwners;
        _weekdays = weekdays;
        _scale = scale;
        _dtHours = dtHours;
        _steps = static_cast<uint32>(static_cast<double>(kFillHours) / dtHours + 0.5);
        _endClock = endClock;
        _startClock = endClock - static_cast<uint64>(kFillHours) * 3600;
        _step = 0;
        _posted = 0;
        _sold = 0;
        _nextId = 0;
    }

    bool FillRun::Advance(uint32 steps, Rng& rng)
    {
        if (!_fac)
        {
            return true;
        }
        Faction const& fac = *_fac;
        uint32 const bots = static_cast<uint32>(_slotOwners.size());
        for (uint32 n = 0; n < steps && _step < _steps; ++n, ++_step)
        {
            uint64 const clock = _startClock + static_cast<uint64>(static_cast<double>(_step) * _dtHours * 3600.0);

            // Expire (end > now survives), then the house as it stands this step.
            _virtual.erase(std::remove_if(_virtual.begin(), _virtual.end(),
                               [clock](Listing const& l) { return l.expire <= clock; }),
                _virtual.end());
            std::vector<Listing>& listings = _engine.Listings();
            listings.assign(_static.begin(), _static.end());
            listings.insert(listings.end(), _virtual.begin(), _virtual.end());
            _engine.BuildState(fac.items.size());

            _orders.clear();
            _engine.PlanPosts(fac, bots, rng, _orders);
            for (PostOrder const& order : _orders)
            {
                for (uint32 k = 0; k < order.listings; ++k)
                {
                    Listing listing;
                    listing.itemIdx = order.itemIdx;
                    listing.perUnit = order.unitPrice;
                    listing.count = order.count;
                    listing.owner = _slotOwners[order.botSlot];
                    listing.auctionId = ++_nextId;
                    listing.expire = static_cast<uint32>(clock + uint64(order.hours) * 3600);
                    listing.flags = Listing::kBuyable | Listing::kBotOwned;
                    listings.push_back(listing);
                    ++_posted;
                }
            }
            _engine.BuildState(fac.items.size());

            _claims.clear();
            size_t const weekday = _step < _weekdays.size() ? _weekdays[_step] : 0;
            _engine.PlanBuys(fac, _scale * _dtHours, weekday, rng, _claims);
            _sold += _claims.size();

            // The virtual listings left: buyable ones only (statics never are).
            _gone.assign(listings.size(), 0);
            for (uint32 index : _claims)
            {
                _gone[index] = 1;
            }
            _virtual.clear();
            for (size_t i = 0; i < listings.size(); ++i)
            {
                if (!_gone[i] && (listings[i].flags & Listing::kBuyable))
                {
                    _virtual.push_back(listings[i]);
                }
            }
        }
        return Done();
    }

    void FillRun::Result(std::vector<Listing>& out) const
    {
        out.clear();
        for (Listing const& listing : _virtual)
        {
            if (listing.expire > _endClock)
            {
                out.push_back(listing);
            }
        }
        // Per item, latest expiry first; keep all but the first botUp[item] from the end.
        std::sort(out.begin(), out.end(), [](Listing const& a, Listing const& b) {
            return a.itemIdx != b.itemIdx ? a.itemIdx < b.itemIdx : a.expire > b.expire;
        });
        size_t write = 0;
        size_t i = 0;
        while (i < out.size())
        {
            size_t j = i;
            while (j < out.size() && out[j].itemIdx == out[i].itemIdx)
            {
                ++j;
            }
            size_t const have = out[i].itemIdx < _botUp.size() ? _botUp[out[i].itemIdx] : 0;
            size_t const keep = (j - i) > have ? (j - i) - have : 0;
            for (size_t k = i; k < i + keep; ++k)
            {
                out[write++] = out[k];
            }
            i = j;
        }
        out.resize(write);
    }
}
