#pragma once
#include <cstddef>
#include <ctime>
#include <unordered_set>
#include <vector>
#include "AuctionHouseMgr.h"
#include "AuctionPricing.h"
#include "DatabaseEnvFwd.h"

class Bot;

// Owns the "buy" side of the bot: candidates found during a scan are queued,
// then executed a few at a time as their rolled buy time comes due. A queued
// entry is either a full buyout or a small outbid on an auction a real player is
// already bidding on -- both share one queue and one dedupe set.
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
        uint32 marketCeilingPerUnit = 0;  // bid path: walk away once a rival pushes past this
        uint32 vendorBuyPrice = 0;        // bid path: per-unit vendor cap, 0 = none (see IsWithinVendorBuyPrice)
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

    // Called for an auction that already carries a real player's high bid. Queues
    // a single small outbid (the game's ~5% minimum increment) if that next bid,
    // per unit, would land below marketPrice -- and then only on a probabilistic
    // roll (see AuctionPricing::ShouldBidAtPrice). Never queues a bid that would
    // reach the auction's buyout (core treats that as a buyout, and players could no
    // longer bid) or pay more per unit than vendorBuyPrice (0 disables, as for the
    // buyout path). No-op if the auction is already queued (shares _queuedAuctionIds
    // with the buyout path).
    void ConsiderForBid(AuctionEntry* auction, uint32 marketPrice, uint32 vendorBuyPrice);

    // Sorts the queue so the soonest-due purchase is processed first. Call once
    // after a scan pass has finished calling ConsiderForPurchase.
    void SortQueue();

    // Executes at most one due action from the queue. Safe to call every tick.
    void ProcessDueQueue();

    // Executes every queued action right now, regardless of its rolled buy time,
    // and clears the queue. Returns how many were run. For the ".auctionsim
    // runqueue" command / the addon's "Run Queue" button.
    size_t DrainQueue();

    size_t QueueSize() const { return _queue.size(); }

    // Read-only view of the current queue, soonest-due last (matches SortQueue's order).
    // For reporting only (e.g. ".auctionsim showqueue") -- entries are AuctionEntry*, only
    // valid until the next ProcessDueQueue()/scan pass on this same world tick.
    std::vector<QueuedPurchase> const& GetQueue() const { return _queue; }

    // Test-support: forces an auction directly into the queue with an explicit buyTime,
    // bypassing ConsiderForPurchase's price/RNG logic, for deterministic tests.
    void EnqueueForTest(AuctionEntry* auction, time_t buyTime);
    void EnqueueBidForTest(
        AuctionEntry* auction, time_t buyTime, uint32 marketCeilingPerUnit, uint32 vendorBuyPrice = 0);

private:
    void Execute(QueuedPurchase const& entry);
    void BuyItem(QueuedPurchase const& entry);
    void PlaceBid(QueuedPurchase const& entry);

    Bot& _bot;
    AuctionPricing::BuyTolerance _tolerance{};
    std::vector<QueuedPurchase> _queue;
    std::unordered_set<uint32> _queuedAuctionIds;
};
