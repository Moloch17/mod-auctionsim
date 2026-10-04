// Fill check: runs the parity setup (Scale 0.1, 100 bots, empty start, 21 days from
// 2026-07-06 00:00 UTC) and, at the end, MARKET_FORMAT.md's Fill twice per faction:
// on an empty house (should come out near the 21-day house) and on the 21-day house
// itself (should add little). Prints sizes, items up and the fill's simulation time.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "MarketData.h"
#include "MarketEngine.h"
#include "MarketRng.h"

namespace
{
    size_t ItemsUp(std::vector<Market::Listing> const& listings)
    {
        std::vector<uint32> items;
        for (Market::Listing const& l : listings)
            items.push_back(l.itemIdx);
        std::sort(items.begin(), items.end());
        return static_cast<size_t>(std::unique(items.begin(), items.end()) - items.begin());
    }

    std::vector<Market::Listing> Fill(Market::Faction const& fac, std::vector<Market::Listing> const& house,
        uint32 bots, double scale, uint64 endClock, Market::Rng& rng, long long& micros, long long& worstTick)
    {
        std::vector<uint32> owners;
        for (uint32 b = 0; b < bots; ++b)
            owners.push_back(b);
        std::vector<uint8> weekdays;
        uint64 const begin = endClock - uint64(Market::FillRun::kFillHours) * 3600;
        for (uint32 k = 0; k < Market::FillRun::kFillHours * 2; ++k)
            weekdays.push_back(static_cast<uint8>(((begin + uint64(k) * 1800) / 86400 + 3) % 7));
        Market::FillRun fill;
        auto start = std::chrono::steady_clock::now();
        fill.Begin(fac, house, owners, weekdays, scale, 0.5, endClock);
        worstTick = 0;
        bool done = false;
        while (!done)
        {
            auto tick = std::chrono::steady_clock::now();
            done = fill.Advance(8, rng);  // MarketService::kFillStepsPerTick
            worstTick = std::max<long long>(worstTick,
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - tick).count());
        }
        std::vector<Market::Listing> out;
        fill.Result(out, rng);
        auto elapsed = std::chrono::steady_clock::now() - start;
        micros = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
        return out;
    }
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        fprintf(stderr, "usage: %s <auctionsim_market.dat> [scale]\n", argv[0]);
        return 2;
    }
    double const scale = argc > 2 ? std::atof(argv[2]) : 0.1;
    Market::Data data;
    std::string error;
    std::ifstream in(argv[1]);
    if (!data.Parse(in, error))
    {
        fprintf(stderr, "parse failed: %s\n", error.c_str());
        return 1;
    }
    data.Resolve([](uint32) { Market::ItemFacts f; f.exists = true; f.maxStack = 100000; return f; });
    double const dt = 0.5;
    uint32 const bots = 100;
    data.SetRates(scale, dt);
    uint64 const start = 1783296000ULL;
    int const steps = static_cast<int>(21 * 24 / dt);
    uint64 const end = start + uint64(steps) * 1800;

    for (uint32 houseId : {6u, 2u})
    {
        Market::Faction const& fac = data.factions[Market::FactionSlot(houseId)];
        Market::Engine engine;
        // Bot slot i posts as owner i; one price memory per house, as the module keeps.
        std::vector<uint32> owners;
        for (uint32 b = 0; b < bots; ++b)
            owners.push_back(b);
        Market::PriceMemory memory;  // unused while Market::kMemoryWeight is 0
        Market::Rng rng(houseId);
        std::vector<Market::Listing> live;
        std::vector<Market::PostOrder> orders;
        std::vector<uint32> claims;
        uint32 nextId = 1;
        for (int step = 0; step < steps; ++step)
        {
            uint64 const clock = start + uint64(step) * 1800;
            live.erase(std::remove_if(live.begin(), live.end(),
                           [clock](Market::Listing const& l) { return l.expire <= clock; }),
                live.end());
            engine.Listings().assign(live.begin(), live.end());
            engine.BuildState(fac.items.size());
            orders.clear();
            engine.PlanPosts(fac, owners, memory, clock, rng, orders);
            for (Market::PostOrder const& o : orders)
                for (uint32 k = 0; k < o.listings; ++k)
                {
                    Market::Listing l;
                    l.itemIdx = o.itemIdx;
                    l.perUnit = o.unitPrice;
                    l.count = o.count;
                    l.owner = o.botSlot;
                    l.auctionId = nextId++;
                    l.expire = static_cast<uint32>(clock + uint64(o.hours) * 3600);
                    l.flags = Market::Listing::kBuyable | Market::Listing::kBotOwned;
                    engine.Listings().push_back(l);
                }
            engine.BuildState(fac.items.size());
            claims.clear();
            engine.PlanBuys(fac, scale * dt, static_cast<size_t>((clock / 86400 + 3) % 7), rng, claims);
            std::vector<char> sold(engine.Listings().size(), 0);
            for (uint32 c : claims)
                sold[c] = 1;
            live.clear();
            for (size_t i = 0; i < engine.Listings().size(); ++i)
                if (!sold[i])
                    live.push_back(engine.Listings()[i]);
        }
        live.erase(std::remove_if(live.begin(), live.end(),
                       [end](Market::Listing const& l) { return l.expire <= end; }),
            live.end());

        long long emptyUs = 0, emptyTick = 0, fullUs = 0, fullTick = 0;
        std::vector<Market::Listing> fromEmpty = Fill(fac, {}, bots, scale, end, rng, emptyUs, emptyTick);
        std::vector<Market::Listing> onFull = Fill(fac, live, bots, scale, end, rng, fullUs, fullTick);
        printf("faction %u, scale %g: 21-day house %zu listings / %zu items; fill from empty %zu / %zu "
               "(%.0f%% / %.0f%%) in %lld us (worst 8-step tick %lld us); fill on the 21-day house adds %zu "
               "in %lld us (worst tick %lld us)\n",
            houseId, scale, live.size(), ItemsUp(live), fromEmpty.size(), ItemsUp(fromEmpty),
            100.0 * fromEmpty.size() / std::max<size_t>(1, live.size()),
            100.0 * ItemsUp(fromEmpty) / std::max<size_t>(1, ItemsUp(live)), emptyUs, emptyTick, onFull.size(),
            fullUs, fullTick);
    }
}
