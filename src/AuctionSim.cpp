#include "AuctionSim.h"
#include <memory>
#include <string>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
#include "AuctionHouseSearcher.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "Config.h"
#include "DatabaseEnvFwd.h"
#include "Define.h"
#include "Log.h"
#include "Mail.h"
#include "ScriptMgr.h"
#include "WorldConfig.h"

AuctionSim* AuctionSim::_instance = nullptr;

AuctionSim::AuctionSim() : WorldScript("AuctionSim")
{
    _instance = this;
    isEnabled = sConfigMgr->GetOption<bool>("AuctionSim.Enabled", false);
}

void AuctionSim::OnStartup()
{
    if (!isEnabled)
    {
        LOG_WARN("module", "AuctionSim is disabled!");
        return;
    }

    bot = std::make_unique<Bot>(this->isEnabled);
    if (!isEnabled)
    {
        return;
    }

    std::string path = sConfigMgr->GetConfigPath() + "/modules/auctionsim.dat";

    config = std::make_unique<ASConfig>(path, isEnabled);
    if (!isEnabled)
    {
        return;
    }

    if (ServerConfigs::CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION == 1)
    {
        LOG_ERROR("module", "AuctionSim: Two sided auction interaction is not allowed");
        isEnabled = false;
        return;
    }

    listingService = std::make_unique<AuctionListingService>(*bot, *config);
    buyingService = std::make_unique<AuctionBuyingService>(*bot);

    if (sConfigMgr->GetOption<bool>("AuctionSim.StartupScan", false))
    {
        ScanAuctions(AuctionHouseId::Alliance);
        ScanAuctions(AuctionHouseId::Horde);
        LOG_INFO("module", "AuctionSim: Startup complete");
    }
}

void AuctionSim::OnUpdate(uint32 diff)
{
    if (!this->isEnabled) return;

    scanTimer += diff;

    if (scanTimer >= AuctionPricing::kScanIntervalSeconds * 1000)
    {
        ScanAuctions(AuctionHouseId::Alliance);
        ScanAuctions(AuctionHouseId::Horde);
        scanTimer = 0;
    }

    buyingService->ProcessDueQueue();
}

void AuctionSim::ScanAuctions(AuctionHouseId _AuctionHouseId)
{
    auto map = sAuctionMgr->GetAuctionsMapByHouseId(_AuctionHouseId)->GetAuctions();
    int auctionTable[MAX_ITEM_CLASS][MAX_ITEM_QUALITY] = {};

    buyingService->RollTolerance();

    for (auto it = map.begin(); it != map.end(); ++it)
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

        if (auction->owner == bot->GetPlayer().get()->GetGUID())
        {
            continue;
        }

        ScannedItem const* scannedItem =
            config->FindScannedItem(_AuctionHouseId, proto->Class, proto->Quality, auction->item_template);
        if (!scannedItem)
        {
            continue;
        }

        uint32 pricePerItem = auction->buyout / auction->itemCount;
        buyingService->ConsiderForPurchase(
            auction, pricePerItem, scannedItem->GetMeanPrice(), scannedItem->GetMaxPrice());
    }

    buyingService->SortQueue();

    listingService->ListNewAuctions(_AuctionHouseId, auctionTable);
}

std::vector<AuctionSimTests::TestResult> AuctionSim::RunTests()
{
    std::vector<AuctionSimTests::TestResult> results = AuctionSimTests::RunLogicTests(*bot, *config);

    results.push_back(
        AuctionSimTests::RunLiveListingTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(AuctionSimTests::RunLiveListingTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    results.push_back(
        AuctionSimTests::RunLiveBuyingTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(AuctionSimTests::RunLiveBuyingTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    results.push_back(
        AuctionSimTests::RunLiveLevelCapTest(*bot, *config, *listingService, AuctionHouseId::Alliance));
    results.push_back(
        AuctionSimTests::RunLiveLevelCapTest(*bot, *config, *listingService, AuctionHouseId::Horde));

    return results;
}

uint32 AuctionSim::CleanOverCapAuctions()
{
    if (!bot || !bot->GetPlayer() || !config)
    {
        return 0;
    }

    ObjectGuid botPlayerGUID = bot->GetPlayer().get()->GetGUID();
    auto trans = CharacterDatabase.BeginTransaction();
    uint32 removedCount = 0;

    for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde})
    {
        auto auctions = sAuctionMgr->GetAuctionsMapByHouseId(houseId)->GetAuctions();
        for (auto it = auctions.begin(); it != auctions.end();)
        {
            AuctionEntry* auction = it->second;
            if (auction->owner != botPlayerGUID)
            {
                ++it;
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(auction->item_template);
            bool withinCap = !proto || AuctionPricing::IsWithinLevelCap(
                                            proto->RequiredLevel,
                                            proto->ItemLevel,
                                            config->maxRequiredLevel,
                                            config->maxItemLevel);
            if (withinCap)
            {
                ++it;
                continue;
            }

            auction->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(auction->item_guid);
            sAuctionMgr->GetAuctionsMapByHouseId(houseId)->RemoveAuction(auction);
            it = auctions.erase(it);
            removedCount++;
        }
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

    ObjectGuid botPlayerGUID = bot->GetPlayer().get()->GetGUID();
    auto trans = CharacterDatabase.BeginTransaction();

    auto allianceAuctions = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Alliance)->GetAuctions();
    for (auto it = allianceAuctions.begin(); it != allianceAuctions.end();)
    {
        if (it->second->owner == botPlayerGUID)
        {
            it->second->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(it->second->item_guid);
            sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Alliance)->RemoveAuction(it->second);
            it = allianceAuctions.erase(it);
        }
        else
        {
            ++it;
        }
    }

    auto hordeAuctions = sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Horde)->GetAuctions();
    for (auto it = hordeAuctions.begin(); it != hordeAuctions.end();)
    {
        if (it->second->owner == botPlayerGUID)
        {
            it->second->DeleteFromDB(trans);
            sAuctionMgr->RemoveAItem(it->second->item_guid);
            sAuctionMgr->GetAuctionsMapByHouseId(AuctionHouseId::Horde)->RemoveAuction(it->second);
            it = hordeAuctions.erase(it);
        }
        else
        {
            ++it;
        }
    }

    CharacterDatabase.CommitTransaction(trans);
}

void AuctionSimMailManager::OnBeforeMailDraftSendMailTo(
    MailDraft* mailDraft,
    MailReceiver const& receiver,
    MailSender const& sender,
    MailCheckMask& checked,
    uint32& deliver_delay,
    uint32& custom_expiration,
    bool& deleteMailItemsFromDB,
    bool& sendMail)
{
    if (receiver.GetPlayerGUIDLow() == sConfigMgr->GetOption<int>("AuctionSim.BotCharacterID", 0))
    {
        sendMail = false;
        if (sender.GetMailMessageType() == MAIL_AUCTION)
        {
            deleteMailItemsFromDB = true;
        }
    }
}
void AddAuctionSimScripts()
{
    new AuctionSim();
    new AuctionSimMailManager();
}
