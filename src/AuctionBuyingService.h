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
// then executed a few at a time as their rolled buy time comes due.
class AuctionBuyingService
{
public:
    struct QueuedPurchase
    {
        AuctionEntry* auction;
        time_t buyTime;
    };

    explicit AuctionBuyingService(Bot& bot);

    // Rolls a fresh buy-tolerance profile for the upcoming scan pass. Call once
    // per ScanAuctions() invocation, before any ConsiderForPurchase() calls.
    void RollTolerance();

    // Called once per non-bot-owned auction found during a scan pass; queues it
    // for purchase per this scan's tolerance profile and the auction's remaining lifetime.
    // A no-op if this auction is already queued (e.g. a re-scan before the first
    // queued purchase fired), so an auction can never end up in the queue twice.
    void ConsiderForPurchase(AuctionEntry* auction, uint32 pricePerItem, uint32 meanPrice, uint32 maxPrice);

    // Sorts the queue so the soonest-due purchase is processed first. Call once
    // after a scan pass has finished calling ConsiderForPurchase.
    void SortQueue();

    // Executes at most one due purchase from the queue. Safe to call every tick.
    void ProcessDueQueue();

    size_t QueueSize() const { return _queue.size(); }

    // Read-only view of the current queue, soonest-due last (matches SortQueue's order).
    // For reporting only (e.g. ".auctionsim showqueue") -- entries are AuctionEntry*, only
    // valid until the next ProcessDueQueue()/scan pass on this same world tick.
    std::vector<QueuedPurchase> const& GetQueue() const { return _queue; }

    // Test-support: forces an auction directly into the queue with an explicit buyTime,
    // bypassing ConsiderForPurchase's price/RNG logic, for deterministic tests.
    void EnqueueForTest(AuctionEntry* auction, time_t buyTime);

private:
    void BuyItem(AuctionEntry* auction, AuctionHouseId houseId);

    Bot& _bot;
    AuctionPricing::BuyTolerance _tolerance{};
    std::vector<QueuedPurchase> _queue;
    std::unordered_set<uint32> _queuedAuctionIds;
};
