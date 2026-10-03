#include "MarketData.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <sstream>
#include <string_view>
#include "ASParse.h"
#include "AuctionSimVersion.h"
#include "Log.h"
#include "StringFormat.h"
#include "Tokenize.h"

namespace Market
{
    namespace
    {
        // Whole-field double, finite only. from_chars: no locale, no allocation.
        bool ParseDouble(std::string_view field, double& out)
        {
            if (!field.empty() && field.front() == '+')
            {
                field.remove_prefix(1);
            }
            auto result = std::from_chars(field.data(), field.data() + field.size(), out);
            return result.ec == std::errc() && result.ptr == field.data() + field.size() && std::isfinite(out);
        }

        bool ParseFloat(std::string_view field, float& out)
        {
            double value = 0.0;
            if (!ParseDouble(field, value))
            {
                return false;
            }
            out = static_cast<float>(value);
            return true;
        }

        bool ParseQuantiles(std::vector<std::string_view> const& f, size_t at, Quantiles& out)
        {
            for (size_t i = 0; i < kQuantiles; ++i)
            {
                if (!ParseFloat(f[at + i], out[i]))
                {
                    return false;
                }
            }
            return true;
        }

        // -1 .. 65534 for type / class, -1 .. 254 for the bins; anything else is malformed.
        bool InRange(int32 value, int32 lo, int32 hi) { return value >= lo && value <= hi; }

        struct RawItem
        {
            size_t faction;
            Item item;
        };
        struct RawKeyed  // CURVE / WEEKDAY: (faction, class) -> row index
        {
            size_t faction;
            int32 itemClass;
            uint32 row;
        };
        struct RawCraft
        {
            size_t faction;
            uint32 item;
            uint32 reagent;
            float qty;
            float margin;
        };
        struct RawBasket
        {
            size_t faction;
            uint32 item;
            BasketRow row;
        };

        // Accumulates every section's rows; Link() turns them into the Faction tables once
        // all sections are read, since sections may come in any order.
        struct Builder
        {
            std::array<std::vector<Curve>, kFactions> curves;
            std::array<std::vector<std::array<float, kWeekdays>>, kFactions> weekdays;
            std::vector<RawItem> items;
            std::vector<RawKeyed> curveKeys;
            std::vector<RawKeyed> weekdayKeys;
            std::vector<RawCraft> craft;
            std::vector<RawBasket> basket;
        };
    }

    size_t FactionSlot(uint32 houseId)
    {
        if (houseId == kAllianceHouse)
        {
            return 0;
        }
        return houseId == kHordeHouse ? 1 : kFactions;
    }

    uint32 FactionHouse(size_t slot) { return slot == 0 ? kAllianceHouse : kHordeHouse; }

    int32 UnitsBin(uint64 units)
    {
        int32 bin = static_cast<int32>(std::floor(std::log1p(static_cast<double>(units))));
        return std::min(bin, kMaxUnitsBin);
    }

    int32 SellersBin(uint32 sellers)
    {
        if (sellers <= 2)
        {
            return static_cast<int32>(sellers);
        }
        if (sellers <= 4)
        {
            return 3;
        }
        return sellers <= 8 ? 4 : 5;
    }

    int32 CheapestBin(bool hasCheapest, double cheapest, double ref)
    {
        if (!hasCheapest || cheapest <= 0.0 || ref <= 0.0)
        {
            return -1;
        }
        double x = std::log(cheapest / ref);
        auto it = std::upper_bound(kCheapestEdges.begin(), kCheapestEdges.end(), x);
        return static_cast<int32>(it - kCheapestEdges.begin());
    }

    double QuantileDraw(Quantiles const& q, double u)
    {
        if (u <= kQuantileP.front())
        {
            return q.front();
        }
        if (u >= kQuantileP.back())
        {
            return q.back();
        }
        size_t hi = 1;
        while (kQuantileP[hi] < u)
        {
            ++hi;
        }
        double p0 = kQuantileP[hi - 1];
        double t = (u - p0) / (kQuantileP[hi] - p0);
        return q[hi - 1] + t * (q[hi] - q[hi - 1]);
    }

    double ReservationRatio(Curve const& w, double uBin, double uIn)
    {
        size_t bin = 0;
        for (size_t b = kCurveBins; b-- > 0;)
        {
            if (uBin < w[b])
            {
                bin = b;
                break;
            }
        }
        double lo = kRatioEdges[bin];
        return lo + uIn * (kRatioEdges[bin + 1] - lo);
    }

    uint64 PolicyKey(int32 type, int32 itemClass, int32 ub, int32 sb, int32 mb)
    {
        // +1 so -1 packs as 0; type/class get 16 bits, the bins 8.
        return (static_cast<uint64>(static_cast<uint16>(type + 1)) << 40) |
               (static_cast<uint64>(static_cast<uint16>(itemClass + 1)) << 24) |
               (static_cast<uint64>(static_cast<uint8>(ub + 1)) << 16) |
               (static_cast<uint64>(static_cast<uint8>(sb + 1)) << 8) | static_cast<uint64>(static_cast<uint8>(mb + 1));
    }

    uint64 StackKey(int32 type, int32 itemClass) { return PolicyKey(type, itemClass, -1, -1, -1); }

    int32 Faction::FindItem(uint32 itemId) const
    {
        auto it = itemIndex.find(itemId);
        return it != itemIndex.end() ? static_cast<int32>(it->second) : -1;
    }

    PolicyRow const* Faction::FindPolicy(int32 type, int32 itemClass, int32 ub, int32 sb, int32 mb) const
    {
        uint64 const keys[] = {
            PolicyKey(type, itemClass, ub, sb, mb),
            PolicyKey(type, itemClass, -1, -1, mb),
            PolicyKey(type, -1, -1, -1, mb),
            PolicyKey(-1, -1, -1, -1, mb),
        };
        for (uint64 key : keys)
        {
            auto it = policyIndex.find(key);
            if (it != policyIndex.end())
            {
                return &policies[it->second];
            }
        }
        return nullptr;
    }

    int32 Faction::FindStack(int32 type, int32 itemClass) const
    {
        for (uint64 key : {StackKey(type, itemClass), StackKey(type, -1), StackKey(-1, -1)})
        {
            auto it = stackIndex.find(key);
            if (it != stackIndex.end())
            {
                return static_cast<int32>(it->second);
            }
        }
        return -1;
    }

    bool Data::Parse(std::istream& in, std::string& error)
    {
        *this = Data{};

        std::string line;
        if (!std::getline(in, line))
        {
            error = "the file is empty";
            return false;
        }
        {
            std::istringstream stamp(line);
            std::string tag;
            uint32 version = 0;
            if (!(stamp >> tag) || tag != "AUCTIONSIM_MARKET" || !(stamp >> version))
            {
                error = "line 1 is not an 'AUCTIONSIM_MARKET <version>' stamp";
                return false;
            }
            foundVersion = version;
        }
        if (foundVersion != AUCTIONSIM_MARKET_VERSION)
        {
            error = Acore::StringFormat(
                "schema v{}, this build reads v{}", foundVersion, static_cast<uint32>(AUCTIONSIM_MARKET_VERSION));
            return false;
        }

        Builder builder;
        std::vector<std::string> seenSections;

        auto skipRow = [this](std::string_view section, std::string const& row) {
            if (++stats.skippedRows <= 20)
            {
                LOG_ERROR("module", "AuctionSim: market file: skipping malformed {} row '{}'", section, row);
            }
        };

        while (std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            if (line.empty())
            {
                continue;
            }

            std::istringstream header(line);
            std::string section;
            size_t count = 0;
            if (!(header >> section >> count))
            {
                error = Acore::StringFormat("malformed section header '{}'", line);
                return false;
            }
            if (std::find(seenSections.begin(), seenSections.end(), section) != seenSections.end())
            {
                error = Acore::StringFormat("section {} appears twice", section);
                return false;
            }
            seenSections.push_back(section);

            for (size_t read = 0; read < count; ++read)
            {
                if (!std::getline(in, line))
                {
                    error = Acore::StringFormat("section {} declares {} rows but the file ends after {}", section,
                        count, read);
                    return false;
                }
                if (!line.empty() && line.back() == '\r')
                {
                    line.pop_back();
                }
                ++stats.rows;

                std::vector<std::string_view> f = Acore::Tokenize(line, ':', true);
                uint32 house = 0;
                bool knownSection = true;
                bool ok = !f.empty() && ASParse::Integer(f[0], house);
                size_t slot = ok ? FactionSlot(house) : kFactions;
                if (section == "CLASS")
                {
                    int32 code = 0;
                    if (f.size() == 2 && ASParse::Integer(f[0], code))
                    {
                        classNames[code] = std::string(f[1]);
                    }
                    else
                    {
                        skipRow(section, line);
                    }
                    continue;
                }
                else if (section == "META")
                {
                    ok = ok && f.size() == 5 && slot < kFactions;
                    Faction& fac = factions[ok ? slot : 0];
                    Faction parsed;
                    ok = ok && ParseFloat(f[1], parsed.demandScale) && ParseFloat(f[2], parsed.supplyScale) &&
                         ASParse::ClampedU32(f[3], parsed.refListings) && ASParse::ClampedU32(f[4], parsed.refSellers) &&
                         parsed.demandScale >= 0.0f && parsed.supplyScale >= 0.0f;
                    if (ok)
                    {
                        fac.present = true;
                        fac.demandScale = parsed.demandScale;
                        fac.supplyScale = parsed.supplyScale;
                        fac.refListings = parsed.refListings;
                        fac.refSellers = parsed.refSellers;
                    }
                }
                else if (section == "ITEM")
                {
                    Item item;
                    ok = ok && f.size() == 8 && slot < kFactions && ASParse::Integer(f[1], item.itemId) &&
                         ASParse::Integer(f[2], item.itemClass) && ParseFloat(f[3], item.ref) &&
                         ASParse::Integer(f[4], item.conv) && ASParse::Integer(f[5], item.maxc) &&
                         ASParse::ClampedU32(f[6], item.vendor) && ParseFloat(f[7], item.buyersH) &&
                         InRange(item.itemClass, -1, 65534) && item.ref > 0.0f && item.buyersH >= 0.0f;
                    if (ok)
                    {
                        item.listed = true;
                        item.conv = std::max<uint32>(1, item.conv);
                        item.maxc = std::max<uint32>(1, item.maxc);
                        builder.items.push_back({slot, item});
                    }
                }
                else if (section == "CURVE")
                {
                    int32 itemClass = 0;
                    Curve w{};
                    ok = ok && f.size() == 2 + kCurveBins && slot < kFactions && ASParse::Integer(f[1], itemClass) &&
                         InRange(itemClass, -1, 65534);
                    for (size_t b = 0; ok && b < kCurveBins; ++b)
                    {
                        ok = ParseFloat(f[2 + b], w[b]) && w[b] >= 0.0f && w[b] <= 1.0001f;
                    }
                    if (ok)
                    {
                        builder.curveKeys.push_back(
                            {slot, itemClass, static_cast<uint32>(builder.curves[slot].size())});
                        builder.curves[slot].push_back(w);
                    }
                }
                else if (section == "WEEKDAY")
                {
                    int32 itemClass = 0;
                    std::array<float, kWeekdays> m{};
                    ok = ok && f.size() == 2 + kWeekdays && slot < kFactions && ASParse::Integer(f[1], itemClass) &&
                         InRange(itemClass, -1, 65534);
                    for (size_t d = 0; ok && d < kWeekdays; ++d)
                    {
                        ok = ParseFloat(f[2 + d], m[d]) && m[d] >= 0.0f;
                    }
                    if (ok)
                    {
                        builder.weekdayKeys.push_back(
                            {slot, itemClass, static_cast<uint32>(builder.weekdays[slot].size())});
                        builder.weekdays[slot].push_back(m);
                    }
                }
                else if (section == "POLICY")
                {
                    int32 type = 0, itemClass = 0, ub = 0, sb = 0, mb = 0;
                    PolicyRow row;
                    ok = ok && f.size() == 14 && slot < kFactions && ASParse::Integer(f[1], type) &&
                         ASParse::Integer(f[2], itemClass) && ASParse::Integer(f[3], ub) &&
                         ASParse::Integer(f[4], sb) && ASParse::Integer(f[5], mb) && ParseQuantiles(f, 6, row.q) &&
                         ParseFloat(f[13], row.offset) && InRange(type, -1, 65534) &&
                         InRange(itemClass, -1, 65534) && InRange(ub, -1, kMaxUnitsBin) && InRange(sb, -1, 5) &&
                         InRange(mb, -1, kMaxCheapestBin);
                    if (ok)
                    {
                        Faction& fac = factions[slot];
                        if (fac.policyIndex
                                .try_emplace(PolicyKey(type, itemClass, ub, sb, mb),
                                    static_cast<uint32>(fac.policies.size()))
                                .second)
                        {
                            fac.policies.push_back(row);
                        }
                    }
                }
                else if (section == "STACK")
                {
                    int32 type = 0, itemClass = 0;
                    Quantiles q{};
                    ok = ok && f.size() == 10 && slot < kFactions && ASParse::Integer(f[1], type) &&
                         ASParse::Integer(f[2], itemClass) && ParseQuantiles(f, 3, q) && InRange(type, -1, 65534) &&
                         InRange(itemClass, -1, 65534);
                    if (ok)
                    {
                        Faction& fac = factions[slot];
                        if (fac.stackIndex.try_emplace(StackKey(type, itemClass), static_cast<uint32>(fac.stacks.size()))
                                .second)
                        {
                            fac.stacks.push_back(q);
                        }
                    }
                }
                else if (section == "CRAFT")
                {
                    RawCraft raw{slot, 0, 0, 0.0f, 0.0f};
                    ok = ok && f.size() == 5 && slot < kFactions && ASParse::Integer(f[1], raw.item) &&
                         ASParse::Integer(f[2], raw.reagent) && ParseFloat(f[3], raw.qty) &&
                         ParseFloat(f[4], raw.margin) && raw.qty > 0.0f && raw.margin >= 0.0f;
                    if (ok)
                    {
                        builder.craft.push_back(raw);
                    }
                }
                else if (section == "BOT")
                {
                    BotName bot;
                    ok = ok && f.size() == 4 && slot < kFactions && ASParse::Integer(f[1], bot.index) &&
                         !f[2].empty() && ASParse::Integer(f[3], bot.type);
                    if (ok)
                    {
                        bot.name = std::string(f[2]);
                        factions[slot].bots.push_back(std::move(bot));
                    }
                }
                else if (section == "BASKET")
                {
                    RawBasket raw{slot, 0, {}};
                    ok = ok && f.size() == 7 && slot < kFactions && ASParse::Integer(f[1], raw.row.bot) &&
                         ASParse::Integer(f[2], raw.item) && ASParse::Integer(f[3], raw.row.type) &&
                         ParseFloat(f[4], raw.row.rateH) && ParseFloat(f[5], raw.row.tl4) &&
                         ParseFloat(f[6], raw.row.batch) && InRange(raw.row.type, -1, 65534) &&
                         raw.row.rateH >= 0.0f && raw.row.tl4 >= 0.0f && raw.row.tl4 <= 1.0f;
                    if (ok)
                    {
                        raw.row.batch = std::max(1.0f, raw.row.batch);
                        builder.basket.push_back(raw);
                    }
                }
                else
                {
                    knownSection = false;  // a newer exporter's section: skip its rows
                }

                if (knownSection && !ok)
                {
                    skipRow(section, line);
                }
            }
        }

        // --- Link ------------------------------------------------------------------
        for (RawItem& raw : builder.items)
        {
            Faction& fac = factions[raw.faction];
            if (fac.itemIndex.try_emplace(raw.item.itemId, static_cast<uint32>(fac.items.size())).second)
            {
                fac.items.push_back(raw.item);
            }
        }

        for (size_t slot = 0; slot < kFactions; ++slot)
        {
            Faction& fac = factions[slot];
            fac.curves = std::move(builder.curves[slot]);
            fac.weekdays = std::move(builder.weekdays[slot]);

            std::unordered_map<int32, int32> curveByClass;
            std::unordered_map<int32, int32> weekdayByClass;
            for (RawKeyed const& key : builder.curveKeys)
            {
                if (key.faction == slot)
                {
                    curveByClass.try_emplace(key.itemClass, static_cast<int32>(key.row));
                }
            }
            for (RawKeyed const& key : builder.weekdayKeys)
            {
                if (key.faction == slot)
                {
                    weekdayByClass.try_emplace(key.itemClass, static_cast<int32>(key.row));
                }
            }
            auto lookup = [](std::unordered_map<int32, int32> const& map, int32 itemClass) {
                auto it = map.find(itemClass);
                if (it == map.end())
                {
                    it = map.find(-1);
                }
                return it != map.end() ? it->second : -1;
            };
            for (Item& item : fac.items)
            {
                item.curve = lookup(curveByClass, item.itemClass);
                item.weekday = lookup(weekdayByClass, item.itemClass);
            }
        }

        // CRAFT: group each crafted item's reagents into one contiguous range. A reagent
        // without an ITEM row still gets an (unlisted) entry so its cheapest-up is tracked.
        std::stable_sort(builder.craft.begin(), builder.craft.end(), [](RawCraft const& a, RawCraft const& b) {
            return a.faction != b.faction ? a.faction < b.faction : a.item < b.item;
        });
        for (RawCraft const& raw : builder.craft)
        {
            Faction& fac = factions[raw.faction];
            int32 crafted = fac.FindItem(raw.item);
            if (crafted < 0)
            {
                continue;
            }
            int32 reagent = fac.FindItem(raw.reagent);
            if (reagent < 0)
            {
                Item unlisted;
                unlisted.itemId = raw.reagent;
                reagent = static_cast<int32>(fac.items.size());
                fac.itemIndex.emplace(raw.reagent, static_cast<uint32>(reagent));
                fac.items.push_back(unlisted);
            }
            Item& item = fac.items[crafted];
            if (item.craftBegin == item.craftEnd)
            {
                item.craftBegin = static_cast<uint32>(fac.craft.size());
            }
            fac.craft.push_back({static_cast<uint32>(reagent), raw.qty});
            item.craftEnd = static_cast<uint32>(fac.craft.size());
            item.craftMargin = raw.margin;
        }

        for (RawBasket& raw : builder.basket)
        {
            Faction& fac = factions[raw.faction];
            int32 idx = fac.FindItem(raw.item);
            if (idx < 0 || !fac.items[idx].listed)
            {
                ++stats.droppedBasket;
                continue;
            }
            raw.row.itemIdx = static_cast<uint32>(idx);
            raw.row.stack = fac.FindStack(raw.row.type, fac.items[idx].itemClass);
            fac.basket.push_back(raw.row);
        }

        for (size_t slot = 0; slot < kFactions; ++slot)
        {
            Faction& fac = factions[slot];
            std::sort(fac.bots.begin(), fac.bots.end(), [](BotName const& a, BotName const& b) {
                return a.index < b.index;
            });
            if (!fac.present)
            {
                continue;
            }
            for (int32 mb = -1; mb <= kMaxCheapestBin; ++mb)
            {
                if (!fac.policyIndex.count(PolicyKey(-1, -1, -1, -1, mb)))
                {
                    error = Acore::StringFormat(
                        "faction {} has no fallback POLICY row (-1,-1,-1,-1,{})", FactionHouse(slot), mb);
                    return false;
                }
            }
            if (!fac.stackIndex.count(StackKey(-1, -1)))
            {
                error = Acore::StringFormat("faction {} has no fallback STACK row (-1,-1)", FactionHouse(slot));
                return false;
            }
            if (fac.bots.empty())
            {
                error = Acore::StringFormat("faction {} has no BOT rows", FactionHouse(slot));
                return false;
            }
        }

        if (!factions[0].present && !factions[1].present)
        {
            error = "no META row for either faction";
            return false;
        }
        return true;
    }

    void Data::Resolve(ItemFactsFn const& facts)
    {
        for (Faction& fac : factions)
        {
            std::vector<bool> postable(fac.items.size(), false);
            for (size_t i = 0; i < fac.items.size(); ++i)
            {
                Item& item = fac.items[i];
                ItemFacts f = facts(item.itemId);
                if (!f.exists)
                {
                    if (item.listed)
                    {
                        ++stats.droppedItems;
                    }
                    item.listed = false;
                    item.curve = -1;
                    continue;
                }
                item.vendor = std::max(item.vendor, f.sellPrice);
                item.maxc = std::max<uint32>(1, std::min(item.maxc, std::max<uint32>(1, f.maxStack)));
                item.vendorBuyGuard = f.vendorBuyGuard;
                postable[i] = item.listed && f.postable;
            }

            size_t before = fac.basket.size();
            fac.basket.erase(std::remove_if(fac.basket.begin(), fac.basket.end(),
                                 [&postable](BasketRow const& row) { return !postable[row.itemIdx]; }),
                fac.basket.end());
            stats.droppedBasket += before - fac.basket.size();
        }
    }

    void Data::SetRates(double scale, double dtHours)
    {
        for (Faction& fac : factions)
        {
            double perRow = static_cast<double>(fac.supplyScale) * scale * dtHours;
            for (BasketRow& row : fac.basket)
            {
                double lambda = static_cast<double>(row.rateH) * perRow;
                row.lambda = static_cast<float>(lambda);
                row.expNegLambda = static_cast<float>(std::exp(-lambda));
                row.expNegBatch = static_cast<float>(std::exp(-(static_cast<double>(row.batch) - 1.0)));
            }
        }
    }

    size_t Data::MemoryBytes() const
    {
        // Hash nodes: key + value + next pointer + cached hash, plus one bucket pointer.
        constexpr size_t kNode = 32 + sizeof(void*);
        size_t bytes = 0;
        for (Faction const& fac : factions)
        {
            bytes += fac.items.capacity() * sizeof(Item) + fac.itemIndex.size() * kNode;
            bytes += fac.craft.capacity() * sizeof(Reagent);
            bytes += fac.curves.capacity() * sizeof(Curve) + fac.weekdays.capacity() * sizeof(fac.weekdays[0]);
            bytes += fac.policies.capacity() * sizeof(PolicyRow) + fac.policyIndex.size() * kNode;
            bytes += fac.stacks.capacity() * sizeof(Quantiles) + fac.stackIndex.size() * kNode;
            bytes += fac.bots.capacity() * (sizeof(BotName) + 16);
            bytes += fac.basket.capacity() * sizeof(BasketRow);
        }
        return bytes;
    }
}
