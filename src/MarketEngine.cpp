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
            // A reagent is worth at most its reference: valuing reagents above it let
            // recipe cycles ratchet each other's floors up without limit.
            double price = static_cast<double>(fac.items[reagent.itemIdx].ref);
            if (state.cheapest != Listing::kNoBuyout)
            {
                price = std::min(price, static_cast<double>(state.cheapest));
            }
            sum += static_cast<double>(reagent.qty) * price;
        }
        return std::min(static_cast<double>(item.craftMargin) * sum, kCeiling * static_cast<double>(item.ref));
    }

    double BlendWithMemory(double fresh, double remembered, double weight)
    {
        if (!(weight > 0.0) || !(remembered > 0.0) || !(fresh > 0.0))
        {
            return fresh;
        }
        return std::exp(weight * std::log(remembered) + (1.0 - weight) * std::log(fresh));
    }

    uint32 PriceMemory::Recent(uint32 owner, uint32 itemIdx, uint64 now) const
    {
        auto it = _entries.find(Key(owner, itemIdx));
        if (it == _entries.end() || now >= uint64(it->second.time) + kMemorySeconds)
        {
            return 0;
        }
        return it->second.unitPrice;
    }

    void PriceMemory::Store(uint32 owner, uint32 itemIdx, uint32 unitPrice, uint64 now)
    {
        _entries[Key(owner, itemIdx)] = {unitPrice, static_cast<uint32>(now)};
    }

    size_t PriceMemory::Prune(uint64 now)
    {
        size_t dropped = 0;
        for (auto it = _entries.begin(); it != _entries.end();)
        {
            if (now >= uint64(it->second.time) + kMemorySeconds)
            {
                it = _entries.erase(it);
                ++dropped;
            }
            else
            {
                ++it;
            }
        }
        return dropped;
    }

    uint32 Engine::RememberedPrice(uint32 itemIdx, uint32 owner, PriceMemory const& memory, uint64 now) const
    {
        if (uint32 recent = memory.Recent(owner, itemIdx, now))
        {
            return recent;
        }
        if (itemIdx >= _state.size())
        {
            return 0;
        }
        // The item's listings are sorted cheapest first: the owner's first is its cheapest.
        ItemState const& state = _state[itemIdx];
        for (uint32 i = state.begin; i < state.end; ++i)
        {
            Listing const& listing = _listings[i];
            if (listing.owner == owner && listing.perUnit != Listing::kNoBuyout)
            {
                return listing.perUnit;
            }
        }
        return 0;
    }

    uint32 Engine::DrawUnitPrice(
        Faction const& fac, BasketRow const& row, uint32 owner, PriceMemory& memory, uint64 now, Rng& rng) const
    {
        Item const& item = fac.items[row.itemIdx];
        ItemState const& state = _state[row.itemIdx];
        double const cheapest = static_cast<double>(state.cheapest);
        double const ref = static_cast<double>(item.ref);
        // Reference anchor: a cheapest above kAnchor x ref counts as nothing up.
        bool const hasCheapest = state.cheapest != Listing::kNoBuyout && cheapest <= kAnchor * ref;
        int32 const mb = CheapestBin(hasCheapest, cheapest, ref);

        PolicyRow const* policy = fac.FindPolicy(
            row.type, item.itemClass, UnitsBin(state.units), SellersBin(state.sellers), mb);
        double unit = mb >= 0 ? cheapest : ref;
        if (policy)
        {
            unit *= std::exp(QuantileDraw(policy->q, DrawU(rng.Uniform())) + static_cast<double>(policy->offset));
        }
        unit = std::min(unit, kCeiling * ref);
        bool const remembers = _memoryWeight > 0.0;
        if (remembers)
        {
            double const remembered = static_cast<double>(RememberedPrice(row.itemIdx, owner, memory, now));
            unit = BlendWithMemory(unit, remembered, _memoryWeight);
        }
        // The vendor floor is never capped; the CRAFT floor is (in CraftFloor).
        unit = std::max({unit, static_cast<double>(item.vendor), CraftFloor(fac, item)});
        uint32 const price = RoundPrice(unit);
        if (remembers)
        {
            memory.Store(owner, row.itemIdx, price, now);
        }
        return price;
    }

    uint32 Engine::DrawCount(Faction const& fac, BasketRow const& row, Rng& rng) const
    {
        Item const& item = fac.items[row.itemIdx];
        double draw = row.stack >= 0 ? QuantileDraw(fac.stacks[row.stack], DrawU(rng.Uniform())) : 0.0;
        // nearbyint: round half to even, as the reference's Python round() does.
        double count = std::nearbyint(static_cast<double>(item.conv) * std::exp(draw));
        count = std::clamp(count, 1.0, static_cast<double>(item.maxc));
        return static_cast<uint32>(count);
    }

    void Engine::PlanPosts(
        Faction const& fac,
        std::vector<uint32> const& slotOwners,
        PriceMemory& memory,
        uint64 now,
        Rng& rng,
        std::vector<PostOrder>& out) const
    {
        uint32 const bots = static_cast<uint32>(slotOwners.size());
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
                order.unitPrice = DrawUnitPrice(fac, row, slotOwners[order.botSlot], memory, now, rng);
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
        // An empty memory: the own-listing fallback starts it from the sellers' listings up.
        _memory.Clear();
        if (_engine.MemoryWeight() > 0.0)
        {
            _memory.Reserve(fac.basket.size());
        }
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
            _engine.PlanPosts(fac, _slotOwners, _memory, clock, rng, _orders);
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

    uint64 FillRun::Survivors() const
    {
        return static_cast<uint64>(std::count_if(_virtual.begin(), _virtual.end(),
            [this](Listing const& listing) { return listing.expire > _endClock; }));
    }

    uint64 FillRun::SellersUp() const
    {
        uint64 total = 0;
        for (uint32 up : _botUp)
        {
            total += up;
        }
        return total;
    }

    void FillRun::Result(std::vector<Listing>& out, Rng& rng) const
    {
        out.clear();
        for (Listing const& listing : _virtual)
        {
            if (listing.expire > _endClock)
            {
                out.push_back(listing);
            }
        }
        uint64 const survivors = out.size();
        uint64 const sellersUp = SellersUp();
        uint64 budget = survivors > sellersUp ? survivors - sellersUp : 0;

        // Per item, latest expiry first, so an item's excess is its first entries.
        std::sort(out.begin(), out.end(), [](Listing const& a, Listing const& b) {
            return a.itemIdx != b.itemIdx ? a.itemIdx < b.itemIdx : a.expire > b.expire;
        });
        struct Excess
        {
            size_t begin;
            size_t count;
        };
        std::vector<Excess> excess;
        size_t i = 0;
        while (i < out.size())
        {
            size_t j = i;
            while (j < out.size() && out[j].itemIdx == out[i].itemIdx)
            {
                ++j;
            }
            size_t const have = out[i].itemIdx < _botUp.size() ? _botUp[out[i].itemIdx] : 0;
            if (j - i > have)
            {
                excess.push_back({i, (j - i) - have});
            }
            i = j;
        }

        // Items in random order (Fisher-Yates) until the house total is reached.
        for (size_t k = excess.size(); k > 1; --k)
        {
            size_t const pick = static_cast<size_t>(rng.Next() % k);
            std::swap(excess[k - 1], excess[pick]);
        }
        std::vector<Listing> chosen;
        for (Excess const& item : excess)
        {
            if (budget == 0)
            {
                break;
            }
            size_t const take = static_cast<size_t>(std::min<uint64>(item.count, budget));
            chosen.insert(chosen.end(), out.begin() + item.begin, out.begin() + item.begin + take);
            budget -= take;
        }
        out.swap(chosen);
    }
}
