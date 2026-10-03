#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "ASConfig.h"
#include "AuctionBuyingService.h"
#include "AuctionHouseMgr.h"
#include "AuctionListingService.h"
#include "AuctionSimTests.h"
#include "AuctionSimVersion.h"
#include "Bot.h"
#include "MarketBots.h"
#include "MarketData.h"
#include "MarketService.h"
#include "Player.h"
#include "ScriptMgr.h"

class AuctionSim : public WorldScript
{

public:
    AuctionSim();
    static AuctionSim* instance() { return _instance; }

    void OnStartup() override;

    void OnUpdate(uint32 diff) override;
    void ScanAuctions(AuctionHouseId _id);
    // The 30-minute pass for the configured mode: a Replay scan of both houses, or one
    // Market step. Backs the timer, ".auctionsim scan" and the addon's Scan button.
    void RunScan();
    void DeleteAuctions();
    uint32 CleanOverCapAuctions();
    std::vector<AuctionSimTests::TestResult> RunTests();
    std::vector<AuctionBuyingService::QueuedPurchase> const& GetBuyQueue() const { return buyingService->GetQueue(); }

    // Executes every queued buy/bid immediately and returns how many ran. Backs the
    // ".auctionsim runqueue" command and the addon's "Run Queue" button.
    size_t RunQueue() { return buyingService ? buyingService->DrainQueue() : 0; }

    // Buy-queue summary for the ".auctionsim showqueue" command and the addon's
    // Show Queue button, so the "soonest-due at the back" ordering lives in one place.
    struct BuyQueueStatus
    {
        size_t size = 0;
        time_t nextBuyInSeconds = 0;
        time_t lastBuyInSeconds = 0;
    };
    BuyQueueStatus GetBuyQueueStatus(time_t now) const;

    // GetBotPlayer() is non-null only while the bot runs. config is loaded at startup
    // regardless of isEnabled, so GetConfig() is null only on a dat parse failure.
    Player* GetBotPlayer() const { return bot ? bot->GetPlayer().get() : nullptr; }
    ASConfig* GetConfig() const { return config.get(); }

    // --- Versioning (evaluated once in OnStartup) ---------------------------------
    // The GM's auctionsim.conf carries an AuctionSim.ConfigVersion older than this
    // build expects: the module still runs (missing keys fall back to defaults) but
    // warns the GM. Data-outdated means auctionsim.dat's stamp didn't match, in
    // which case the module also refuses to run (no market data).
    static constexpr char const* ModuleVersion() { return AUCTIONSIM_VERSION; }
    bool IsConfigOutdated() const { return _configOutdated; }
    uint32 ConfigHaveVersion() const { return _configHaveVer; }
    uint32 ConfigNeedVersion() const { return _configNeedVer; }
    bool IsDataOutdated() const { return _dataOutdated; }
    uint32 DataHaveVersion() const { return _dataHaveVer; }
    uint32 DataNeedVersion() const { return _dataNeedVer; }

    // Low GUID of the bot's character, or 0 when no bot is running. Cheap accessor
    // for the mail hook -- reads the in-memory id, never re-parses config.
    uint32 GetBotCharacterLowGuid() const { return bot ? bot->GetCharacterID() : 0; }

    // --- Market mode -------------------------------------------------------------
    // A named market seller's character (any mode: leftovers stay guarded after a
    // switch back to Replay).
    bool IsMarketBot(uint32 lowGuid) const { return marketRoster.IsBot(lowGuid); }
    // The buyer bot or a market seller: every mail to these is discarded.
    bool IsModuleCharacter(uint32 lowGuid) const;
    bool IsMarketMode() const { return config && config->marketMode; }
    MarketService* GetMarket() const { return market.get(); }
    Market::Data const* GetMarketData() const { return marketData.get(); }
    // Market mode was asked for but auctionsim_market.dat is missing, outdated or
    // malformed; the module refuses to run. Have/need are schema versions (have 0 =
    // missing or unstamped); MarketError() is the loader's reason.
    bool IsMarketUnavailable() const { return _marketUnavailable; }
    uint32 MarketHaveVersion() const { return _marketHaveVer; }
    uint32 MarketNeedVersion() const { return AUCTIONSIM_MARKET_VERSION; }
    std::string const& MarketError() const { return _marketError; }
    // Re-reads auctionsim.conf and auctionsim_market.dat and re-resolves the sellers
    // (".auctionsim market reload"). False, with the reason in `note`, on failure.
    bool ReloadMarket(std::string& note);

    // Starts the bot, or swaps it to the character in auctionsim.conf, with no
    // restart. reloadConfig re-reads the .conf first (for values the addon just
    // wrote). Returns false, leaving any running bot untouched, if config won't load
    // or the configured ids don't resolve.
    bool StartOrReloadBot(bool reloadConfig = true);

    bool isEnabled;
    bool startupScan;  // cached so the addon bridge can read it back live

private:
    // ASConfigWriter can only edit a real auctionsim.conf, so create one from the
    // .dist on first run if it's missing. Returns true if it just created the file
    // (ConfigMgr then needs a reload to pick up its values).
    bool EnsureConfigFileExists();

    // Reads AuctionSim.ConfigVersion from the live auctionsim.conf and sets the
    // _config* fields if it is behind AUCTIONSIM_CONFIG_VERSION.
    void EvaluateConfigVersion();

    // Loads auctionsim_market.dat into marketData, or records why not.
    bool LoadMarketData();

    static AuctionSim* _instance;
    std::unique_ptr<Bot> bot;
    // Old bots kept alive rather than destroyed: the headless Player is only safe to
    // tear down at shutdown.
    std::vector<std::unique_ptr<Bot>> retiredBots;
    std::unique_ptr<ASConfig> config;
    std::unique_ptr<AuctionListingService> listingService;
    std::unique_ptr<AuctionBuyingService> buyingService;
    Market::BotRoster marketRoster;
    std::unique_ptr<Market::Data> marketData;
    std::unique_ptr<MarketService> market;
    uint32 scanTimer = 0;

    bool _configOutdated = false;
    uint32 _configHaveVer = 0;
    uint32 _configNeedVer = 0;
    bool _dataOutdated = false;
    uint32 _dataHaveVer = 0;
    uint32 _dataNeedVer = 0;
    bool _marketUnavailable = false;
    uint32 _marketHaveVer = 0;
    std::string _marketError;
};
class AuctionSimMailManager : public MailScript
{
public:
    // scoped to the one hook we use (an empty list would enable them all)
    AuctionSimMailManager() : MailScript("AuctionSimMailManager", {MAILHOOK_ON_BEFORE_MAIL_DRAFT_SEND_MAIL_TO}) {}

    void OnBeforeMailDraftSendMailTo(
        MailDraft* mailDraft,
        MailReceiver const& receiver,
        MailSender const& sender,
        MailCheckMask& checked,
        uint32& deliver_delay,
        uint32& custom_expiration,
        bool& deleteMailItemsFromDB,
        bool& sendMail) override;
};

// Market sellers never play: a login on one (say after a GM reset its account's
// password) is kicked straight away.
class AuctionSimMarketGuard : public PlayerScript
{
public:
    AuctionSimMarketGuard() : PlayerScript("AuctionSimMarketGuard", {PLAYERHOOK_ON_LOGIN}) {}

    void OnPlayerLogin(Player* player) override;
};
