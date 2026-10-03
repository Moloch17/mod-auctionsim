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
}
