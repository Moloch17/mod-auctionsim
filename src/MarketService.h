#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "AuctionHouseMgr.h"
#include "DatabaseEnvFwd.h"
#include "MarketBots.h"
#include "MarketData.h"
#include "MarketEngine.h"
#include "MarketRng.h"
#include "ObjectGuid.h"

class ASConfig;
class AuctionBuyingService;

// Market mode (AuctionSim.Mode = Market): named bot sellers post from the learned
// tables in auctionsim_market.dat, and buyers drawn from the same tables buy the
// cheapest acceptable listing of any owner through the bot's buy queue. One Step()
// per 30-minute timer runs ml/MARKET_FORMAT.md's runtime for both houses.
class MarketService
{
public:
    // Contract's step length: the module's 30-minute timer.
    static constexpr double kStepHours = 0.5;
    // Most auctions created in one world tick, both houses and carried-over posts
    // together; a bigger step's remainder is created on the following ticks, so a
    // large Market.Scale never stalls one tick.
    static constexpr uint32 kMaxListingsPerTick = 100;

    // Reads and links auctionsim_market.dat, then resolves it against this realm's
    // item_template, vendors and level caps. On failure `error` says why and
    // `foundVersion` is the file's stamp (0 if missing / unstamped).
    static bool LoadFile(
        std::string const& path, ASConfig const& config, Market::Data& out, std::string& error, uint32& foundVersion);

    MarketService(
        Market::Data& data, Market::BotRoster& roster, AuctionBuyingService& buying, ObjectGuid buyerGuid);

    // Market.Scale; applies to the next step.
    void SetScale(float scale);
    float GetScale() const { return _scale; }

    // Resolves (creating if needed) up to `bots` sellers per faction. Pending means
    // the bot accounts are still being created; Update() keeps retrying.
    Market::BotRoster::Result SetupBots(uint32 bots);
    bool IsReady() const { return _ready; }
    std::string const& SetupNote() const { return _setupNote; }

    // Per world tick: retries a pending bot setup and creates carried-over posts.
    void Update(uint32 diff);

    // One market step on both houses. No-op until IsReady().
    void Step();

    struct HouseStats
    {
        uint32 listingsSeen = 0;
        uint32 postEvents = 0;
        uint32 listingsCreated = 0;
        uint32 listingsCarried = 0;
        uint32 buyers = 0;
        uint32 buysQueued = 0;
        long long micros = 0;
    };
    HouseStats const& LastStats(size_t faction) const { return _stats[faction]; }
    size_t PendingListings() const;
    uint32 BotsInUse(size_t faction) const { return static_cast<uint32>(_roster.Slots(faction).size()); }
    std::vector<ObjectGuid> const& Slots(size_t faction) const { return _roster.Slots(faction); }

    // Monday = 0, server local time, as WEEKDAY expects.
    static size_t Weekday(time_t now);

private:
    struct PendingPost
    {
        size_t faction = 0;
        Market::PostOrder order;
    };

    void StepHouse(size_t faction);
    // Creates up to `budget` of the order's listings in `trans`; returns how many.
    uint32 CreateListings(
        size_t faction,
        Market::PostOrder& order,
        uint32 budget,
        CharacterDatabaseTransaction& trans,
        bool recordInEngine);
    void DrainPending();

    Market::Data& _data;
    Market::BotRoster& _roster;
    AuctionBuyingService& _buying;
    ObjectGuid _buyerGuid;

    float _scale = 0.1f;
    uint32 _wantedBots = 0;
    bool _ready = false;
    bool _setupPending = false;
    std::string _setupNote;
    uint32 _setupRetryTimer = 0;

    Market::Rng _rng;
    Market::Engine _engine;
    std::vector<Market::PostOrder> _orders;
    std::vector<uint32> _claims;
    std::vector<PendingPost> _pending;
    size_t _pendingHead = 0;
    uint32 _budgetLeft = kMaxListingsPerTick;  // this tick's remaining auction creations
    std::array<HouseStats, Market::kFactions> _stats{};
};
