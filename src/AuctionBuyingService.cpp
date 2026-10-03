#include "AuctionBuyingService.h"
#include <algorithm>
#include "AuctionHouseSearcher.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Log.h"
#include "Mail.h"

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

void AuctionBuyingService::ConsiderForBid(AuctionEntry* auction, uint32 marketPrice)
{
    if (_queuedAuctionIds.count(auction->Id) > 0)
    {
        return;  // already queued (as a buyout or a bid) -- one terminal action per auction
    }

    // The game's minimum next bid: current bid + ~5% (floored at 1c). "Only
    // slightly overbid" is exactly this.
    uint32 nextBid = auction->bid + auction->GetAuctionOutBid();
    uint32 nextBidPerUnit = nextBid / std::max<uint32>(1, auction->itemCount);

    // Hard gate + per-scan eagerness roll (see ShouldBidAtPrice). Both keep every
    // outbid the bot places below the item's market value.
    if (!AuctionPricing::ShouldBidAtPrice(nextBidPerUnit, marketPrice))
    {
        return;
    }

    time_t now = GameTime::GetGameTime().count();
    time_t buyTime = AuctionPricing::RollBuyTime(auction->expire_time, now);
    _queue.push_back(
        {auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Bid, marketPrice});
    _queuedAuctionIds.insert(auction->Id);
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

void AuctionBuyingService::EnqueueBidForTest(AuctionEntry* auction, time_t buyTime, uint32 marketCeilingPerUnit)
{
    _queue.push_back(
        {auction->Id, auction->GetHouseId(), buyTime, QueuedPurchase::Action::Bid, marketCeilingPerUnit});
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

    auction->bidder = _bot.GetPlayerRef().GetGUID();
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
    if (!auction->bidder || auction->bidder == botGuid || auction->bid == 0)
    {
        return;  // nothing to outbid, or the bot is already the high bidder
    }

    uint32 nextBid = auction->bid + auction->GetAuctionOutBid();
    uint32 nextBidPerUnit = nextBid / std::max<uint32>(1, auction->itemCount);
    if (nextBidPerUnit >= entry.marketCeilingPerUnit)
    {
        return;  // a rival pushed the price past our walk-away point
    }

    auto trans = CharacterDatabase.BeginTransaction();

    // Must run BEFORE the mutation below: it refunds the current auction->bid to
    // the current auction->bidder.
    sAuctionMgr->SendAuctionOutbiddedMail(auction, nextBid, &_bot.GetPlayerRef(), trans);

    auction->bidder = botGuid;
    auction->bid = nextBid;
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
