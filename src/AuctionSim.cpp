#include "AuctionSim.h"
#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "Config.h"
#include "CraftedItems.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Log.h"
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "Mail.h"
#include "MarketService.h"
#include "StringFormat.h"
#include "ScriptMgr.h"
#include "World.h"
#include "WorldConfig.h"

namespace
{
    // Collects every module-owned auction on `houseId` (isOwner: the buyer bot, or any
    // market seller) that `shouldRemove` accepts, then deletes them in a second pass --
    // so the live house map is never mutated while it is being iterated, and it is never
    // copied. Returns the number removed.
    //
    // An auction that already carries a bid is left alone: this path just drops the
    // row (no SendAuctionCancelledToBidderMail), so removing a bid-on auction would
    // strand the bidder's escrowed gold. Those clear themselves when they expire or
    // are won.
    template <typename OwnerPredicate, typename Predicate>
    uint32 RemoveBotAuctionsIf(
        AuctionHouseId houseId,
        OwnerPredicate isOwner,
        SQLTransaction<CharacterDatabaseConnection>& trans,
        Predicate shouldRemove)
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);

        std::vector<AuctionEntry*> toRemove;
        for (auto const& entry : house->GetAuctions())
        {
            AuctionEntry* auction = entry.second;
            if (isOwner(auction->owner) && auction->bid == 0 && shouldRemove(auction))
            {
                toRemove.push_back(auction);
            }
        }

        for (AuctionEntry* auction : toRemove)
        {
            auction->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
            house->RemoveAuction(auction);
        }
        return static_cast<uint32>(toRemove.size());
    }

    bool IsBotCharacter(uint32 lowGuid)
    {
        // Read the in-memory ids rather than re-parsing config: this hook fires for
        // every mail delivered server-wide.
        AuctionSim* sim = AuctionSim::instance();
        return sim && sim->IsModuleCharacter(lowGuid);
    }
}

AuctionSim* AuctionSim::_instance = nullptr;

AuctionSim::AuctionSim() : WorldScript("AuctionSim")
{
    _instance = this;
    isEnabled = sConfigMgr->GetOption<bool>("AuctionSim.Enabled", false);
    startupScan = sConfigMgr->GetOption<bool>("AuctionSim.StartupScan", false);
}

bool AuctionSim::EnsureConfigFileExists()
{
    std::filesystem::path dir = std::filesystem::path(sConfigMgr->GetConfigPath()) / "modules";
    std::filesystem::path livePath = dir / "auctionsim.conf";
    std::filesystem::path distPath = dir / "auctionsim.conf.dist";

    std::error_code ec;
    if (std::filesystem::exists(livePath, ec))
    {
        return false;
    }
    if (!std::filesystem::exists(distPath, ec))
    {
        LOG_ERROR("module", "AuctionSim: neither auctionsim.conf nor auctionsim.conf.dist found in {}", dir.string());
        return false;
    }

    std::filesystem::copy_file(distPath, livePath, ec);
    if (ec)
    {
        LOG_ERROR("module", "AuctionSim: couldn't create {}: {}", livePath.string(), ec.message());
        return false;
    }
    LOG_INFO("module", "AuctionSim: created auctionsim.conf from auctionsim.conf.dist");
    return true;
}

void AuctionSim::EvaluateConfigVersion()
{
    uint32 liveVer = sConfigMgr->GetOption<uint32>("AuctionSim.ConfigVersion", 0);
    if (liveVer >= AUCTIONSIM_CONFIG_VERSION)
    {
        return;
    }

    _configOutdated = true;
    _configHaveVer = liveVer;
    _configNeedVer = AUCTIONSIM_CONFIG_VERSION;
    LOG_WARN(
        "module",
        "AuctionSim: auctionsim.conf is out of date (config schema v{} < v{}); a current "
        "auctionsim.conf.dist is in {}modules/ -- merge the new keys into auctionsim.conf. Missing keys "
        "fall back to built-in defaults until you do.",
        liveVer,
        AUCTIONSIM_CONFIG_VERSION,
        sConfigMgr->GetConfigPath());
}

void AuctionSim::OnStartup()
{
    if (EnsureConfigFileExists())
    {
        // File didn't exist when ConfigMgr loaded module configs; pull it in now so
        // ASConfig and Bot below read real values instead of defaults.
        sConfigMgr->Reload();
    }

    EvaluateConfigVersion();
    CraftedItems::Load();

    // Load auctionsim.dat unconditionally: the addon shows/edits the listing table
    // whether or not the module is enabled.
    {
        bool datOk = true;
        config = std::make_unique<ASConfig>(sConfigMgr->GetConfigPath() + "/modules/auctionsim.dat", datOk);
        if (!datOk)
        {
            // A version-stamp mismatch (or a pre-stamp / missing file, found == 0) is
            // an "out of date" condition the GM warning names explicitly; other load
            // failures (corrupt rows) just get the generic error.
            uint32 found = config ? config->GetFoundDataVersion() : 0;
            if (found != AUCTIONSIM_DATA_VERSION)
            {
                _dataOutdated = true;
                _dataHaveVer = found;
                _dataNeedVer = AUCTIONSIM_DATA_VERSION;
            }
            LOG_ERROR("module", "AuctionSim: auctionsim.dat failed to load");
            config.reset();
        }
    }

    // Any mode: market sellers from an earlier Market run keep their mail swallowed.
    marketRoster.LoadExisting();

    // Market mode's tables load whether or not the module is enabled, so enabling from
    // the addon needs no restart -- and a bad file is reported at once.
    if (config && config->marketMode)
    {
        LoadMarketData();
    }

    if (!isEnabled)
    {
        // The addon still works while disabled (replies are self-whispers), so a GM
        // can configure everything and enable without a restart.
        LOG_WARN("module", "AuctionSim is disabled!");
        return;
    }

    if (!config)
    {
        LOG_ERROR("module", "AuctionSim: disabling -- auctionsim.dat is required to run");
        isEnabled = false;
        return;
    }

    if (config->marketMode && !marketData)
    {
        LOG_ERROR("module", "AuctionSim: disabling -- Market mode needs a current auctionsim_market.dat");
        isEnabled = false;
        return;
    }

    if (!StartOrReloadBot(false))  // config is fresh at boot; no reload
    {
        isEnabled = false;
        return;
    }

    if (this->startupScan)
    {
        RunScan();
        LOG_INFO("module", "AuctionSim: Startup complete");
    }
}

bool AuctionSim::LoadMarketData()
{
    std::string path = sConfigMgr->GetConfigPath() + "/modules/auctionsim_market.dat";
    auto data = std::make_unique<Market::Data>();
    std::string error;
    uint32 found = 0;
    if (!MarketService::LoadFile(path, *config, *data, error, found))
    {
        _marketUnavailable = true;
        _marketHaveVer = found;
        _marketError = error;
        marketData.reset();
        LOG_ERROR("module", "AuctionSim: Market mode can't run -- auctionsim_market.dat: {}", error);
        return false;
    }
    _marketUnavailable = false;
    _marketHaveVer = found;
    _marketError.clear();
    marketData = std::move(data);
    return true;
}

bool AuctionSim::IsModuleCharacter(uint32 lowGuid) const
{
    if (lowGuid == 0)
    {
        return false;
    }
    return lowGuid == GetBotCharacterLowGuid() || marketRoster.IsBot(lowGuid);
}

void AuctionSim::RunScan()
{
    if (market)
    {
        market->Step();
        return;
    }
    if (IsMarketMode())
    {
        return;  // Market mode without a running market (purged): never fall back to Replay
    }
    ScanAuctions(AuctionHouseId::Alliance);
    ScanAuctions(AuctionHouseId::Horde);
}

bool AuctionSim::ReloadMarket(std::string& note)
{
    if (!sConfigMgr->Reload())
    {
        note = "couldn't reload auctionsim.conf";
        return false;
    }
    bool datOk = true;
    auto fresh = std::make_unique<ASConfig>(sConfigMgr->GetConfigPath() + "/modules/auctionsim.dat", datOk);
    if (!datOk)
    {
        note = "auctionsim.dat failed to load; nothing changed";
        return false;
    }
    if (!fresh->marketMode)
    {
        note = "AuctionSim.Mode is Replay in auctionsim.conf; restart to switch modes";
        return false;
    }
    if (!IsMarketMode())
    {
        note = "the module started in Replay mode; restart to switch modes";
        return false;
    }

    // The services hold references into marketData and config: drop the market first.
    market.reset();
    _marketPurged = false;
    config->marketBots = fresh->marketBots;
    config->marketScale = fresh->marketScale;
    config->replayBidding = fresh->replayBidding;
    if (!LoadMarketData())
    {
        note = Acore::StringFormat("auctionsim_market.dat: {}", _marketError);
        isEnabled = false;
        return false;
    }
    if (!isEnabled)
    {
        note = "market data reloaded; the module is disabled";
        return true;
    }
    if (!StartOrReloadBot(false))
    {
        note = "market data reloaded but the market couldn't start; see the server log";
        isEnabled = false;
        return false;
    }
    note = market && !market->IsReady() ? market->SetupNote() : "market reloaded";
    return true;
}

bool AuctionSim::StartOrReloadBot(bool reloadConfig)
{
    if (!config)
    {
        return false;
    }

    // Checked here rather than only at startup, so enabling from the addon can't
    // bypass it. With two-side auctions, both factions share one house.
    if (sWorld->getBoolConfig(CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION))
    {
        LOG_ERROR("module", "AuctionSim: Two sided auction interaction is not allowed");
        return false;
    }

    // "Set Bot Char" just rewrote BotAccountID/BotCharacterID; reload so Bot's ctor
    // and the mail hook see them. A failed reload leaves any running bot alone.
    if (reloadConfig && !sConfigMgr->Reload())
    {
        return false;
    }
    if (reloadConfig)
    {
        config->LoadReplayBidding();
    }

    // Market mode without its tables never starts (the GM is told at login).
    if (config->marketMode && !marketData)
    {
        LOG_ERROR("module", "AuctionSim: Market mode needs a current auctionsim_market.dat");
        return false;
    }

    // Throwaway flag: a bad/unset id must not clear the module's isEnabled or kill a
    // running bot.
    bool built = true;
    auto newBot = std::make_unique<Bot>(built);
    if (!built || !newBot->GetPlayer())
    {
        return false;
    }

    // Retire the old bot rather than destroying it (its headless Player is only ever
    // torn down at shutdown); rebuild the services, which hold a Bot&.
    if (bot)
    {
        retiredBots.push_back(std::move(bot));
    }
    bot = std::move(newBot);
    market.reset();  // holds a reference to the old buying service
    listingService = std::make_unique<AuctionListingService>(*bot, *config);
    buyingService = std::make_unique<AuctionBuyingService>(*bot);
    buyingService->SetBiddingEnabled(config->replayBidding);

    if (config->marketMode)
    {
        market = std::make_unique<MarketService>(
            *marketData, marketRoster, *buyingService, bot->GetPlayer()->GetGUID());
        market->SetScale(config->marketScale);
        if (market->SetupBots(config->marketBots) == Market::BotRoster::Result::Failed)
        {
            LOG_ERROR("module", "AuctionSim: market sellers couldn't be set up: {}", market->SetupNote());
            market.reset();
            return false;
        }
    }

    LOG_INFO(
        "module",
        "AuctionSim: bot active (character {}), {} mode",
        bot->GetCharacterID(),
        ASConfig::ModeName(config->marketMode));
    return true;
}

void AuctionSim::OnUpdate(uint32 diff)
{
    // isEnabled can be set before a bot exists (enabled via the addon), so check both
    if (!this->isEnabled || !buyingService) return;

    scanTimer += diff;

    if (scanTimer >= AuctionPricing::kScanIntervalSeconds * 1000)
    {
        RunScan();
        scanTimer = 0;
    }

    if (market)
    {
        market->Update(diff);
    }

    buyingService->ProcessDueQueue();
}

void AuctionSim::ScanAuctions(AuctionHouseId _AuctionHouseId)
{
    // const& -- GetAuctions() returns the live map by reference; a by-value `auto`
    // would deep-copy every auction node on the house each scan.
    auto const& auctions = sAuctionMgr->GetAuctionsMapByHouseId(_AuctionHouseId)->GetAuctions();
    int auctionTable[MAX_ITEM_CLASS][MAX_ITEM_QUALITY] = {};
    std::unordered_map<uint32, int> itemAuctionCount;

    ObjectGuid const botGuid = bot->GetPlayer()->GetGUID();

    buyingService->SetBiddingEnabled(config->replayBidding);
    buyingService->RollTolerance();
    buyingService->PruneBidValuations();

    // Cheapest per-unit buyout of each item on this house: a player would not bid
    // more than it costs to buy the same item outright two rows down.
    std::unordered_map<uint32, uint32> cheapestBuyoutPerUnit;
    for (auto const& entry : auctions)
    {
        AuctionEntry const* auction = entry.second;
        if (auction->buyout == 0)
        {
            continue;
        }
        uint32 perUnit = auction->buyout / std::max<uint32>(1, auction->itemCount);
        auto [cheapest, inserted] = cheapestBuyoutPerUnit.emplace(auction->item_template, perUnit);
        if (!inserted && perUnit < cheapest->second)
        {
            cheapest->second = perUnit;
        }
    }

    for (auto it = auctions.begin(); it != auctions.end(); ++it)
    {
        AuctionEntry* auction = it->second;
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
        {
            LOG_WARN(
                "module",
                "AuctionSim: auction {} references item {} not found in item_template, skipping",
                auction->Id,
                auction->item_template);
            continue;
        }

        auctionTable[proto->Class][proto->Quality]++;
        itemAuctionCount[auction->item_template]++;

        // The bot must never BUY its own listings (that's just churn), but it does
        // OUTBID a real player who has bid on one -- on a bot-heavy realm the bot's
        // own auctions are most of the AH, and the outbid refund goes to the player
        // while every bot-directed auction mail is swallowed, so there is no cheese.
        bool const isBotOwned = (auction->owner == botGuid);

        ScannedItem const* scannedItem =
            config->FindScannedItem(_AuctionHouseId, proto->Class, proto->Quality, auction->item_template);
        if (!scannedItem)
        {
            continue;
        }

        // Never touch grey items -- vendor trash that only shows up on the AH as
        // gold-cheese bait. (Grey auctions are still counted above, so the listing
        // side is unaffected.)
        if (!AuctionPricing::IsBuyableQuality(proto->Quality))
        {
            continue;
        }

        // Never pay more per unit than it would cost to buy the item straight from a
        // vendor -- by buyout or by bid. The cap only applies when a vendor actually
        // stocks the item (npc_vendor), without a stock limit or token cost: a BuyPrice
        // left on an item no vendor sells is stale DB data, not a real floor (0 disables
        // the check). BuyPrice buys BuyCount items; the cap is the price of one.
        uint32 vendorBuyPrice = (config->IsVendorSold(auction->item_template) && proto->BuyPrice > 0)
            ? AuctionPricing::VendorUnitBuyPrice(proto->BuyPrice, proto->BuyCount)
            : 0;

        // Buyout consideration -- only for real buyout auctions the bot doesn't own.
        // A bid-only auction has buyout == 0, which would give pricePerItem == 0,
        // pass "<= marketPrice", and get "bought" for nothing; guard against that.
        if (!isBotOwned && auction->buyout > 0)
        {
            uint32 pricePerItem = auction->buyout / auction->itemCount;
            if (AuctionPricing::IsWithinVendorBuyPrice(pricePerItem, vendorBuyPrice))
            {
                buyingService->ConsiderForPurchase(
                    auction, pricePerItem, scannedItem->GetMarketPrice(), scannedItem->GetBuyCeiling());
            }
        }

        // Bid consideration -- whenever a real player holds the high bid (on any
        // auction, including the bot's own), or to open bidding on a player's auction
        // nobody has bid on. ConsiderForBid is a no-op if this auction was just
        // queued for buyout above (shared dedupe set), so on a non-bot auction an
        // acceptable buyout still wins over a bid.
        bool const playerHoldsBid = auction->bid > 0 && auction->bidder && auction->bidder != botGuid;
        bool const openable = auction->bid == 0 && !isBotOwned;
        if (AuctionPricing::MayBid(config->replayBidding, playerHoldsBid, openable))
        {
            AuctionBuyingService::BidLimits limits;
            limits.valuationLowPerUnit = scannedItem->GetBidValuationLow();
            limits.marketPerUnit = scannedItem->GetMarketPrice();
            limits.vendorBuyPrice = vendorBuyPrice;
            auto cheapest = cheapestBuyoutPerUnit.find(auction->item_template);
            limits.cheapestBuyoutPerUnit = cheapest != cheapestBuyoutPerUnit.end() ? cheapest->second : 0;
            buyingService->ConsiderForBid(auction, limits);
        }
    }

    buyingService->SortQueue();

    listingService->ListNewAuctions(_AuctionHouseId, auctionTable, itemAuctionCount);
}

std::vector<AuctionSimTests::TestResult> AuctionSim::RunTests()
{
    std::vector<AuctionSimTests::TestResult> results = AuctionSimTests::RunLogicTests(*bot, *config);

    std::vector<AuctionSimTests::TestResult> marketResults =
        AuctionSimTests::RunMarketTests(*config, marketData.get(), market.get());
    results.insert(results.end(), marketResults.begin(), marketResults.end());

    results.push_back(
        AuctionSimTests::RunLiveListingTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(AuctionSimTests::RunLiveListingTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    results.push_back(
        AuctionSimTests::RunLiveBuyingTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(AuctionSimTests::RunLiveBuyingTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    results.push_back(
        AuctionSimTests::RunLiveBiddingTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(AuctionSimTests::RunLiveBiddingTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    results.push_back(
        AuctionSimTests::RunLiveLevelCapTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(
        AuctionSimTests::RunLiveLevelCapTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    return results;
}

AuctionSim::BuyQueueStatus AuctionSim::GetBuyQueueStatus(time_t now) const
{
    auto const& queue = buyingService->GetQueue();
    if (queue.empty())
    {
        return {};
    }

    // SortQueue keeps the soonest-due purchase at the back and the furthest-due at
    // the front.
    return {queue.size(), queue.back().buyTime - now, queue.front().buyTime - now};
}

uint32 AuctionSim::CleanOverCapAuctions()
{
    if (!bot || !bot->GetPlayer() || !config)
    {
        return 0;
    }

    auto isOwner = [this](ObjectGuid owner) { return IsModuleCharacter(owner.GetCounter()); };
    auto trans = CharacterDatabase.BeginTransaction();
    uint32 removedCount = 0;

    auto isOverCap = [this](AuctionEntry const* auction) {
        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
        if (!proto)
        {
            return false;  // can't judge it -- leave it alone
        }
        return !AuctionPricing::IsWithinLevelCap(
            proto->RequiredLevel, proto->ItemLevel, config->maxRequiredLevel, config->maxItemLevel);
    };

    for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde})
    {
        removedCount += RemoveBotAuctionsIf(houseId, isOwner, trans, isOverCap);
    }

    CharacterDatabase.CommitTransaction(trans);
    LOG_INFO("module", "AuctionSim: cleaned {} over-cap auctions", removedCount);
    return removedCount;
}

void AuctionSim::DeleteAuctions()
{
    if (!bot || !bot->GetPlayer())
    {
        return;
    }

    auto isOwner = [this](ObjectGuid owner) { return IsModuleCharacter(owner.GetCounter()); };
    auto trans = CharacterDatabase.BeginTransaction();

    for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde})
    {
        RemoveBotAuctionsIf(houseId, isOwner, trans, [](AuctionEntry const*) { return true; });
    }

    CharacterDatabase.CommitTransaction(trans);
}

void AuctionSimMailManager::OnBeforeMailDraftSendMailTo(
    MailDraft* /*mailDraft*/,
    MailReceiver const& receiver,
    MailSender const& sender,
    MailCheckMask& /*checked*/,
    uint32& /*deliver_delay*/,
    uint32& /*custom_expiration*/,
    bool& deleteMailItemsFromDB,
    bool& sendMail)
{
    if (AuctionSim::ShouldSwallowMail(IsBotCharacter(receiver.GetPlayerGUIDLow()), sender))
    {
        // Sale proceeds, returned deposits, won / expired / cancelled items, outbid
        // refunds: none of it is wanted by a character that never logs in.
        sendMail = false;
        deleteMailItemsFromDB = true;
    }
}

AuctionSim::PurgeReport AuctionSim::PurgeMarket(bool confirm)
{
    PurgeReport report;
    uint32 const buyerAccount = sConfigMgr->GetOption<uint32>("AuctionSim.BotAccountID", 0);
    uint32 const buyerCharacter = sConfigMgr->GetOption<uint32>("AuctionSim.BotCharacterID", 0);
    Market::PurgePlan plan = Market::BotRoster::GatherPurge(buyerAccount, buyerCharacter);
    report.problems = plan.problems;
    if (!plan.problems.empty())
    {
        report.refused = true;
        return report;
    }

    std::unordered_set<uint32> guids;
    std::string guidList;
    for (Market::PurgeCharacter const& character : plan.characters)
    {
        guids.insert(character.guid);
        guidList += (guidList.empty() ? "" : ",") + std::to_string(character.guid);
    }
    report.accounts = static_cast<uint32>(plan.accounts.size());
    report.characters = static_cast<uint32>(plan.characters.size());

    for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral})
    {
        for (auto const& entry : sAuctionMgr->GetAuctionsMapByHouseId(houseId)->GetAuctions())
        {
            if (guids.count(entry.second->owner.GetCounter()))
            {
                report.auctions++;
                report.auctionsWithBids += entry.second->bid > 0 ? 1 : 0;
            }
        }
    }
    if (!guidList.empty())
    {
        if (QueryResult mails = CharacterDatabase.Query("SELECT COUNT(*) FROM mail WHERE receiver IN ({})", guidList))
        {
            report.mails = mails->Fetch()[0].Get<uint32>();
        }
    }
    if (!confirm || plan.accounts.empty())
    {
        return report;
    }

    // 1. Stop the market: no posting, carried-over posts, fills or market buys from
    //    here on. Market buyers are the only thing queued in Market mode.
    market.reset();
    _marketPurged = IsMarketMode();
    if (buyingService && IsMarketMode())
    {
        report.queuedDropped = static_cast<uint32>(buyingService->ClearQueue());
    }

    // 2. Every seller auction goes, as a cancel would: a bidder gets the bid back by
    //    the core's cancel mail (the item was the seller bot's and is destroyed). This
    //    must happen before the characters go: deleting a character drops its item
    //    rows (the auction items are owned by it) but never its auctions.
    auto trans = CharacterDatabase.BeginTransaction();
    for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral})
    {
        AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
        std::vector<AuctionEntry*> toRemove;
        for (auto const& entry : house->GetAuctions())
        {
            if (guids.count(entry.second->owner.GetCounter()))
            {
                toRemove.push_back(entry.second);
            }
        }
        for (AuctionEntry* auction : toRemove)
        {
            if (auction->bidder)
            {
                sAuctionMgr->SendAuctionCancelledToBidderMail(auction, trans);
            }
            auction->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
            house->RemoveAuction(auction);
        }
    }
    CharacterDatabase.CommitTransaction(trans);

    // 3. Forget the sellers before deleting them, so nothing routes to them meanwhile.
    marketRoster.Clear();

    // 4. Characters and accounts through the stock path: AccountMgr::DeleteAccount runs
    //    Player::DeleteFromDB(deleteFinally) on each character (which also returns any
    //    player-sent mail to its sender) and removes the account's auth rows.
    for (Market::PurgeAccount const& account : plan.accounts)
    {
        AccountOpResult result = AccountMgr::DeleteAccount(account.id);
        if (result != AOR_OK)
        {
            report.problems.push_back(Acore::StringFormat(
                "AccountMgr couldn't delete {} (error {})", account.name, static_cast<uint32>(result)));
        }
    }
    for (Market::PurgeCharacter const& character : plan.characters)
    {
        sCharacterCache->DeleteCharacterCacheEntry(
            ObjectGuid::Create<HighGuid::Player>(character.guid), character.name);
    }
    report.done = true;

    LOG_INFO(
        "module",
        "AuctionSim: market purge: deleted {} account(s), {} character(s), {} auction(s) ({} with bids, bidders "
        "refunded), {} mail(s) with the characters; dropped {} queued buy(s)",
        report.accounts,
        report.characters,
        report.auctions,
        report.auctionsWithBids,
        report.mails,
        report.queuedDropped);
    return report;
}

std::vector<std::string> AuctionSim::DescribePurge(PurgeReport const& report, bool confirm, bool marketMode)
{
    std::vector<std::string> lines;
    if (report.refused)
    {
        lines.push_back("Market purge refused -- the seller accounts hold something the module didn't create:");
        for (std::string const& problem : report.problems)
        {
            lines.push_back("  " + problem);
        }
        lines.push_back("Nothing was deleted.");
        return lines;
    }
    if (report.accounts == 0)
    {
        lines.push_back("Market purge: no market seller accounts found -- nothing to delete.");
        return lines;
    }
    lines.push_back(Acore::StringFormat(
        "Market purge {}: {} account(s), {} character(s), {} auction(s) ({} with bids -- bidders get their gold "
        "back by mail), {} mail(s).",
        report.done ? "done" : "would delete",
        report.accounts,
        report.characters,
        report.auctions,
        report.auctionsWithBids,
        report.mails));
    if (report.done)
    {
        lines.push_back(Acore::StringFormat(
            "The market is stopped until restart or \".auctionsim market reload\"; {} queued buy(s) dropped.",
            report.queuedDropped));
        for (std::string const& problem : report.problems)
        {
            lines.push_back("  " + problem);
        }
    }
    else if (confirm)
    {
        lines.push_back("Nothing was deleted.");
    }
    else
    {
        lines.push_back("Run \".auctionsim market purge confirm\" to delete them.");
    }
    if (marketMode)
    {
        lines.push_back("AuctionSim.Mode is Market: the next start or reload creates the sellers again. Set "
                        "AuctionSim.Mode = Replay (or disable the module) first to remove the module's footprint.");
    }
    return lines;
}

std::vector<std::string> AuctionSim::FillMarket(std::string_view which, bool& ok)
{
    ok = false;
    std::vector<std::string> lines;
    if (!market)
    {
        lines.push_back(_marketPurged ? "The market was purged; restart or reload it first."
                                      : "The market isn't running (AuctionSim.Mode = Market and enabled?).");
        return lines;
    }
    std::vector<size_t> slots;
    if (which.empty() || which == "both")
    {
        slots = {0, 1};
    }
    else if (which == "alliance")
    {
        slots = {0};
    }
    else if (which == "horde")
    {
        slots = {1};
    }
    else
    {
        lines.push_back("Usage: .auctionsim market fill [alliance|horde]");
        return lines;
    }
    for (size_t slot : slots)
    {
        std::string note;
        bool started = market->StartFill(slot, note);
        ok |= started;
        lines.push_back(Acore::StringFormat("{}: {}", slot == 0 ? "Alliance" : "Horde", note));
    }
    if (ok)
    {
        lines.push_back("Progress: .auctionsim market status");
    }
    return lines;
}

std::vector<std::string> AuctionSim::DescribeMarketStatus() const
{
    std::vector<std::string> lines;
    if (!IsMarketMode())
    {
        lines.push_back("AuctionSim is in Replay mode (AuctionSim.Mode = Replay).");
        return lines;
    }
    if (IsMarketUnavailable())
    {
        lines.push_back(Acore::StringFormat(
            "Market mode can't run: auctionsim_market.dat {} (file schema v{}, needs v{}).",
            MarketError(),
            MarketHaveVersion(),
            MarketNeedVersion()));
        return lines;
    }
    MarketService const* service = market.get();
    if (!service)
    {
        lines.push_back(
            IsMarketPurged()
                ? "Market mode: stopped by a purge until restart or \".auctionsim market reload\"."
                : "Market mode: data loaded, market not running (module disabled?).");
        return lines;
    }
    if (!service->IsReady())
    {
        lines.push_back(Acore::StringFormat("Market mode: setting up sellers -- {}", service->SetupNote()));
        return lines;
    }
    lines.push_back(Acore::StringFormat(
        "Market mode: scale {:g}, {} Alliance / {} Horde sellers, {} listing(s) waiting to post, buy queue {}.",
        service->GetScale(),
        service->BotsInUse(0),
        service->BotsInUse(1),
        service->PendingListings(),
        GetBuyQueue().size()));
    for (size_t faction = 0; faction < Market::kFactions; ++faction)
    {
        MarketService::HouseStats const& stats = service->LastStats(faction);
        lines.push_back(Acore::StringFormat(
            "  last step, {}: {} listings seen, {} post events, {} created ({} carried), {} buyers, {} buys "
            "queued, {} us",
            faction == 0 ? "Alliance" : "Horde",
            stats.listingsSeen,
            stats.postEvents,
            stats.listingsCreated,
            stats.listingsCarried,
            stats.buyers,
            stats.buysQueued,
            stats.micros));
        MarketService::FillStatus fill = service->GetFillStatus(faction);
        if (fill.running)
        {
            lines.push_back(
                Acore::StringFormat("  fill running: {}/{} simulated steps", fill.stepsDone, fill.stepsTotal));
        }
        else if (fill.stepsTotal > 0)
        {
            lines.push_back(Acore::StringFormat(
                "  last fill: {} listings queued ({} us of simulation)", fill.result, fill.micros));
        }
    }
    return lines;
}

void AuctionSimMarketGuard::OnPlayerLogin(Player* player)
{
    AuctionSim* sim = AuctionSim::instance();
    if (sim && player && player->GetSession() && sim->IsMarketBot(player->GetGUID().GetCounter()))
    {
        LOG_WARN("module", "AuctionSim: kicked a login on market seller {}", player->GetName());
        player->GetSession()->KickPlayer("AuctionSim market seller");
    }
}

void AddAuctionSimScripts()
{
    new AuctionSim();
    new AuctionSimMailManager();
    new AuctionSimMarketGuard();
}
