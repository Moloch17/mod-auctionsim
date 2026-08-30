#pragma once
#include <memory>
#include <vector>
#include "ASConfig.h"
#include "AuctionBuyingService.h"
#include "AuctionHouseMgr.h"
#include "AuctionListingService.h"
#include "AuctionSimTests.h"
#include "Bot.h"
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
    void DeleteAuctions();
    uint32 CleanOverCapAuctions();
    std::vector<AuctionSimTests::TestResult> RunTests();
    std::vector<AuctionBuyingService::QueuedPurchase> const& GetBuyQueue() const { return buyingService->GetQueue(); }

    // GetBotPlayer() is non-null only while the bot runs. config is loaded at startup
    // regardless of isEnabled, so GetConfig() is null only on a dat parse failure.
    Player* GetBotPlayer() const { return bot ? bot->GetPlayer().get() : nullptr; }
    ASConfig* GetConfig() const { return config.get(); }

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

    static AuctionSim* _instance;
    std::unique_ptr<Bot> bot;
    // Old bots kept alive rather than destroyed: the headless Player is only safe to
    // tear down at shutdown.
    std::vector<std::unique_ptr<Bot>> retiredBots;
    std::unique_ptr<ASConfig> config;
    std::unique_ptr<AuctionListingService> listingService;
    std::unique_ptr<AuctionBuyingService> buyingService;
    uint32 scanTimer = 0;
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
