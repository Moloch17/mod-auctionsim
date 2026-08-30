#include "AuctionSim.h"
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
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

namespace
{
    bool IsBotCharacter(uint32 lowGuid)
    {
        uint32 botLowGuid = sConfigMgr->GetOption<uint32>("AuctionSim.BotCharacterID", 0);
        return botLowGuid != 0 && lowGuid == botLowGuid;
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

void AuctionSim::OnStartup()
{
    if (EnsureConfigFileExists())
    {
        // File didn't exist when ConfigMgr loaded module configs; pull it in now so
        // ASConfig and Bot below read real values instead of defaults.
        sConfigMgr->Reload();
    }

    // Load auctionsim.dat unconditionally: the addon shows/edits the listing table
    // whether or not the module is enabled.
    {
        bool datOk = true;
        config = std::make_unique<ASConfig>(sConfigMgr->GetConfigPath() + "/modules/auctionsim.dat", datOk);
        if (!datOk)
        {
            LOG_ERROR("module", "AuctionSim: auctionsim.dat failed to load");
            config.reset();
        }
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

    if (ServerConfigs::CONFIG_ALLOW_TWO_SIDE_INTERACTION_AUCTION == 1)
    {
        LOG_ERROR("module", "AuctionSim: Two sided auction interaction is not allowed");
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
        ScanAuctions(AuctionHouseId::Alliance);
        ScanAuctions(AuctionHouseId::Horde);
        LOG_INFO("module", "AuctionSim: Startup complete");
    }
}

bool AuctionSim::StartOrReloadBot(bool reloadConfig)
{
    if (!config)
    {
        return false;
    }

    // "Set Bot Char" just rewrote BotAccountID/BotCharacterID; reload so Bot's ctor
    // and the mail hook see them. A failed reload leaves any running bot alone.
    if (reloadConfig && !sConfigMgr->Reload())
    {
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
    listingService = std::make_unique<AuctionListingService>(*bot, *config);
    buyingService = std::make_unique<AuctionBuyingService>(*bot);

    LOG_INFO("module", "AuctionSim: bot active (character {})", bot->GetCharacterID());
    return true;
}

void AuctionSim::OnUpdate(uint32 diff)
{
    // isEnabled can be set before a bot exists (enabled via the addon), so check both
    if (!this->isEnabled || !buyingService) return;

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
            sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
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
            sAuctionMgr->RemoveAItem(it->second->item_guid, true, &trans);
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
            sAuctionMgr->RemoveAItem(it->second->item_guid, true, &trans);
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
    MailDraft* /*mailDraft*/,
    MailReceiver const& receiver,
    MailSender const& sender,
    MailCheckMask& /*checked*/,
    uint32& /*deliver_delay*/,
    uint32& /*custom_expiration*/,
    bool& deleteMailItemsFromDB,
    bool& sendMail)
{
    if (IsBotCharacter(receiver.GetPlayerGUIDLow()))
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
