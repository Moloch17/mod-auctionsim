#include "AuctionListingService.h"
#include <ctime>
#include "ASConfig.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Random.h"
#include "ScannedItem.h"

AuctionListingService::AuctionListingService(Bot& bot, ASConfig const& config) : _bot(bot), _config(config) {}

void AuctionListingService::ListNewAuctions(
    AuctionHouseId houseId, int const (&existingCounts)[MAX_ITEM_CLASS][MAX_ITEM_QUALITY])
{
    auto trans = CharacterDatabase.BeginTransaction();

    for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; itemClass++)
    {
        for (uint32 quality = 0; quality < MAX_ITEM_QUALITY; quality++)
        {
            float mask = _config.ItemSelectionMask[itemClass][quality];
            if (mask == 0.0f)
            {
                continue;
            }

            std::vector<ScannedItem*> const& pool = _config.ItemsFor(houseId, itemClass, quality);
            if (pool.empty())
            {
                continue;
            }

            int targetCount = AuctionPricing::CalculateTargetListingCount(mask, pool.size());
            int itemsToPick = AuctionPricing::CalculateItemsToList(targetCount, existingCounts[itemClass][quality]);

            while (itemsToPick > 0)
            {
                ScannedItem const& scan = *pool[urand(0, static_cast<uint32>(pool.size()) - 1)];

                if (!AuctionPricing::IsListablePrice(scan.GetMeanPrice()))
                {
                    itemsToPick--;
                    continue;
                }

                ListOneItem(scan, houseId, trans);
                itemsToPick--;
            }
        }
    }

    CharacterDatabase.CommitTransaction(trans);
}

AuctionEntry* AuctionListingService::ListTestItem(ScannedItem const& scan, AuctionHouseId houseId)
{
    auto trans = CharacterDatabase.BeginTransaction();
    AuctionEntry* auction = ListOneItem(scan, houseId, trans);
    CharacterDatabase.CommitTransaction(trans);
    return auction;
}

AuctionEntry* AuctionListingService::ListOneItem(
    ScannedItem const& scan, AuctionHouseId houseId, SQLTransaction<CharacterDatabaseConnection>& trans)
{
    ItemTemplate const* proto = sObjectMgr->GetItemTemplate(scan.GetItemID());
    if (!proto)
    {
        LOG_WARN("module", "AuctionSim: item {} not found in item_template, skipping listing", scan.GetItemID());
        return nullptr;
    }

    if (!AuctionPricing::IsWithinLevelCap(
            proto->RequiredLevel, proto->ItemLevel, _config.maxRequiredLevel, _config.maxItemLevel))
    {
        return nullptr;
    }

    uint32 quantity = AuctionPricing::RollQuantity(proto->GetMaxStackSize());
    uint32 buyout =
        AuctionPricing::RollBuyoutPrice(scan.GetMinPrice(), scan.GetMeanPrice(), scan.GetMaxPrice(), quantity);

    // Item::CreateItem's randomPropertyId is declared uint32 but is passed straight into
    // SetItemRandomProperties(int32) -- negative (enchantment/suffix table) values round-trip
    // correctly through the bit-preserving uint32<->int32 conversion.
    //
    // The item is never placed in an inventory slot, so it must not go through the bot
    // player's item update queue: CreateItem already stamps the owner GUID, and
    // SaveToDB below persists it in `trans`. Queueing it instead leaves a stray entry
    // that Player::_SaveInventory later trips over (bag 255 / slot 0) when the bot
    // character is also logged in.
    Item* item = Item::CreateItem(
        scan.GetItemID(), quantity, _bot.GetPlayer().get(), false, static_cast<uint32>(scan.GetSuffixID()));

    AuctionEntry* auction = new AuctionEntry();
    auction->Id = sObjectMgr->GenerateAuctionID();
    auction->houseId = houseId;
    auction->item_guid = item->GetGUID();
    auction->item_template = item->GetEntry();
    auction->itemCount = quantity;
    auction->owner = _bot.GetPlayer().get()->GetGUID();
    auction->startbid = buyout;
    auction->buyout = buyout;
    auction->bid = 0;
    auction->deposit = 0;
    auction->expire_time = std::time(nullptr) + AuctionPricing::RollAuctionDuration();
    auction->auctionHouseEntry = sAuctionMgr->GetAuctionHouseEntryFromHouse(houseId);

    item->SaveToDB(trans);
    sAuctionMgr->AddAItem(item);
    sAuctionMgr->GetAuctionsMapByHouseId(houseId)->AddAuction(auction);
    auction->SaveToDB(trans);

    return auction;
}
