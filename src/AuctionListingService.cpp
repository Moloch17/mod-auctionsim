#include "AuctionListingService.h"
#include <algorithm>
#include <ctime>
#include <vector>
#include "ASConfig.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "Item.h"
#include "Log.h"
#include "ObjectMgr.h"
#include "Random.h"
#include "ScannedItem.h"

namespace
{
    // Fill-loop iteration budget: enough attempts to place `toAdd` items even when
    // weighted picks keep landing on capped/unlistable entries before they're zeroed.
    constexpr int kFillAttemptsPerItem = 4;
    constexpr int kFillAttemptsBase = 16;

    // Scale a market figure by a config multiplier, rounding to nearest.
    int ScaleAndRound(uint32 value, float multiplier)
    {
        return static_cast<int>(static_cast<float>(value) * multiplier + 0.5f);
    }
}

AuctionListingService::AuctionListingService(Bot& bot, ASConfig const& config) : _bot(bot), _config(config) {}

void AuctionListingService::ListNewAuctions(
    AuctionHouseId houseId,
    int const (&existingCounts)[MAX_ITEM_CLASS][MAX_ITEM_QUALITY],
    std::unordered_map<uint32, int> const& itemAuctionCount)
{
    auto trans = CharacterDatabase.BeginTransaction();

    // Auctions this pass has created, so the per-item cap counts them alongside
    // what was already on the house.
    std::unordered_map<uint32, int> listedThisScan;

    for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; itemClass++)
    {
        for (uint32 quality = 0; quality < MAX_ITEM_QUALITY; quality++)
        {
            float multiplier = _config.ItemSelectionMask[itemClass][quality];
            if (multiplier <= 0.0f)
            {
                continue;
            }

            ASConfig::CategoryDepth const& depth = _config.GetCategoryDepth(houseId, itemClass, quality);
            if (!depth.has)
            {
                continue;  // never observed this category in the scans -- leave it alone
            }

            std::vector<ScannedItem*> const& pool = _config.ItemsFor(houseId, itemClass, quality);
            if (pool.empty())
            {
                continue;
            }

            int current = existingCounts[itemClass][quality];

            // Gate: only top a category up once it has dropped below its observed
            // lower quartile. We wait for a real dip rather than nudging toward the
            // mean every pass.
            int gate = ScaleAndRound(depth.q1, multiplier);
            if (current >= gate)
            {
                continue;
            }

            int target = ScaleAndRound(AuctionPricing::RollCategoryTarget(depth.q1, depth.median), multiplier);
            int toAdd = AuctionPricing::CalculateItemsToList(target, current);
            if (toAdd <= 0)
            {
                continue;
            }

            // Weight each item by how many concurrent auctions the real market
            // carries of it; items never seen in a snapshot still get weight 1 so
            // the category can fill. That same figure is the per-item cap. A weight
            // is zeroed once its item is capped, unlistable, or can't be created,
            // and weightTotal is kept in step so WeightedPick never re-sums the pool.
            std::vector<uint32>& weights = _weightBuffer;
            weights.resize(pool.size());
            uint32 weightTotal = 0;
            for (size_t i = 0; i < pool.size(); ++i)
            {
                weights[i] = std::max(1u, pool[i]->GetTypicalListingCount());
                weightTotal += weights[i];
            }

            auto dropWeight = [&weights, &weightTotal](size_t at) {
                weightTotal -= weights[at];
                weights[at] = 0;
            };

            int guard = toAdd * kFillAttemptsPerItem + kFillAttemptsBase;
            while (toAdd > 0 && guard-- > 0)
            {
                size_t idx = AuctionPricing::WeightedPick(weights, weightTotal);
                if (idx >= pool.size())
                {
                    break;  // every remaining item is zeroed out
                }

                ScannedItem const& scan = *pool[idx];
                uint32 itemId = scan.GetItemID();

                int existingForItem = 0;
                if (auto it = itemAuctionCount.find(itemId); it != itemAuctionCount.end())
                {
                    existingForItem = it->second;
                }
                // weights[idx] is still the item's untouched weight -- WeightedPick
                // only returns non-zero indices and weights are only ever zeroed --
                // which is exactly max(1, GetTypicalListingCount()), the per-item cap.
                int itemCap = static_cast<int>(weights[idx]);
                if (existingForItem + listedThisScan[itemId] >= itemCap)
                {
                    dropWeight(idx);
                    continue;
                }

                if (!AuctionPricing::IsListablePrice(scan.GetMarketPrice()))
                {
                    dropWeight(idx);
                    continue;
                }

                if (ListOneItem(scan, houseId, trans))
                {
                    listedThisScan[itemId]++;
                    toAdd--;
                }
                else
                {
                    dropWeight(idx);  // level cap / missing template -- don't retry it
                }
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

    uint32 quantity = AuctionPricing::RollStackSize(
        scan.GetTypicalStackSize(), scan.GetStackLow(), scan.GetStackHigh(), proto->GetMaxStackSize());
    uint32 buyout = AuctionPricing::RollBuyoutPrice(
        scan.GetListLow(), scan.GetMarketPrice(), scan.GetListHigh(), quantity, scan.GetSampleCount());

    // The item only ever lives in the auction house, never in the bot's inventory or
    // item-update queue -- otherwise Player::_SaveInventory trips over it (bag 255 /
    // slot 0) once the bot character is also logged in. Create it ownerless, stamp the
    // owner guid, and let SaveToDB persist it in `trans` (committed async, like the
    // core's own HandleAuctionSellItem).
    //
    // randomPropertyId is declared uint32 but is passed straight into
    // SetItemRandomProperties(int32); negative (enchantment/suffix table) values
    // round-trip correctly through the bit-preserving uint32<->int32 conversion.
    Item* item =
        Item::CreateItem(scan.GetItemID(), quantity, nullptr, false, static_cast<uint32>(scan.GetSuffixID()));
    item->SetOwnerGUID(_bot.GetPlayerRef().GetGUID());

    AuctionEntry* auction = new AuctionEntry();
    auction->Id = sObjectMgr->GenerateAuctionID();
    auction->houseId = houseId;
    auction->item_guid = item->GetGUID();
    auction->item_template = item->GetEntry();
    auction->itemCount = quantity;
    auction->owner = _bot.GetPlayerRef().GetGUID();
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
