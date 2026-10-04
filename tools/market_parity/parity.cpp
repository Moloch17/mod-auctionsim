// Parity run: C++ MarketEngine on the shipped market file, empty house, no players.
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>
#include "MarketData.h"
#include "MarketEngine.h"
#include "MarketRng.h"

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: %s <auctionsim_market.dat> <output dir>\n", argv[0]);
        return 2;
    }
    Market::Data data;
    std::string error;
    std::ifstream in(argv[1]);
    if (!data.Parse(in, error))
    {
        fprintf(stderr, "parse failed: %s\n", error.c_str());
        return 1;
    }
    data.Resolve([](uint32) { Market::ItemFacts f; f.exists = true; f.maxStack = 100000; return f; });
    double const scale = 0.1, dt = 0.5;
    uint32 const bots = 100;
    data.SetRates(scale, dt);
    uint64 const start = 1783296000ULL;  // 2026-07-06 00:00:00 UTC

    for (uint32 house : {6u, 2u})
    {
        size_t slot = Market::FactionSlot(house);
        Market::Faction const& fac = data.factions[slot];
        Market::Engine engine;
        // Bot slot i posts as owner i; one price memory per house, as the module keeps.
        std::vector<uint32> owners;
        for (uint32 b = 0; b < bots; ++b)
            owners.push_back(b);
        Market::PriceMemory memory;
        memory.Reserve(fac.basket.size());
        Market::Rng rng(house);
        std::vector<Market::Listing> live;
        std::vector<Market::PostOrder> orders;
        std::vector<uint32> claims;
        uint64 posts = 0, events = 0, buyers = 0, sales = 0;
        uint32 nextId = 1;
        std::string path = std::string(argv[2]) + "/parity_cpp_" + std::to_string(house) + ".csv";
        FILE* out = fopen(path.c_str(), "w");
        fprintf(out, "faction,day,item,units,min_unit,med_unit,listings_total\n");

        int const steps = static_cast<int>(21 * 24 / dt);
        for (int step = 0; step < steps; ++step)
        {
            uint64 const clock = start + static_cast<uint64>(step * dt * 3600);
            // Expiry (end > now survives).
            live.erase(std::remove_if(live.begin(), live.end(),
                           [clock](Market::Listing const& l) { return l.expire <= clock; }),
                live.end());
            engine.Listings().assign(live.begin(), live.end());
            engine.BuildState(fac.items.size());

            orders.clear();
            engine.PlanPosts(fac, owners, memory, clock, rng, orders);
            events += orders.size();
            for (Market::PostOrder const& o : orders)
            {
                for (uint32 k = 0; k < o.listings; ++k)
                {
                    Market::Listing l;
                    l.itemIdx = o.itemIdx;
                    l.perUnit = o.unitPrice;
                    l.count = o.count;
                    l.owner = o.botSlot;
                    l.auctionId = nextId++;
                    l.expire = static_cast<uint32>(clock + o.hours * 3600);
                    l.flags = Market::Listing::kBuyable;
                    engine.Listings().push_back(l);
                    ++posts;
                }
            }
            engine.BuildState(fac.items.size());

            claims.clear();
            size_t weekday = static_cast<size_t>((clock / 86400 + 3) % 7);  // UTC, Monday = 0
            buyers += engine.PlanBuys(fac, scale * dt, weekday, rng, claims);
            sales += claims.size();
            std::vector<char> sold(engine.Listings().size(), 0);
            for (uint32 c : claims)
                sold[c] = 1;
            live.clear();
            for (size_t i = 0; i < engine.Listings().size(); ++i)
                if (!sold[i])
                    live.push_back(engine.Listings()[i]);

            if ((clock / 3600) % 24 == 20 && clock % 3600 < dt * 3600)
            {
                uint64 const day = (clock - start) / 86400;
                std::vector<Market::Listing> snap = live;
                std::sort(snap.begin(), snap.end(), [](Market::Listing const& a, Market::Listing const& b) {
                    return a.itemIdx != b.itemIdx ? a.itemIdx < b.itemIdx : a.perUnit < b.perUnit;
                });
                for (size_t i = 0; i < snap.size();)
                {
                    size_t j = i;
                    uint64 units = 0;
                    while (j < snap.size() && snap[j].itemIdx == snap[i].itemIdx)
                        units += snap[j++].count;
                    uint64 cum = 0;
                    uint32 med = snap[i].perUnit;
                    for (size_t k = i; k < j; ++k)
                    {
                        cum += snap[k].count;
                        if (2 * cum >= units)
                        {
                            med = snap[k].perUnit;
                            break;
                        }
                    }
                    fprintf(out, "%u,%llu,%u,%llu,%u,%u,%zu\n", house, (unsigned long long)day,
                        fac.items[snap[i].itemIdx].itemId, (unsigned long long)units, snap[i].perUnit, med,
                        snap.size());
                    i = j;
                }
            }
        }
        fclose(out);
        printf("faction %u: post events %llu, listings posted %llu, buyers %llu, sales %llu, house at end %zu, "
               "price memory entries %zu -> %s\n",
            house, (unsigned long long)events, (unsigned long long)posts, (unsigned long long)buyers,
            (unsigned long long)sales, live.size(), memory.Size(), path.c_str());
    }
}
