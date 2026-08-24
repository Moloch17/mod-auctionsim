#pragma once
#include <string>
#include <vector>
#include "AuctionHouseMgr.h"

class ASConfig;
class AuctionListingService;
class Bot;

// Self-checks for the ".auctionsim test" GM command. Every test reports a clear
// pass/fail rather than asserting/throwing, so a broken invariant is reported,
// not a crash.
namespace AuctionSimTests
{
    struct TestResult
    {
        std::string name;
        bool passed;
        std::string detail;
    };

    // Logic/data checks only -- no DB writes, no live auction house mutation.
    std::vector<TestResult> RunLogicTests(Bot& bot, ASConfig const& config);

    // End-to-end: lists one real temporary auction via the bot, verifies it appears
    // in the given house, then deletes it. Briefly mutates live game state.
    TestResult RunLiveListingTest(
        Bot& bot, ASConfig const& config, AuctionListingService& listingService, AuctionHouseId houseId);

    // End-to-end: lists one real temporary auction, forces it into a throwaway
    // AuctionBuyingService's queue as already due, processes the queue, and verifies
    // the auction was actually bought (removed from the house). Briefly mutates live
    // game state; does not touch the real bot's live buy queue.
    TestResult RunLiveBuyingTest(
        Bot& bot, ASConfig const& config, AuctionListingService& listingService, AuctionHouseId houseId);

    // End-to-end: verifies AuctionListingService::ListOneItem actually enforces the
    // level caps, not just that AuctionPricing::IsWithinLevelCap is correct in isolation.
    // Temporarily overrides config's caps (restoring them before returning, even on
    // failure) to confirm a real candidate is blocked when above either cap and listed
    // when both are disabled; cleans up anything it lists. ASConfig& is non-const
    // specifically for this temporary override -- every other test only reads config.
    TestResult RunLiveLevelCapTest(
        Bot& bot, ASConfig& config, AuctionListingService& listingService, AuctionHouseId houseId);
}
