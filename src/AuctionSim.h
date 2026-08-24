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
    bool isEnabled;

private:
    static AuctionSim* _instance;
    std::unique_ptr<Bot> bot;
    std::unique_ptr<ASConfig> config;
    std::unique_ptr<AuctionListingService> listingService;
    std::unique_ptr<AuctionBuyingService> buyingService;
    uint32 scanTimer = 0;
};
class AuctionSimMailManager : public MailScript
{
public:
    AuctionSimMailManager() : MailScript("AuctionSimMailManager") {}

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
