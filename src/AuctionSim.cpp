#include "AuctionSim.h"
#include <memory>
#include <string>
#include "ASConfig.h"
#include "AuctionHouseMgr.h"
#include "Bot.h"
#include "Define.h"
#include "Log.h"
#include "Mail.h"

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

    config = std::make_unique<ASConfig>("env/dist/etc/modules/auctionsim.dat", isEnabled);

    if (config->updateInterval == 0)
    {
        LOG_WARN("module", "AuctionSim: UpdateInterval cannot be 0");
        isEnabled = false;
        return;
    }
}

void AuctionSim::OnUpdate(uint32 diff)
{
    if (!this->isEnabled) return;

    static uint32 scanTimer = 0;
    scanTimer += diff;

    if (scanTimer >= config->updateInterval * 60000)
    {
        ScanAuctions(AuctionHouseId::Alliance, 0);
        ScanAuctions(AuctionHouseId::Horde, 1);

        scanTimer = 0;
    }
}

void AuctionSim::ScanAuctions(AuctionHouseId _id, bool _DataFactionID)
{
    auto map = sAuctionMgr->GetAuctionsMapByHouseId(_id)->GetAuctions();
    int auctionTable[MAX_ITEM_CLASS][MAX_ITEM_QUALITY] = {};
    for (auto it = map.begin(); it != map.end(); ++it)
    {
        const ItemTemplate* proto = sObjectMgr->GetItemTemplate(it->second->item_template);
        auctionTable[proto->Class][proto->Quality]++;
        if (it->second->owner != bot->GetPlayer().get()->GetGUID())
        {
            // check if price is within buy range
            int pricePerItem = it->second->buyout / it->second->itemCount;

            // Check if we have any data on the item
            ScannedItem* scannedItem = nullptr;
            for (ScannedItem*& item : config->ItemSelectionTable[_DataFactionID][proto->Class][proto->Quality])
            {
                if (item->GetItemID() == it->second->item_template)
                {
                    scannedItem = item;
                }
            }

            if (!scannedItem) continue;

            // Buy item if price is under mean price
            if (pricePerItem <= scannedItem->GetMeanPrice())
            {
                AuctionEntry* auction = it->second;
                auto trans = CharacterDatabase.BeginTransaction();

                // Remove auction from the auction house
                sAuctionMgr->GetAuctionsMapByHouseId(_id)->RemoveAuction(auction);

                // Remove the item from auction manager
                sAuctionMgr->RemoveAItem(auction->item_guid, false, &trans);

                // Set the bot as the bidder/winner
                auction->bidder = bot->GetPlayer().get()->GetGUID();
                auction->bid = auction->buyout;

                // Send auction successful mail to seller (with the gold)
                sAuctionMgr->SendAuctionSuccessfulMail(auction, trans, true, true, true);

                // Delete the auction from the database
                auction->DeleteFromDB(trans);

                // Remove from auction house (this will also delete the auction pointer)
                sAuctionMgr->GetAuctionsMapByHouseId(_id)->RemoveAuction(auction);

                CharacterDatabase.CommitTransaction(trans);
            }
        }
    }
    for (int i = 0; i < MAX_ITEM_CLASS; i++)
    {
        for (int j = 0; j < MAX_ITEM_QUALITY; j++)
        {
            if (config->ItemSelectionMask[i][j] == 0) continue;
            if (config->ItemSelectionTable[_DataFactionID][i][j].size() == 0) continue;

            int itemsWanted = config->ItemSelectionMask[i][j] * config->ItemSelectionTable[_DataFactionID][i][j].size();
            int itemsToPick = itemsWanted - auctionTable[i][j];
            while (itemsToPick > 0)
            {
                int randIndex = std::rand() % config->ItemSelectionTable[_DataFactionID][i][j].size();
                ScannedItem* scan = config->ItemSelectionTable[_DataFactionID][i][j][randIndex];
                int maxQty = sObjectMgr->GetItemTemplate(scan->GetItemID())->GetMaxStackSize();
                int x = std::rand() % 100;

                int qty;
                if (x < 33)
                {
                    qty = 1;
                }
                else if (x > 66)
                {
                    qty = maxQty;
                }
                else
                {
                    qty = 2 + (std::rand() % (maxQty - 2));
                }

                if (scan->GetMeanPrice() < 3)
                {
                    itemsToPick--;
                    continue;
                }

                int buyout = qty * ((std::rand() % (int)(scan->GetMeanPrice() * 0.3f)) + scan->GetMeanPrice() * 0.8f);

                // Create the item
                Item* item =
                    Item::CreateItem(scan->GetItemID(), qty, bot->GetPlayer().get(), false, scan->GetSuffixID());
                item->AddToUpdateQueueOf(bot->GetPlayer().get());

                // Create and populate the auction entry
                AuctionEntry* auction = new AuctionEntry();
                auction->Id = sObjectMgr->GenerateAuctionID();
                auction->houseId = _id;
                auction->item_guid = item->GetGUID();
                auction->item_template = item->GetEntry();
                auction->itemCount = qty;
                auction->owner = bot->GetPlayer().get()->GetGUID();
                auction->startbid = 0;
                auction->buyout = buyout;
                auction->bid = 0;
                auction->deposit = 0;
                int time = (std::rand() % 39601) + 3600;  // Random duration between 1 and 12 hours
                auction->expire_time = std::time(nullptr) + time;
                auction->auctionHouseEntry = sAuctionMgr->GetAuctionHouseEntryFromHouse(_id);

                // Save to database
                auto trans = CharacterDatabase.BeginTransaction();
                item->SaveToDB(trans);
                item->RemoveFromUpdateQueueOf(bot->GetPlayer().get());
                sAuctionMgr->AddAItem(item);
                sAuctionMgr->GetAuctionsMapByHouseId(_id)->AddAuction(auction);
                auction->SaveToDB(trans);
                CharacterDatabase.CommitTransaction(trans);
                itemsToPick--;
            }
        }
    }
}

void AuctionSim::DeleteAuctions(Player* player)
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
