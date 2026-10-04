#include "MarketService.h"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <fstream>
#include "ASConfig.h"
#include "AuctionBuyingService.h"
#include "AuctionPricing.h"
#include "CraftedItems.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Random.h"
#include "StringFormat.h"
#include "Timer.h"

namespace
{
    // Retry cadence for a bot setup waiting on account creation.
    constexpr uint32 kSetupRetryMs = 2000;
    constexpr uint64 kMaxBuyout = 0x7FFFFFFF - 1;  // Player.h's MAX_MONEY_AMOUNT

    AuctionHouseId HouseOf(size_t faction) { return static_cast<AuctionHouseId>(Market::FactionHouse(faction)); }
}

bool MarketService::LoadFile(
    std::string const& path, ASConfig const& config, Market::Data& out, std::string& error, uint32& foundVersion)
{
    foundVersion = 0;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec))
    {
        error = Acore::StringFormat("{} not found", path);
        return false;
    }
    std::ifstream stream(path, std::ios::in);
    if (!stream.is_open())
    {
        error = Acore::StringFormat("couldn't open {}", path);
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    bool parsed = out.Parse(stream, error);
    foundVersion = out.foundVersion;
    if (!parsed)
    {
        return false;
    }

    out.Resolve([&config](uint32 itemId) {
        Market::ItemFacts facts;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(itemId);
        if (!proto)
        {
            return facts;
        }
        facts.exists = true;
        facts.postable = AuctionPricing::IsWithinLevelCap(
            proto->RequiredLevel, proto->ItemLevel, config.maxRequiredLevel, config.maxItemLevel);
        facts.sellPrice = proto->SellPrice;
        facts.maxStack = std::max<uint32>(1, proto->GetMaxStackSize());
        facts.vendorBuyGuard = (config.IsVendorSold(itemId) && proto->BuyPrice > 0)
            ? AuctionPricing::VendorUnitBuyPrice(proto->BuyPrice, proto->BuyCount)
            : 0;
        return facts;
    });

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    for (size_t slot = 0; slot < Market::kFactions; ++slot)
    {
        Market::Faction const& fac = out.factions[slot];
        if (fac.present)
        {
            LOG_INFO(
                "module",
                "AuctionSim: market faction {}: {} items, {} basket rows, {} policy rows, {} bot names",
                Market::FactionHouse(slot),
                fac.items.size(),
                fac.basket.size(),
                fac.policies.size(),
                fac.bots.size());
        }
    }
    LOG_INFO(
        "module",
        "AuctionSim: loaded {} in {} ms (~{} KB of tables; {} malformed rows skipped, {} items not on this realm, "
        "{} basket rows dropped)",
        path,
        elapsed.count(),
        out.MemoryBytes() / 1024,
        out.stats.skippedRows,
        out.stats.droppedItems,
        out.stats.droppedBasket);
    return true;
}

MarketService::MarketService(
    Market::Data& data, Market::BotRoster& roster, AuctionBuyingService& buying, ObjectGuid buyerGuid)
    : _data(data),
      _roster(roster),
      _buying(buying),
      _buyerGuid(buyerGuid),
      _rng((static_cast<uint64>(rand32()) << 32) | rand32())
{
}

void MarketService::SetScale(float scale)
{
    _scale = std::max(0.0f, scale);
    _data.SetRates(_scale, kStepHours);
}

Market::BotRoster::Result MarketService::SetupBots(uint32 bots)
{
    _wantedBots = bots;
    Market::BotRoster::Result result = _roster.Ensure(_data, bots, _setupNote);
    _ready = result == Market::BotRoster::Result::Ready;
    _setupPending = result == Market::BotRoster::Result::Pending;
    if (_ready)
    {
        _setupNote.clear();
        LOG_INFO(
            "module",
            "AuctionSim: market ready with {} Alliance and {} Horde seller(s)",
            BotsInUse(0),
            BotsInUse(1));
    }
    else if (result == Market::BotRoster::Result::Failed)
    {
        LOG_ERROR("module", "AuctionSim: market bot setup failed: {}", _setupNote);
    }
    return result;
}

void MarketService::Update(uint32 diff)
{
    if (_setupPending)
    {
        _setupRetryTimer += diff;
        if (_setupRetryTimer >= kSetupRetryMs)
        {
            _setupRetryTimer = 0;
            if (SetupBots(_wantedBots) == Market::BotRoster::Result::Ready && _stepRequested)
            {
                Step();  // a step (startup scan, ".auctionsim scan") asked for while waiting
            }
        }
    }

    for (size_t faction = 0; faction < Market::kFactions; ++faction)
    {
        Fill& fill = _fills[faction];
        if (fill.running)
        {
            // Simulated steps until this tick's time budget is spent (at least one).
            auto start = std::chrono::steady_clock::now();
            bool done = false;
            long long spent = 0;
            for (uint32 n = 0; n < kFillStepsPerTick && !done && spent < kFillTickBudgetMicros; ++n)
            {
                done = fill.run.Advance(1, _rng);
                spent = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
            }
            fill.micros += spent;
            if (done)
            {
                FinishFill(faction);
            }
        }
    }

    if (_pendingHead < _pending.size() && _budgetLeft > 0)
    {
        DrainPending();
    }
    _budgetLeft = kMaxListingsPerTick;  // a fresh budget for the next tick's step and drain
}

size_t MarketService::PendingListings() const
{
    size_t total = 0;
    for (size_t i = _pendingHead; i < _pending.size(); ++i)
    {
        total += _pending[i].order.listings;
    }
    return total;
}

size_t MarketService::Weekday(time_t now)
{
    std::tm local = Acore::Time::TimeBreakdown(now);
    return static_cast<size_t>((local.tm_wday + 6) % 7);
}

void MarketService::Step()
{
    if (!_ready)
    {
        _stepRequested = _setupPending;
        return;
    }
    _stepRequested = false;
    for (size_t faction = 0; faction < Market::kFactions; ++faction)
    {
        if (_data.factions[faction].present)
        {
            StepHouse(faction);
        }
    }
    _buying.SortQueue();
}

void MarketService::Snapshot(size_t faction, std::vector<Market::Listing>& listings) const
{
    Market::Faction const& fac = _data.factions[faction];
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(HouseOf(faction));
    listings.clear();
    for (auto const& entry : house->GetAuctions())
    {
        AuctionEntry const* auction = entry.second;
        int32 itemIdx = fac.FindItem(auction->item_template);
        if (itemIdx < 0)
        {
            continue;
        }
        Market::Listing listing;
        listing.itemIdx = static_cast<uint32>(itemIdx);
        listing.count = std::max<uint32>(1, auction->itemCount);
        listing.perUnit = auction->buyout > 0 ? auction->buyout / listing.count : Market::Listing::kNoBuyout;
        listing.owner = auction->owner.GetCounter();
        listing.auctionId = auction->Id;
        listing.expire = static_cast<uint32>(auction->expire_time);

        // A buyer never takes the buyer bot's own listing or one already queued; a
        // player's listing of a vendor-stocked item only up to the vendor's price, so
        // vendor goods can't be relisted into the market's buyers for a profit.
        uint32 const guard = fac.items[itemIdx].vendorBuyGuard;
        bool const botOwned = _roster.IsBot(listing.owner);
        if (botOwned)
        {
            listing.flags |= Market::Listing::kBotOwned;
        }
        if (auction->buyout > 0 && auction->owner != _buyerGuid && !_buying.IsQueued(auction->Id) &&
            !(!botOwned && guard > 0 && listing.perUnit > guard))
        {
            listing.flags |= Market::Listing::kBuyable;
        }
        listings.push_back(listing);
    }
}

bool MarketService::StartFill(size_t faction, std::string& note)
{
    if (!_ready)
    {
        note = _setupPending ? "the market sellers are still being set up" : "the market isn't running";
        return false;
    }
    if (faction >= Market::kFactions || !_data.factions[faction].present)
    {
        note = "the market file has no data for that faction";
        return false;
    }
    if (_roster.Slots(faction).empty())
    {
        note = "that faction has no sellers";
        return false;
    }
    Fill& fill = _fills[faction];
    if (fill.running)
    {
        note = "a fill of that house is already running";
        return false;
    }

    auto start = std::chrono::steady_clock::now();
    Snapshot(faction, _fillScratch);
    std::vector<uint32> owners;
    for (ObjectGuid const& guid : _roster.Slots(faction))
    {
        owners.push_back(guid.GetCounter());
    }
    time_t const now = GameTime::GetGameTime().count();
    uint32 const steps = static_cast<uint32>(Market::FillRun::kFillHours / kStepHours + 0.5);
    std::vector<uint8> weekdays(steps);
    time_t const begin = now - static_cast<time_t>(Market::FillRun::kFillHours) * 3600;
    for (uint32 k = 0; k < steps; ++k)
    {
        weekdays[k] = static_cast<uint8>(Weekday(begin + static_cast<time_t>(k * kStepHours * 3600)));
    }
    fill.run.Begin(
        _data.factions[faction], _fillScratch, owners, weekdays, _scale, kStepHours, static_cast<uint64>(now));
    fill.running = true;
    fill.result = 0;
    fill.micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    note = Acore::StringFormat("fill started: {} simulated steps over the last {} h, {} real listings as competitors",
        steps, Market::FillRun::kFillHours, _fillScratch.size());
    return true;
}

void MarketService::FinishFill(size_t faction)
{
    Fill& fill = _fills[faction];
    fill.running = false;
    fill.run.Result(_fillScratch, _rng);

    // The survivors go through the normal creation queue, each as one listing that
    // keeps its simulated expiry (its posting duration still sets the deposit).
    std::vector<ObjectGuid> const& slots = _roster.Slots(faction);
    for (Market::Listing const& listing : _fillScratch)
    {
        Market::PostOrder order;
        order.itemIdx = listing.itemIdx;
        order.count = listing.count;
        order.unitPrice = listing.perUnit;
        order.listings = 1;
        order.expireAt = listing.expire;
        order.hours = 48;
        for (uint32 slot = 0; slot < slots.size(); ++slot)
        {
            if (slots[slot].GetCounter() == listing.owner)
            {
                order.botSlot = slot;
                break;
            }
        }
        _pending.push_back({faction, order});
    }
    fill.result = static_cast<uint32>(_fillScratch.size());
    LOG_INFO(
        "module",
        "AuctionSim: market fill house {}: {} simulated posts, {} sales, {} survivors, {} seller listings already "
        "up, {} listings queued for creation, {} us of simulation",
        Market::FactionHouse(faction),
        fill.run.Posted(),
        fill.run.Sold(),
        fill.run.Survivors(),
        fill.run.SellersUp(),
        fill.result,
        fill.micros);
}

MarketService::FillStatus MarketService::GetFillStatus(size_t faction) const
{
    Fill const& fill = _fills[faction];
    FillStatus status;
    status.running = fill.running;
    status.stepsDone = fill.run.StepsDone();
    status.stepsTotal = fill.run.StepsTotal();
    status.result = fill.result;
    status.micros = fill.micros;
    return status;
}

void MarketService::StepHouse(size_t faction)
{
    auto start = std::chrono::steady_clock::now();
    Market::Faction const& fac = _data.factions[faction];
    Market::Engine& engine = _engines[faction];
    AuctionHouseId const houseId = HouseOf(faction);
    HouseStats stats;

    // One pass over the house: every auction of an item the market knows, all owners.
    std::vector<Market::Listing>& listings = engine.Listings();
    Snapshot(faction, listings);
    stats.listingsSeen = static_cast<uint32>(listings.size());
    engine.BuildState(fac.items.size());

    // 1. Posts, priced from the state above; created now up to the tick budget.
    _orders.clear();
    engine.PlanPosts(fac, BotsInUse(faction), _rng, _orders);
    stats.postEvents = static_cast<uint32>(_orders.size());
    if (!_orders.empty())
    {
        auto trans = CharacterDatabase.BeginTransaction();
        for (Market::PostOrder& order : _orders)
        {
            uint32 made = _budgetLeft > 0 ? CreateListings(faction, order, _budgetLeft, trans, true) : 0;
            _budgetLeft -= made;
            stats.listingsCreated += made;
            if (order.listings > 0)
            {
                stats.listingsCarried += order.listings;
                _pending.push_back({faction, order});
            }
        }
        CharacterDatabase.CommitTransaction(trans);
        if (stats.listingsCreated > 0)
        {
            engine.BuildState(fac.items.size());  // this step's buyers see this step's posts
        }
    }

    // 2. Buyers, queued so their purchases spread over the next 20 minutes.
    _claims.clear();
    time_t const now = GameTime::GetGameTime().count();
    stats.buyers = engine.PlanBuys(fac, static_cast<double>(_scale) * kStepHours, Weekday(now), _rng, _claims);
    for (uint32 index : _claims)
    {
        Market::Listing const& listing = listings[index];
        if (_buying.EnqueueBuyout(
                listing.auctionId, houseId, AuctionPricing::RollBuyTime(static_cast<time_t>(listing.expire), now)))
        {
            stats.buysQueued++;
        }
    }

    stats.micros =
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
    _stats[faction] = stats;
    LOG_DEBUG(
        "module",
        "AuctionSim: market step house {}: {} listings seen, {} post events, {} listings created, {} carried, "
        "{} buyers, {} buys queued, {} us",
        Market::FactionHouse(faction),
        stats.listingsSeen,
        stats.postEvents,
        stats.listingsCreated,
        stats.listingsCarried,
        stats.buyers,
        stats.buysQueued,
        stats.micros);
}

uint32 MarketService::CreateListings(
    size_t faction,
    Market::PostOrder& order,
    uint32 budget,
    CharacterDatabaseTransaction& trans,
    bool recordInEngine)
{
    std::vector<ObjectGuid> const& slots = _roster.Slots(faction);
    if (slots.empty())
    {
        order.listings = 0;  // no sellers any more (reload shrank the roster): drop it
        return 0;
    }
    Market::Faction const& fac = _data.factions[faction];
    if (order.itemIdx >= fac.items.size())
    {
        order.listings = 0;
        return 0;
    }
    ObjectGuid const owner = slots[order.botSlot % slots.size()];
    uint32 const itemId = fac.items[order.itemIdx].itemId;
    AuctionHouseId const houseId = HouseOf(faction);
    AuctionHouseEntry const* houseEntry = sAuctionMgr->GetAuctionHouseEntryFromHouse(houseId);
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
    time_t const now = GameTime::GetGameTime().count();
    uint32 const duration = order.hours * 3600;
    // A fill survivor keeps its simulated expiry; one about to lapse isn't worth creating.
    time_t const expireTime = order.expireAt ? static_cast<time_t>(order.expireAt) : now + duration;
    if (expireTime <= now + 60)
    {
        order.listings = 0;
        return 0;
    }

    uint32 made = 0;
    while (order.listings > 0 && made < budget)
    {
        --order.listings;

        // As AuctionListingService::ListOneItem: the item only ever lives in the house,
        // created ownerless with the seller's guid stamped on, saved in `trans`.
        Item* item = Item::CreateItem(itemId, order.count, nullptr);
        if (!item)
        {
            continue;
        }
        item->SetOwnerGUID(owner);
        CraftedItems::SignIfCrafted(item, owner);  // "<Made by seller>", as a player's craft
        uint32 const count = item->GetCount();
        uint32 const buyout = static_cast<uint32>(std::min<uint64>(uint64(order.unitPrice) * count, kMaxBuyout));

        AuctionEntry* auction = new AuctionEntry();
        auction->Id = sObjectMgr->GenerateAuctionID();
        auction->houseId = houseId;
        auction->item_guid = item->GetGUID();
        auction->item_template = itemId;
        auction->itemCount = count;
        auction->owner = owner;
        // No bid below the buyout: the tables are buyout prices, and the reference has
        // no bidding. A bid of the full buyout is a buyout to the core.
        auction->startbid = buyout;
        auction->buyout = buyout;
        auction->bid = 0;
        // What the core would take from a player posting this. Recorded, not debited:
        // the sellers hold no gold, and every mail to them (sale proceeds with this
        // deposit back, or the expired item) is discarded.
        auction->deposit = AuctionHouseMgr::GetAuctionDeposit(houseEntry, duration, item, count);
        auction->expire_time = expireTime;
        auction->auctionHouseEntry = houseEntry;

        item->SaveToDB(trans);
        sAuctionMgr->AddAItem(item);
        house->AddAuction(auction);
        auction->SaveToDB(trans);
        ++made;

        if (recordInEngine)
        {
            Market::Listing listing;
            listing.itemIdx = order.itemIdx;
            listing.count = count;
            listing.perUnit = buyout / count;
            listing.owner = owner.GetCounter();
            listing.auctionId = auction->Id;
            listing.expire = static_cast<uint32>(auction->expire_time);
            listing.flags = Market::Listing::kBuyable;
            _engines[faction].Listings().push_back(listing);
        }
    }
    return made;
}

void MarketService::DrainPending()
{
    auto trans = CharacterDatabase.BeginTransaction();
    while (_pendingHead < _pending.size() && _budgetLeft > 0)
    {
        PendingPost& post = _pending[_pendingHead];
        _budgetLeft -= CreateListings(post.faction, post.order, _budgetLeft, trans, false);
        if (post.order.listings == 0)
        {
            ++_pendingHead;
        }
    }
    CharacterDatabase.CommitTransaction(trans);

    if (_pendingHead == _pending.size())
    {
        _pending.clear();  // keeps capacity
        _pendingHead = 0;
    }
}
