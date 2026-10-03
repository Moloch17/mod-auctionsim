#include "AuctionBuyingService.h"
#include <algorithm>
#include <iterator>
#include "AuctionHouseSearcher.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Mail.h"

namespace
{
    // The least core accepts as the next bid: the starting bid when nobody has bid
    // yet, otherwise the current bid plus the ~5% minimum increment.
    uint32 MinimumNextBid(AuctionEntry const* auction)
    {
        return auction->bid ? auction->bid + auction->GetAuctionOutBid() : std::max<uint32>(1, auction->startbid);
    }

    uint32 PerUnit(AuctionEntry const* auction, uint32 amount)
    {
        return amount / std::max<uint32>(1, auction->itemCount);
    }

    // The bot never bids against itself, and never opens bidding on its own listing
    // (that would be bidding with nobody else in the room).
    bool IsBiddable(AuctionEntry const* auction, ObjectGuid botGuid)
    {
        return auction->bidder != botGuid && (auction->bid > 0 || auction->owner != botGuid);
    }

    // Caps every bid the bot places, checked when it is queued and again when it
    // executes. Below the auction's ceiling (private valuation, capped by the
    // cheapest live buyout); strictly below its buyout, which core would treat as a
    // buyout and which would leave players nothing to bid; and within the vendor cap
    // the buyout path also applies.
    bool IsWithinBidCaps(AuctionEntry const* auction, uint32 amount, uint32 ceilingPerUnit, uint32 vendorBuyPrice)
    {
        uint32 perUnit = PerUnit(auction, amount);
        if (perUnit >= ceilingPerUnit)
        {
            return false;
        }
        if (auction->buyout > 0 && amount >= auction->buyout)
        {
            return false;
        }
        return AuctionPricing::IsWithinVendorBuyPrice(perUnit, vendorBuyPrice);
    }
}

AuctionBuyingService::AuctionBuyingService(Bot& bot) : _bot(bot) {}

void AuctionBuyingService::RollTolerance() { _tolerance = AuctionPricing::RollBuyTolerance(); }

void AuctionBuyingService::ConsiderForPurchase(
    AuctionEntry* auction, uint32 pricePerItem, uint32 marketPrice, uint32 ceilingPrice)
{
    if (_queuedAuctionIds.count(auction->Id) > 0)
    {
        return;
    }

    time_t now = GameTime::GetGameTime().count();
    uint32 remainingScans = AuctionPricing::CalculateRemainingScans(auction->expire_time - now);

    if (!AuctionPricing::ShouldBuyAtPrice(pricePerItem, marketPrice, ceilingPrice, _tolerance, remainingScans))
    {
        return;
    }

    time_t buyTime = AuctionPricing::RollBuyTime(auction->expire_time, now);
    _queue.push_back(
        {auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Buyout, 0});
    _queuedAuctionIds.insert(auction->Id);
}

void AuctionBuyingService::ConsiderForBid(AuctionEntry* auction, BidLimits const& limits)
{
    if (_queuedAuctionIds.count(auction->Id) > 0)
    {
        return;  // already queued (as a buyout or a bid) -- one terminal action per auction
    }

    time_t now = GameTime::GetGameTime().count();
    if (AuctionPricing::IsTooLateToBid(auction->expire_time, now) ||
        !IsBiddable(auction, _bot.GetPlayerRef().GetGUID()))
    {
        return;
    }

    // One valuation per auction, rolled the first time the bot considers it and
    // kept for the auction's life, so a bid war ends where this bidder's limit is.
    auto valuation = _bidValuations.find(auction->Id);
    if (valuation == _bidValuations.end())
    {
        valuation = _bidValuations
                        .emplace(auction->Id,
                            AuctionPricing::RollBidValuation(limits.valuationLowPerUnit, limits.marketPerUnit))
                        .first;
    }

    // No sense bidding more than the item costs to buy outright elsewhere on the AH.
    uint32 ceilingPerUnit = valuation->second;
    if (limits.cheapestBuyoutPerUnit > 0)
    {
        ceilingPerUnit = std::min(ceilingPerUnit, limits.cheapestBuyoutPerUnit);
    }

    uint32 minimumBid = MinimumNextBid(auction);
    if (!IsWithinBidCaps(auction, minimumBid, ceilingPerUnit, limits.vendorBuyPrice))
    {
        return;
    }

    // Per-scan eagerness roll (see ShouldBidAtPrice); opening an unbid auction is
    // rarer and needs a clear deal.
    if (!AuctionPricing::ShouldBidAtPrice(PerUnit(auction, minimumBid), ceilingPerUnit, auction->bid == 0))
    {
        return;
    }

    time_t buyTime = AuctionPricing::RollBidTime(auction->expire_time, now);
    _queue.push_back({auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Bid, ceilingPerUnit,
        limits.vendorBuyPrice});
    _queuedAuctionIds.insert(auction->Id);
}

void AuctionBuyingService::PruneBidValuations()
{
    // Valuations die with their auction. Checked against every house, so a scan of
    // one house never drops the other's.
    for (auto it = _bidValuations.begin(); it != _bidValuations.end();)
    {
        bool live = false;
        for (AuctionHouseId houseId : {AuctionHouseId::Alliance, AuctionHouseId::Horde, AuctionHouseId::Neutral})
        {
            AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(houseId);
            if (house && house->GetAuction(it->first))
            {
                live = true;
                break;
            }
        }
        it = live ? std::next(it) : _bidValuations.erase(it);
    }
}

void AuctionBuyingService::SortQueue()
{
    // Descending by buyTime so the soonest-due action sits at the back.
    std::sort(_queue.begin(), _queue.end(), [](QueuedPurchase const& a, QueuedPurchase const& b) {
        return a.buyTime > b.buyTime;
    });
}

void AuctionBuyingService::ProcessDueQueue()
{
    if (_queue.empty())
    {
        return;
    }

    QueuedPurchase next = _queue.back();
    if (GameTime::GetGameTime().count() < next.buyTime)
    {
        return;
    }

    _queue.pop_back();
    _queuedAuctionIds.erase(next.auctionId);
    Execute(next);
}

size_t AuctionBuyingService::DrainQueue()
{
    size_t ran = 0;
    for (QueuedPurchase const& entry : _queue)
    {
        Execute(entry);
        ++ran;
    }
    _queue.clear();
    _queuedAuctionIds.clear();
    return ran;
}

void AuctionBuyingService::EnqueueForTest(AuctionEntry* auction, time_t buyTime)
{
    _queue.push_back(
        {auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Buyout, 0});
    _queuedAuctionIds.insert(auction->Id);
}

void AuctionBuyingService::EnqueueBidForTest(
    AuctionEntry* auction, time_t buyTime, uint32 bidCeilingPerUnit, uint32 vendorBuyPrice)
{
    _queue.push_back(
        {auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Bid, bidCeilingPerUnit,
         vendorBuyPrice});
    _queuedAuctionIds.insert(auction->Id);
}

void AuctionBuyingService::Execute(QueuedPurchase const& entry)
{
    if (entry.action == QueuedPurchase::Action::Bid)
    {
        PlaceBid(entry);
    }
    else
    {
        BuyItem(entry);
    }
}

void AuctionBuyingService::BuyItem(QueuedPurchase const& entry)
{
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(entry.houseId);
    if (!house)
    {
        return;
    }

    // Re-fetch by id: a player may have bought or the server expired this auction
    // during the queue delay.
    AuctionEntry* auction = house->GetAuction(entry.auctionId);
    if (!auction)
    {
        return;
    }

    auto trans = CharacterDatabase.BeginTransaction();

    // A player holding the high bid gets it back, as core's own buyout path does.
    // Must run before the mutation below: it refunds the current auction->bid.
    Player* botPlayer = &_bot.GetPlayerRef();
    if (auction->bidder && auction->bidder != botPlayer->GetGUID())
    {
        sAuctionMgr->SendAuctionOutbiddedMail(auction, auction->buyout, botPlayer, trans);
    }

    auction->bidder = botPlayer->GetGUID();
    auction->bid = auction->buyout;
    sAuctionMgr->SendAuctionSuccessfulMail(auction, trans);
    auction->DeleteFromDB(trans);
    sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);  // destroy the bought item, don't leak it
    house->RemoveAuction(auction);

    CharacterDatabase.CommitTransaction(trans);
}

void AuctionBuyingService::PlaceBid(QueuedPurchase const& entry)
{
    AuctionHouseObject* house = sAuctionMgr->GetAuctionsMapByHouseId(entry.houseId);
    if (!house)
    {
        return;
    }

    AuctionEntry* auction = house->GetAuction(entry.auctionId);
    if (!auction)
    {
        return;  // bought out / expired / cancelled during the delay
    }

    ObjectGuid const botGuid = _bot.GetPlayerRef().GetGUID();
    if (!IsBiddable(auction, botGuid))
    {
        return;  // the bot already holds the high bid
    }
    if (AuctionPricing::IsTooLateToBid(auction->expire_time, GameTime::GetGameTime().count()))
    {
        return;  // no sniping: the auction slipped into its last 30 minutes during the delay
    }

    // A rival may have bid during the delay: re-check against the current minimum.
    uint32 minimumBid = MinimumNextBid(auction);
    if (!IsWithinBidCaps(auction, minimumBid, entry.bidCeilingPerUnit, entry.vendorBuyPrice))
    {
        return;  // pushed past this bidder's limit, up to the buyout, or past the vendor price
    }

    // The amount a player would type; the bare minimum if the rounded one breaks a cap.
    uint32 amount = AuctionPricing::RollBidAmount(minimumBid);
    if (!IsWithinBidCaps(auction, amount, entry.bidCeilingPerUnit, entry.vendorBuyPrice))
    {
        amount = minimumBid;
    }

    auto trans = CharacterDatabase.BeginTransaction();

    // Must run BEFORE the mutation below: it refunds the current auction->bid to
    // the current auction->bidder. An opening bid has nobody to refund.
    if (auction->bidder)
    {
        sAuctionMgr->SendAuctionOutbiddedMail(auction, amount, &_bot.GetPlayerRef(), trans);
    }

    auction->bidder = botGuid;
    auction->bid = amount;
    sAuctionMgr->GetAuctionHouseSearcher()->UpdateBid(auction);

    CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_UPD_AUCTION_BID);
    stmt->SetData(0, auction->bidder.GetCounter());
    stmt->SetData(1, auction->bid);
    stmt->SetData(2, auction->Id);
    trans->Append(stmt);

    CharacterDatabase.CommitTransaction(trans);
    // Do NOT DeleteFromDB / RemoveAItem / RemoveAuction -- core settles the auction
    // normally at expiry (seller paid, item mailed to the winner).
}
