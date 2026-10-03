#pragma once
#include <cstddef>
#include <ctime>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "AuctionHouseMgr.h"
#include "AuctionPricing.h"
#include "DatabaseEnvFwd.h"

class Bot;

// Owns the "buy" side of the bot: candidates found during a scan are queued,
// then executed a few at a time as their rolled buy time comes due. A queued
// entry is either a full buyout or a bid -- an outbid on an auction a real player
// is bidding on, or an opening bid on a player's unbid auction. Both share one
// queue and one dedupe set.
class AuctionBuyingService
{
public:
    struct QueuedPurchase
    {
        // No AuctionEntry* is kept: a player could buy it out, or it could expire or be
        // cancelled, during the delay. Both paths re-fetch by auctionId + houseId.
        uint32 auctionId = 0;
        AuctionHouseId houseId{};
        time_t buyTime = 0;
        enum class Action
        {
            Buyout,
            Bid
        } action = Action::Buyout;
        uint32 bidCeilingPerUnit = 0;  // bid path: walk away once the next bid reaches this
        uint32 vendorBuyPrice = 0;     // bid path: per-unit vendor cap, 0 = none (see IsWithinVendorBuyPrice)
    };

    // What one scan knows about an auction's item, for a bid decision. All per unit.
    struct BidLimits
    {
        uint32 valuationLowPerUnit = 0;    // valuation band: lower quartile ...
        uint32 marketPerUnit = 0;          // ... up to the market price (see RollBidValuation)
        uint32 vendorBuyPrice = 0;         // 0 = no vendor cap
        uint32 cheapestBuyoutPerUnit = 0;  // cheapest live buyout of the item on this house, 0 = none
    };

    explicit AuctionBuyingService(Bot& bot);

    // Rolls a fresh buy-tolerance profile for the upcoming scan pass. Call once
    // per ScanAuctions() invocation, before any ConsiderForPurchase() calls.
    void RollTolerance();

    // Called once per non-bot-owned auction found during a scan pass; queues it
    // for purchase per this scan's tolerance profile and the auction's remaining lifetime.
    // A no-op if this auction is already queued (e.g. a re-scan before the first
    // queued purchase fired), so an auction can never end up in the queue twice.
    // marketPrice / ceilingPrice are the item's robust typical price and 75th-
    // percentile ceiling (see ScannedItem), not the raw scan mean/max.
    void ConsiderForPurchase(
        AuctionEntry* auction, uint32 pricePerItem, uint32 marketPrice, uint32 ceilingPrice);

    // Called for an auction a real player holds the high bid on, or a player's
    // auction nobody has bid on yet. Queues one bid, executed later at the game's
    // minimum next bid (sometimes rounded up the way a player types it). The first
    // call for an auction rolls its private valuation (AuctionPricing::
    // RollBidValuation), kept until the auction is gone; every bid stays below that,
    // below the cheapest live buyout of the item, strictly below the auction's own
    // buyout (core treats reaching it as a buyout), and within the vendor cap. No
    // bid in the auction's last 30 minutes (no sniping). Then a per-scan roll
    // (AuctionPricing::ShouldBidAtPrice). No-op if the auction is already queued
    // (shares _queuedAuctionIds with the buyout path).
    void ConsiderForBid(AuctionEntry* auction, BidLimits const& limits);

    // Drops the valuations of auctions no longer on any house. Call once per scan.
    void PruneBidValuations();

    // Sorts the queue so the soonest-due purchase is processed first. Call once
    // after a scan pass has finished calling ConsiderForPurchase.
    void SortQueue();

    // Executes at most one due action from the queue. Safe to call every tick.
    void ProcessDueQueue();

    // Executes every queued action right now, regardless of its rolled buy time,
    // and clears the queue. Returns how many were run. For the ".auctionsim
    // runqueue" command / the addon's "Run Queue" button.
    size_t DrainQueue();

    // Drops every queued action without running it; returns how many. For a market
    // purge, which must leave nothing behind that could buy afterwards.
    size_t ClearQueue();

    size_t QueueSize() const { return _queue.size(); }

    // True while this auction waits in the queue (as a buyout or a bid).
    bool IsQueued(uint32 auctionId) const { return _queuedAuctionIds.count(auctionId) > 0; }

    // Market mode's buyers: queues a buyout of an auction they chose, executed at
    // buyTime by the same BuyItem path (re-fetch, bidder refund). False, and nothing
    // queued, if the auction is already queued. Call SortQueue() after a batch.
    bool EnqueueBuyout(uint32 auctionId, AuctionHouseId houseId, time_t buyTime);

    // Read-only view of the current queue, soonest-due last (matches SortQueue's order).
    // For reporting only (e.g. ".auctionsim showqueue") -- entries are AuctionEntry*, only
    // valid until the next ProcessDueQueue()/scan pass on this same world tick.
    std::vector<QueuedPurchase> const& GetQueue() const { return _queue; }

    // Test-support: forces an auction directly into the queue with an explicit buyTime,
    // bypassing ConsiderForPurchase's price/RNG logic, for deterministic tests.
    void EnqueueForTest(AuctionEntry* auction, time_t buyTime);
    void EnqueueBidForTest(
        AuctionEntry* auction, time_t buyTime, uint32 bidCeilingPerUnit, uint32 vendorBuyPrice = 0);

private:
    void Execute(QueuedPurchase const& entry);
    void BuyItem(QueuedPurchase const& entry);
    void PlaceBid(QueuedPurchase const& entry);

    Bot& _bot;
    AuctionPricing::BuyTolerance _tolerance{};
    std::vector<QueuedPurchase> _queue;
    std::unordered_set<uint32> _queuedAuctionIds;
    std::unordered_map<uint32, uint32> _bidValuations;  // auction id -> per-unit valuation
};
