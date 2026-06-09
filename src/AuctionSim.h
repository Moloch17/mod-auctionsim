#pragma once
#include <memory>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
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
    void ScanAuctions(AuctionHouseId _id, bool _DataFactionID);
    void DeleteAuctions(Player* player);
    bool isEnabled;
    
private:
    static AuctionSim* _instance;
    std::unique_ptr<Bot> bot;
    std::unique_ptr<ASConfig> config;
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
