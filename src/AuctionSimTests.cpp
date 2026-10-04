#include "AuctionSimTests.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
#include "ASConfig.h"
#include "AuctionSim.h"
#include "AuctionBuyingService.h"
#include "AuctionListingService.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "CharacterCache.h"
#include "CraftedItems.h"
#include "Item.h"
#include "MarketBots.h"
#include "MarketData.h"
#include "MarketEngine.h"
#include "MarketRng.h"
#include "MarketService.h"
#include "GameTime.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScannedItem.h"
#include "StringFormat.h"

namespace
{
    using AuctionSimTests::TestResult;

    TestResult Pass(std::string name, std::string detail = "ok")
    {
        return {std::move(name), true, std::move(detail)};
    }

    TestResult Fail(std::string name, std::string detail)
    {
        return {std::move(name), false, std::move(detail)};
    }

    TestResult TestBotValid(Bot& bot)
    {
        if (!bot.GetSession())
        {
            return Fail("Bot session valid", "bot has no WorldSession");
        }
        if (!bot.GetPlayer())
        {
            return Fail("Bot session valid", "bot has no Player");
        }
        return Pass("Bot session valid", Acore::StringFormat("player guid {}", bot.GetPlayer()->GetGUID().ToString()));
    }

    TestResult TestPriceDataLoaded(ASConfig const& config)
    {
        if (config.ScanData.empty())
        {
            return Fail("Price data loaded", "ScanData is empty -- auctionsim.dat failed to load");
        }
        return Pass("Price data loaded", Acore::StringFormat("{} items", config.ScanData.size()));
    }

    TestResult TestBothFactionsHavePriceData(ASConfig const& config)
    {
        size_t allianceCount = 0;
        size_t hordeCount = 0;
        for (ScannedItem const& item : config.ScanData)
        {
            if (item.GetFactionNum() == static_cast<uint8>(AuctionHouseId::Alliance))
            {
                allianceCount++;
            }
            else if (item.GetFactionNum() == static_cast<uint8>(AuctionHouseId::Horde))
            {
                hordeCount++;
            }
        }

        if (allianceCount == 0 || hordeCount == 0)
        {
            return Fail(
                "Both factions have price data",
                Acore::StringFormat("alliance={}, horde={}", allianceCount, hordeCount));
        }
        return Pass(
            "Both factions have price data", Acore::StringFormat("alliance={}, horde={}", allianceCount, hordeCount));
    }

    TestResult TestListingMasksConfigured(ASConfig const& config)
    {
        for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; itemClass++)
        {
            for (uint32 quality = 0; quality < MAX_ITEM_QUALITY; quality++)
            {
                if (config.ItemSelectionMask[itemClass][quality] > 0.0f)
                {
                    return Pass("Listing masks configured");
                }
            }
        }
        return Fail("Listing masks configured", "every listing multiplier is 0 -- nothing will ever be listed");
    }

    TestResult TestFindScannedItemRoundTrip(ASConfig const& config)
    {
        for (ScannedItem const& item : config.ScanData)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.GetItemID());
            if (!proto)
            {
                continue;
            }

            auto houseId = static_cast<AuctionHouseId>(item.GetFactionNum());
            ScannedItem const* found = config.FindScannedItem(houseId, proto->Class, proto->Quality, item.GetItemID());
            if (!found || found->GetItemID() != item.GetItemID())
            {
                return Fail(
                    "FindScannedItem round-trip",
                    Acore::StringFormat("item {} not found back in its own bucket", item.GetItemID()));
            }
            return Pass("FindScannedItem round-trip", Acore::StringFormat("verified via item {}", item.GetItemID()));
        }
        return Fail("FindScannedItem round-trip", "no ScanData entry resolves to a valid item_template to test with");
    }

    TestResult TestRollStackSizeBounds()
    {
        for (uint32 itemMaxStack : {1u, 2u, 5u, 20u, 200u})
        {
            for (int i = 0; i < 50; i++)
            {
                // typical/spread taken deliberately wider than itemMaxStack to
                // exercise the clamp.
                uint32 qty = AuctionPricing::RollStackSize(300, 1, 300, itemMaxStack);
                if (qty < 1 || qty > itemMaxStack)
                {
                    return Fail(
                        "RollStackSize bounds",
                        Acore::StringFormat("itemMaxStack={} produced qty={}", itemMaxStack, qty));
                }
            }
        }

        // Zero-width spread always yields the typical size (clamped).
        for (int i = 0; i < 20; i++)
        {
            if (AuctionPricing::RollStackSize(20, 20, 20, 200) != 20)
            {
                return Fail("RollStackSize bounds", "zero-width spread did not return the typical size");
            }
        }
        return Pass("RollStackSize bounds");
    }

    TestResult TestIsListablePriceBoundary()
    {
        if (AuctionPricing::IsListablePrice(2))
        {
            return Fail("IsListablePrice boundary", "price 2 was reported listable");
        }
        if (!AuctionPricing::IsListablePrice(3))
        {
            return Fail("IsListablePrice boundary", "price 3 was reported not listable");
        }
        return Pass("IsListablePrice boundary");
    }

    TestResult TestRollAuctionDurationBounds()
    {
        for (int i = 0; i < 50; i++)
        {
            uint32 duration = AuctionPricing::RollAuctionDuration();
            if (duration < 3600 || duration > 43200)
            {
                return Fail("RollAuctionDuration bounds", Acore::StringFormat("rolled {} seconds", duration));
            }
        }
        return Pass("RollAuctionDuration bounds");
    }

    TestResult TestRollBuyoutPriceSanity()
    {
        constexpr uint32 low = 60;
        constexpr uint32 market = 100;
        constexpr uint32 high = 150;
        constexpr uint32 quantity = 5;

        // Healthy sample: draws stay within the trimmed [low, high] band.
        for (int i = 0; i < 50; i++)
        {
            uint32 buyout = AuctionPricing::RollBuyoutPrice(low, market, high, quantity, 50);
            if (buyout < quantity * low || buyout > quantity * high)
            {
                return Fail("RollBuyoutPrice sanity", Acore::StringFormat("healthy sample rolled {} copper", buyout));
            }
        }

        // Thin sample with no observed spread: falls back to a small synthetic
        // band around the market price rather than pinning every listing to it.
        for (int i = 0; i < 50; i++)
        {
            uint32 buyout = AuctionPricing::RollBuyoutPrice(market, market, market, quantity, 1);
            if (buyout < quantity * 90 || buyout > quantity * 110)
            {
                return Fail("RollBuyoutPrice sanity", Acore::StringFormat("thin sample rolled {} copper", buyout));
            }
        }

        return Pass("RollBuyoutPrice sanity");
    }

    TestResult TestScannedItemParse()
    {
        // Old shorter formats are rejected: rows are a fixed 54 fields now.
        if (ScannedItem::TryParse("6:6543:-19:23229:8000:99000"))
        {
            return Fail("ScannedItem parse", "6-field (old format) line was accepted");
        }
        if (ScannedItem::TryParse(
                "6:6543:-19:12:8000:99000:23229:30000:8000:19000:30843:8100:31686:23000:29500:8000"))
        {
            return Fail("ScannedItem parse", "16-field (old gear format) line was accepted");
        }
        // The pre-bid 41-field format is also rejected now.
        if (ScannedItem::TryParse(
                "6:6543:-19:12"
                ":8000:99000:23229:30000:8000:19000:30843:8100:31686:23000:29500:8000"
                ":2:20:14:20:20:5:20:2:20:14:20:20"
                ":1:12:4:3:2:2:6:1:9:4:3:2"
                ":47"))
        {
            return Fail("ScannedItem parse", "41-field (pre-bid format) line was accepted");
        }

        // 4 identity + 12 price + 12 stack + 12 listing-count + 12 bid-ratio
        //   + bidRatioSampleCount + listingSnapshotCount = 54.
        //   price    : adjLow 8100  adjHigh 31686  adjMedian 29500  q3 30843
        //   stack    : adjMode 20   adjLow 2       adjHigh 20
        //   list     : adjMedian 3                 snapshotCount 47
        //   bidRatio : adjLow 5000  adjHigh 9000   adjMedian 7000   sampleCount 9
        auto parsed = ScannedItem::TryParse(
            "6:6543:-19:12"
            ":8000:99000:23229:30000:8000:19000:30843:8100:31686:23000:29500:8000"
            ":2:20:14:20:20:5:20:2:20:14:20:20"
            ":1:12:4:3:2:2:6:1:9:4:3:2"
            ":4000:9500:7000:7100:7000:5500:8500:5000:9000:7050:7000:7000"
            ":9"
            ":47");
        if (!parsed)
        {
            return Fail("ScannedItem parse", "valid 54-field line was rejected");
        }

        ScannedItem const& s = *parsed;
        if (s.GetFactionNum() != 6 || s.GetItemID() != 6543 || s.GetSuffixID() != -19 || s.GetSampleCount() != 12)
        {
            return Fail("ScannedItem parse", "identity fields did not round-trip");
        }
        if (s.GetMarketPrice() != 29500)  // adjMedian
        {
            return Fail("ScannedItem parse", Acore::StringFormat("market price {} (expected adjMedian 29500)", s.GetMarketPrice()));
        }
        if (s.GetListLow() != 8100 || s.GetListHigh() != 31686)  // adjLow / adjHigh
        {
            return Fail("ScannedItem parse", "list band did not match adjLow/adjHigh");
        }
        if (s.GetBuyCeiling() != 30843)  // q3
        {
            return Fail("ScannedItem parse", Acore::StringFormat("buy ceiling {} (expected q3 30843)", s.GetBuyCeiling()));
        }
        // Stack block is always present now -- gear included.
        if (s.GetTypicalStackSize() != 20 || s.GetStackLow() != 2 || s.GetStackHigh() != 20)
        {
            return Fail(
                "ScannedItem parse",
                Acore::StringFormat(
                    "stack stats: typical={} low={} high={} (expected 20/2/20)",
                    s.GetTypicalStackSize(), s.GetStackLow(), s.GetStackHigh()));
        }
        if (s.GetTypicalListingCount() != 3)  // listAdjMedian
        {
            return Fail(
                "ScannedItem parse",
                Acore::StringFormat("typical listing count {} (expected listAdjMedian 3)", s.GetTypicalListingCount()));
        }
        if (s.GetListingSnapshotCount() != 47)
        {
            return Fail(
                "ScannedItem parse",
                Acore::StringFormat("listing snapshot count {} (expected 47)", s.GetListingSnapshotCount()));
        }
        if (s.GetBidRatioTypicalBp() != 7000 || s.GetBidRatioLowBp() != 5000 || s.GetBidRatioHighBp() != 9000)
        {
            return Fail(
                "ScannedItem parse",
                Acore::StringFormat(
                    "bid-ratio bp: typical={} low={} high={} (expected 7000/5000/9000)",
                    s.GetBidRatioTypicalBp(), s.GetBidRatioLowBp(), s.GetBidRatioHighBp()));
        }
        if (s.GetBidRatioSampleCount() != 9)
        {
            return Fail(
                "ScannedItem parse",
                Acore::StringFormat("bid-ratio sample count {} (expected 9)", s.GetBidRatioSampleCount()));
        }
        return Pass("ScannedItem parse");
    }

    TestResult TestDataVersionHeaderParse()
    {
        bool consumed = false;

        if (ASConfig::ParseDataVersionLine("AUCTIONSIM_DAT 1", consumed) != 1 || !consumed)
        {
            return Fail("Data version header parse", "'AUCTIONSIM_DAT 1' did not parse as (1, consumed)");
        }
        if (ASConfig::ParseDataVersionLine("AUCTIONSIM_DAT 7", consumed) != 7 || !consumed)
        {
            return Fail("Data version header parse", "'AUCTIONSIM_DAT 7' did not parse as (7, consumed)");
        }
        // A legacy unversioned file: line 1 is the "N M" header, no stamp.
        if (ASConfig::ParseDataVersionLine("131615 108", consumed) != 0 || consumed)
        {
            return Fail("Data version header parse", "legacy 'N M' line was treated as a version stamp");
        }
        if (ASConfig::ParseDataVersionLine("AUCTIONSIM_DAT", consumed) != 0 || consumed)
        {
            return Fail("Data version header parse", "'AUCTIONSIM_DAT' with no number was accepted");
        }
        return Pass("Data version header parse");
    }

    TestResult TestRollBuyToleranceBounds()
    {
        for (int i = 0; i < 50; i++)
        {
            AuctionPricing::BuyTolerance tolerance = AuctionPricing::RollBuyTolerance();
            if (tolerance.boundaryPercent < 0.5f || tolerance.boundaryPercent > 0.7f)
            {
                return Fail(
                    "RollBuyTolerance bounds", Acore::StringFormat("rolled boundary {}", tolerance.boundaryPercent));
            }
        }
        return Pass("RollBuyTolerance bounds");
    }

    TestResult TestShouldBuyAtPriceBoundaries()
    {
        AuctionPricing::BuyTolerance tolerance{0.6f};

        if (!AuctionPricing::ShouldBuyAtPrice(100, 100, 200, tolerance, 1))
        {
            return Fail("ShouldBuyAtPrice boundaries", "price at mean was not always-buy");
        }
        if (AuctionPricing::ShouldBuyAtPrice(201, 100, 200, tolerance, 1))
        {
            return Fail("ShouldBuyAtPrice boundaries", "price above max was bought");
        }
        if (AuctionPricing::ShouldBuyAtPrice(150, 100, 100, tolerance, 1))
        {
            return Fail("ShouldBuyAtPrice boundaries", "degenerate maxPrice<=meanPrice was bought above mean");
        }
        return Pass("ShouldBuyAtPrice boundaries");
    }

    TestResult TestRollBuyTimeBounds()
    {
        constexpr time_t now = 1'000'000;

        // Plenty of time left -- delay must be capped at 20 minutes and never past expiry.
        time_t farBuyTime = AuctionPricing::RollBuyTime(now + 100000, now);
        if (farBuyTime < now || farBuyTime > now + 1200)
        {
            return Fail("RollBuyTime bounds", Acore::StringFormat("far case rolled {}", farBuyTime - now));
        }

        // Already expired -- must not roll before now or crash on a negative window.
        time_t expiredBuyTime = AuctionPricing::RollBuyTime(now - 5, now);
        if (expiredBuyTime != now)
        {
            return Fail(
                "RollBuyTime bounds", Acore::StringFormat("already-expired case rolled {}", expiredBuyTime - now));
        }

        return Pass("RollBuyTime bounds");
    }

    TestResult TestCalculateRemainingScans()
    {
        uint32 interval = AuctionPricing::kScanIntervalSeconds;

        if (AuctionPricing::CalculateRemainingScans(0) != 1)
        {
            return Fail("CalculateRemainingScans", "0 remaining seconds should be 1 scan");
        }
        if (AuctionPricing::CalculateRemainingScans(-100) != 1)
        {
            return Fail("CalculateRemainingScans", "negative remaining seconds should be 1 scan");
        }
        if (AuctionPricing::CalculateRemainingScans(static_cast<time_t>(interval)) != 1)
        {
            return Fail("CalculateRemainingScans", "exactly one interval should be 1 scan");
        }
        if (AuctionPricing::CalculateRemainingScans(static_cast<time_t>(interval) + 1) != 2)
        {
            return Fail("CalculateRemainingScans", "one interval plus one second should round up to 2 scans");
        }
        return Pass("CalculateRemainingScans");
    }

    TestResult TestListingCountMath()
    {
        if (AuctionPricing::CalculateItemsToList(5, 2) != 3)
        {
            return Fail("Listing count math", "target 5 minus existing 2 should be 3");
        }

        for (int i = 0; i < 100; i++)
        {
            uint32 target = AuctionPricing::RollCategoryTarget(4, 9);
            if (target < 4 || target > 9)
            {
                return Fail("Listing count math", Acore::StringFormat("RollCategoryTarget(4,9) rolled {}", target));
            }
        }
        // Order-tolerant: swapped bounds behave the same.
        for (int i = 0; i < 20; i++)
        {
            uint32 target = AuctionPricing::RollCategoryTarget(9, 4);
            if (target < 4 || target > 9)
            {
                return Fail("Listing count math", Acore::StringFormat("RollCategoryTarget(9,4) rolled {}", target));
            }
        }
        // Degenerate band collapses to the single value.
        if (AuctionPricing::RollCategoryTarget(7, 7) != 7)
        {
            return Fail("Listing count math", "RollCategoryTarget(7,7) did not return 7");
        }
        return Pass("Listing count math");
    }

    TestResult TestWeightedPick()
    {
        // All-zero weights -> the "nothing to pick" sentinel (== size()).
        if (AuctionPricing::WeightedPick({0, 0, 0}) != 3)
        {
            return Fail("WeightedPick", "all-zero weights did not return the size() sentinel");
        }
        // The only non-zero entry is always chosen.
        for (int i = 0; i < 50; i++)
        {
            if (AuctionPricing::WeightedPick({0, 7, 0}) != 1)
            {
                return Fail("WeightedPick", "the only non-zero weight was not always picked");
            }
        }
        // Uniform weights: every index is in range and reachable.
        bool seen[3] = {false, false, false};
        for (int i = 0; i < 400; i++)
        {
            size_t idx = AuctionPricing::WeightedPick({1, 1, 1});
            if (idx >= 3)
            {
                return Fail("WeightedPick", Acore::StringFormat("returned out-of-range index {}", idx));
            }
            seen[idx] = true;
        }
        if (!seen[0] || !seen[1] || !seen[2])
        {
            return Fail("WeightedPick", "uniform weights never reached some index across 400 draws");
        }
        return Pass("WeightedPick");
    }

    TestResult TestCategoryDepthParse(ASConfig const& config)
    {
        for (uint32 house = 0; house < ASConfig::kAuctionHouseIndexBound; house++)
        {
            for (uint32 itemClass = 0; itemClass < MAX_ITEM_CLASS; itemClass++)
            {
                for (uint32 quality = 0; quality < MAX_ITEM_QUALITY; quality++)
                {
                    ASConfig::CategoryDepth const& depth =
                        config.GetCategoryDepth(static_cast<AuctionHouseId>(house), itemClass, quality);
                    if (!depth.has)
                    {
                        continue;
                    }
                    if (depth.q1 > depth.median)
                    {
                        return Fail(
                            "Category depth parse",
                            Acore::StringFormat(
                                "house={} class={} quality={}: q1 {} > median {}",
                                house, itemClass, quality, depth.q1, depth.median));
                    }
                    return Pass(
                        "Category depth parse",
                        Acore::StringFormat(
                            "house={} class={} quality={} q1={} median={}",
                            house, itemClass, quality, depth.q1, depth.median));
                }
            }
        }
        return Fail("Category depth parse", "no category-depth rows were loaded from auctionsim.dat");
    }

    TestResult TestIsWithinLevelCapBoundary()
    {
        if (!AuctionPricing::IsWithinLevelCap(80, 200, 0, 0))
        {
            return Fail("IsWithinLevelCap boundary", "disabled caps (0, 0) rejected an item");
        }
        if (!AuctionPricing::IsWithinLevelCap(70, 150, 70, 0))
        {
            return Fail("IsWithinLevelCap boundary", "item exactly at the required-level cap was rejected");
        }
        if (AuctionPricing::IsWithinLevelCap(71, 150, 70, 0))
        {
            return Fail("IsWithinLevelCap boundary", "item above the required-level cap was accepted");
        }
        if (AuctionPricing::IsWithinLevelCap(70, 201, 0, 200))
        {
            return Fail("IsWithinLevelCap boundary", "item above the item-level cap was accepted");
        }
        return Pass("IsWithinLevelCap boundary");
    }

    TestResult TestIsWithinVendorBuyPriceBoundary()
    {
        if (!AuctionPricing::IsWithinVendorBuyPrice(1'000'000, 0))
        {
            return Fail("IsWithinVendorBuyPrice boundary", "buyPrice 0 (no vendor purchase price) rejected a buy");
        }
        if (!AuctionPricing::IsWithinVendorBuyPrice(500, 500))
        {
            return Fail("IsWithinVendorBuyPrice boundary", "price equal to the vendor buy price was rejected");
        }
        if (!AuctionPricing::IsWithinVendorBuyPrice(499, 500))
        {
            return Fail("IsWithinVendorBuyPrice boundary", "price below the vendor buy price was rejected");
        }
        if (AuctionPricing::IsWithinVendorBuyPrice(501, 500))
        {
            return Fail("IsWithinVendorBuyPrice boundary", "price above the vendor buy price was accepted");
        }
        return Pass("IsWithinVendorBuyPrice boundary");
    }

    TestResult TestIsBuyableQuality()
    {
        if (AuctionPricing::IsBuyableQuality(0))
        {
            return Fail("IsBuyableQuality", "poor/grey quality (0) was reported buyable");
        }
        for (uint32 quality = 1; quality < MAX_ITEM_QUALITY; quality++)
        {
            if (!AuctionPricing::IsBuyableQuality(quality))
            {
                return Fail(
                    "IsBuyableQuality", Acore::StringFormat("quality {} was reported not buyable", quality));
            }
        }
        return Pass("IsBuyableQuality");
    }

    // Heap-allocates a bare AuctionEntry for queue-mechanics tests that never reach
    // BuyItem (so it's never freed via AuctionHouseObject::RemoveAuction). Callers
    // that don't process it must delete it themselves.
    AuctionEntry* MakeTestAuctionEntry(uint32 id, time_t expireTime)
    {
        AuctionEntry* auction = new AuctionEntry();
        auction->Id = id;
        auction->expire_time = expireTime;
        return auction;
    }

    TestResult TestBuyQueuePopulatesOnQualifyingPrice(Bot& bot)
    {
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFF0, GameTime::GetGameTime().count() + 100000);

        AuctionBuyingService testService(bot);
        testService.ConsiderForPurchase(testAuction, 1, 1'000'000, 2'000'000);  // always-buy: 1 <= mean
        bool ok = testService.QueueSize() == 1;

        delete testAuction;

        if (!ok)
        {
            return Fail("Buy queue populates on qualifying price", "queue size was not 1 after one qualifying call");
        }
        return Pass("Buy queue populates on qualifying price");
    }

    TestResult TestBuyQueueDedupesRescan(Bot& bot)
    {
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFF1, GameTime::GetGameTime().count() + 100000);

        AuctionBuyingService testService(bot);
        testService.ConsiderForPurchase(testAuction, 1, 1'000'000, 2'000'000);
        testService.ConsiderForPurchase(testAuction, 1, 1'000'000, 2'000'000);
        bool ok = testService.QueueSize() == 1;

        delete testAuction;

        if (!ok)
        {
            return Fail("Buy queue dedupes on rescan", "queue size was not 1 after two calls for the same auction");
        }
        return Pass("Buy queue dedupes on rescan");
    }

    TestResult TestBuyQueueNotYetDue(Bot& bot)
    {
        time_t now = GameTime::GetGameTime().count();
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFF2, now + 100000);

        AuctionBuyingService testService(bot);
        testService.EnqueueForTest(testAuction, now + 10000);
        testService.ProcessDueQueue();
        bool ok = testService.QueueSize() == 1;

        delete testAuction;

        if (!ok)
        {
            return Fail("Buy queue leaves not-yet-due items alone", "queue was drained before the item was due");
        }
        return Pass("Buy queue leaves not-yet-due items alone");
    }

    TestResult TestReplayBiddingConfig()
    {
        std::string const name = "Replay bidding config";
        struct Case
        {
            char const* text;
            bool parses;
            bool value;
        };
        Case const cases[] = {
            {"", true, true},  // key missing: today's behaviour
            {"1", true, true},
            {"0", true, false},
            {"true", true, true},
            {"FALSE", true, false},
            {" 0 ", true, false},
            {"\"1\"", true, true},
            {"2", false, true},
            {"maybe", false, true},
        };
        for (Case const& c : cases)
        {
            bool value = true;
            bool parsed = ASConfig::ParseReplayBidding(c.text, value);
            if (parsed != c.parses || (parsed && value != c.value))
            {
                return Fail(name, Acore::StringFormat("'{}' parsed {} as {}", c.text, parsed, value));
            }
        }

        // The scan's gate: never with bidding off; with it on, an outbid or an opening bid.
        for (bool holds : {false, true})
        {
            for (bool openable : {false, true})
            {
                if (AuctionPricing::MayBid(false, holds, openable) ||
                    AuctionPricing::MayBid(true, holds, openable) != (holds || openable))
                {
                    return Fail(name, Acore::StringFormat("MayBid wrong for holds={} openable={}", holds, openable));
                }
            }
        }
        return Pass(name);
    }

    // Bidding off: queued bids are dropped, buyouts stay, and no new bid is queued.
    TestResult TestBidQueueBiddingOff(Bot& bot)
    {
        std::string const name = "Bid queue with bidding off";
        time_t now = GameTime::GetGameTime().count();
        AuctionEntry* buyout = MakeTestAuctionEntry(0xFFFFFF40, now + 100000);
        AuctionEntry* bid = MakeTestAuctionEntry(0xFFFFFF41, now + 100000);
        AuctionEntry* fresh = MakeTestAuctionEntry(0xFFFFFF42, now + 100000);
        fresh->startbid = 10;
        fresh->itemCount = 1;

        AuctionBuyingService testService(bot);
        testService.EnqueueForTest(buyout, now + 10000);
        testService.EnqueueBidForTest(bid, now + 10000, 1'000'000);
        testService.SetBiddingEnabled(false);
        bool const dropped = testService.QueueSize() == 1 && testService.IsQueued(buyout->Id) &&
                             !testService.IsQueued(bid->Id);

        AuctionBuyingService::BidLimits limits;
        limits.valuationLowPerUnit = 1'000'000;
        limits.marketPerUnit = 1'000'000;
        testService.ConsiderForBid(fresh, limits);
        bool const noneQueued = !testService.IsQueued(fresh->Id);

        delete buyout;
        delete bid;
        delete fresh;

        if (!dropped)
        {
            return Fail(name, "turning bidding off didn't drop exactly the queued bid");
        }
        if (!noneQueued)
        {
            return Fail(name, "a bid was queued with bidding off");
        }
        return Pass(name);
    }

    TestResult TestRollStartBidBounds()
    {
        // Healthy sample: startbid stays inside [buyout*lowRatio, buyout*highRatio].
        for (int i = 0; i < 200; i++)
        {
            uint32 startbid = AuctionPricing::RollStartBid(100000, 5000, 7000, 9000, 40);
            if (startbid < 1 || startbid > 100000)
            {
                return Fail("RollStartBid bounds", Acore::StringFormat("healthy sample rolled {}", startbid));
            }
            if (startbid < 45000 || startbid > 95000)  // 0.45..0.95 of buyout, +slack
            {
                return Fail(
                    "RollStartBid bounds",
                    Acore::StringFormat("healthy sample {} outside the ratio band", startbid));
            }
        }

        // Thin sample: small synthetic band around typical ratio (0.70 -> ~0.644..0.756).
        for (int i = 0; i < 200; i++)
        {
            uint32 startbid = AuctionPricing::RollStartBid(100000, 7000, 7000, 7000, 1);
            if (startbid < 60000 || startbid > 80000)
            {
                return Fail("RollStartBid bounds", Acore::StringFormat("thin sample rolled {}", startbid));
            }
        }

        // Ratio > 1.0 (bad data) still yields startbid <= buyout.
        for (int i = 0; i < 50; i++)
        {
            uint32 startbid = AuctionPricing::RollStartBid(5000, 9000, 12000, 15000, 40);
            if (startbid < 1 || startbid > 5000)
            {
                return Fail("RollStartBid bounds", Acore::StringFormat("ratio>1 rolled {}", startbid));
            }
        }

        // Degenerate buyout.
        if (AuctionPricing::RollStartBid(1, 5000, 7000, 9000, 40) != 1)
        {
            return Fail("RollStartBid bounds", "buyout 1 did not return startbid 1");
        }

        return Pass("RollStartBid bounds");
    }

    TestResult TestShouldBidAtPriceBoundaries()
    {
        // Hard gate: never at or above market, never with market 0.
        if (AuctionPricing::ShouldBidAtPrice(100, 100))
        {
            return Fail("ShouldBidAtPrice boundaries", "bid equal to market was allowed");
        }
        if (AuctionPricing::ShouldBidAtPrice(150, 100))
        {
            return Fail("ShouldBidAtPrice boundaries", "bid above market was allowed");
        }
        if (AuctionPricing::ShouldBidAtPrice(50, 0))
        {
            return Fail("ShouldBidAtPrice boundaries", "bid allowed with market price 0");
        }

        // Per-scan roll below market: a clear deal (well under market) fires more
        // often than a slim margin (just under market), and neither is a certainty.
        int dealHits = 0, thinHits = 0;
        for (int i = 0; i < 600; i++)
        {
            if (AuctionPricing::ShouldBidAtPrice(50, 100)) dealHits++;   // position 0.50 -> deal tier
            if (AuctionPricing::ShouldBidAtPrice(95, 100)) thinHits++;   // position 0.95 -> thin tier
        }
        if (dealHits == 0 || dealHits == 600)
        {
            return Fail(
                "ShouldBidAtPrice boundaries",
                Acore::StringFormat("deal tier not probabilistic ({}/600)", dealHits));
        }
        if (thinHits >= dealHits)
        {
            return Fail(
                "ShouldBidAtPrice boundaries",
                Acore::StringFormat("thin tier ({}) did not fire less than deal tier ({})", thinHits, dealHits));
        }

        // Opening bids: clear deals only, and rarer than outbids on the same deal.
        int openHits = 0;
        for (int i = 0; i < 600; i++)
        {
            if (AuctionPricing::ShouldBidAtPrice(95, 100, true))
            {
                return Fail("ShouldBidAtPrice boundaries", "opened on a slim margin");
            }
            if (AuctionPricing::ShouldBidAtPrice(50, 100, true)) openHits++;
        }
        if (openHits == 0 || openHits >= dealHits)
        {
            return Fail(
                "ShouldBidAtPrice boundaries",
                Acore::StringFormat("opening hits {} not in (0, deal hits {})", openHits, dealHits));
        }

        return Pass("ShouldBidAtPrice boundaries");
    }

    // Bid limits with a one-point valuation band, so the rolled valuation is known.
    AuctionBuyingService::BidLimits FlatLimits(uint32 valuationPerUnit, uint32 vendorBuyPrice = 0)
    {
        AuctionBuyingService::BidLimits limits;
        limits.valuationLowPerUnit = valuationPerUnit;
        limits.marketPerUnit = valuationPerUnit;
        limits.vendorBuyPrice = vendorBuyPrice;
        return limits;
    }

    TestResult TestRollBidValuationBounds()
    {
        for (int i = 0; i < 200; i++)
        {
            uint32 v = AuctionPricing::RollBidValuation(700, 1000);
            if (v < 700 || v > 1000)
            {
                return Fail("RollBidValuation bounds", Acore::StringFormat("rolled {} outside [700, 1000]", v));
            }
        }
        if (AuctionPricing::RollBidValuation(1000, 700) > 1000)  // swapped band still bounded
        {
            return Fail("RollBidValuation bounds", "swapped band rolled above its high end");
        }
        return Pass("RollBidValuation bounds");
    }

    TestResult TestRollBidAmountBounds()
    {
        // Never below the minimum; at most the round-up plus a two-step jump.
        struct Case
        {
            uint32 minimum, step;
        };
        for (Case c : {Case{57, 1}, Case{4723, 100}, Case{43712, 1000}, Case{521234, 10000}})
        {
            if (AuctionPricing::BidRoundingStep(c.minimum) != c.step)
            {
                return Fail(
                    "RollBidAmount bounds",
                    Acore::StringFormat("minimum {} rounds by {}, expected {}", c.minimum,
                        AuctionPricing::BidRoundingStep(c.minimum), c.step));
            }
            for (int i = 0; i < 200; i++)
            {
                uint32 amount = AuctionPricing::RollBidAmount(c.minimum);
                if (amount < c.minimum || amount >= c.minimum + 3 * c.step)
                {
                    return Fail(
                        "RollBidAmount bounds",
                        Acore::StringFormat("minimum {} rolled {} (step {})", c.minimum, amount, c.step));
                }
                if (amount != c.minimum && amount % c.step != 0)
                {
                    return Fail(
                        "RollBidAmount bounds",
                        Acore::StringFormat("minimum {} rolled {}, not a clean multiple of {}", c.minimum, amount,
                            c.step));
                }
            }
        }
        return Pass("RollBidAmount bounds");
    }

    TestResult TestBidTimingNoSniping()
    {
        constexpr time_t now = 1'000'000;
        if (!AuctionPricing::IsTooLateToBid(now + AuctionPricing::kNoBidBeforeExpirySeconds, now) ||
            AuctionPricing::IsTooLateToBid(now + AuctionPricing::kNoBidBeforeExpirySeconds + 1, now))
        {
            return Fail("Bid timing: no sniping", "the no-bid window boundary is off");
        }
        for (int i = 0; i < 200; i++)
        {
            time_t expire = now + AuctionPricing::kNoBidBeforeExpirySeconds + 600;
            time_t bidTime = AuctionPricing::RollBidTime(expire, now);
            if (bidTime < now || AuctionPricing::IsTooLateToBid(expire, bidTime))
            {
                return Fail(
                    "Bid timing: no sniping",
                    Acore::StringFormat("bid rolled for {}s before expiry", expire - bidTime));
            }
        }
        return Pass("Bid timing: no sniping");
    }

    TestResult TestBidQueueSkipsLastMinutes(Bot& bot)
    {
        time_t now = GameTime::GetGameTime().count();
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE5, now + AuctionPricing::kNoBidBeforeExpirySeconds);
        testAuction->itemCount = 1;
        testAuction->bid = 10;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00005u);

        AuctionBuyingService testService(bot);
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(1'000'000));
        }
        bool ok = testService.QueueSize() == 0;

        delete testAuction;

        if (!ok)
        {
            return Fail("Bid queue skips the last 30 minutes", "a bid was queued inside the no-bid window");
        }
        return Pass("Bid queue skips the last 30 minutes");
    }

    TestResult TestBidValuationSticks(Bot& bot)
    {
        // First look rolls a 500/unit valuation; a later scan offering a far higher
        // band must not raise it, so the 1050 next bid is never queued.
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE6, GameTime::GetGameTime().count() + 100000);
        testAuction->itemCount = 1;
        testAuction->bid = 1000;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00006u);

        AuctionBuyingService testService(bot);
        testService.ConsiderForBid(testAuction, FlatLimits(500));
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(1'000'000));
        }
        bool ok = testService.QueueSize() == 0;

        delete testAuction;

        if (!ok)
        {
            return Fail("Bid valuation sticks", "a later scan raised the auction's valuation");
        }
        return Pass("Bid valuation sticks");
    }

    TestResult TestBidQueueCheapestBuyoutCap(Bot& bot)
    {
        // Next bid 1050/unit; the same item can be bought outright for 1050 elsewhere.
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE7, GameTime::GetGameTime().count() + 100000);
        testAuction->itemCount = 1;
        testAuction->bid = 1000;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00007u);

        AuctionBuyingService testService(bot);
        AuctionBuyingService::BidLimits limits = FlatLimits(1'000'000);
        limits.cheapestBuyoutPerUnit = 1050;
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, limits);
        }
        bool ok = testService.QueueSize() == 0;

        delete testAuction;

        if (!ok)
        {
            return Fail("Bid queue cheapest-buyout cap", "bid queued at the price of a live buyout");
        }
        return Pass("Bid queue cheapest-buyout cap");
    }

    TestResult TestBidQueueOpeningBids(Bot& bot)
    {
        time_t future = GameTime::GetGameTime().count() + 100000;
        ObjectGuid const botGuid = bot.GetPlayer()->GetGUID();

        // A player's unbid auction at a clear bargain is eventually opened...
        AuctionEntry* playerAuction = MakeTestAuctionEntry(0xFFFFFFE8, future);
        playerAuction->itemCount = 1;
        playerAuction->startbid = 100;
        playerAuction->owner = ObjectGuid::Create<HighGuid::Player>(0x00F00008u);

        // ...the bot's own unbid listing never is.
        AuctionEntry* botAuction = MakeTestAuctionEntry(0xFFFFFFE9, future);
        botAuction->itemCount = 1;
        botAuction->startbid = 100;
        botAuction->owner = botGuid;

        AuctionBuyingService testService(bot);
        for (int i = 0; i < 100; i++)
        {
            testService.ConsiderForBid(botAuction, FlatLimits(1'000'000));
        }
        bool ownNeverOpened = testService.QueueSize() == 0;
        for (int i = 0; i < 100 && testService.QueueSize() == 0; i++)
        {
            testService.ConsiderForBid(playerAuction, FlatLimits(1'000'000));
        }
        bool playerOpened = testService.QueueSize() == 1;

        delete playerAuction;
        delete botAuction;

        if (!ownNeverOpened)
        {
            return Fail("Bid queue opening bids", "the bot opened bidding on its own listing");
        }
        if (!playerOpened)
        {
            return Fail("Bid queue opening bids", "a bargain unbid player auction was never opened");
        }
        return Pass("Bid queue opening bids");
    }

    TestResult TestBidQueueHardGate(Bot& bot)
    {
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE0, GameTime::GetGameTime().count() + 100000);
        testAuction->itemCount = 1;
        testAuction->bid = 1'000'000;  // next bid (+5%) is way over any sane market

        AuctionBuyingService testService(bot);
        testService.RollTolerance();
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(100));  // valuation far below the next bid
        }
        bool ok = testService.QueueSize() == 0;

        delete testAuction;

        if (!ok)
        {
            return Fail("Bid queue hard gate", "an above-market outbid was queued");
        }
        return Pass("Bid queue hard gate");
    }

    TestResult TestBidQueueRespectsBuyoutAndVendorCaps(Bot& bot)
    {
        // Next bid is 1000 + 50 = 1050, well under a 1'000'000 market, so only the
        // caps can stop it. 50 rolls each so the per-scan chance can't hide a miss.
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE3, GameTime::GetGameTime().count() + 100000);
        testAuction->itemCount = 1;
        testAuction->bid = 1000;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00004u);

        AuctionBuyingService testService(bot);
        testService.RollTolerance();

        testAuction->buyout = 1050;  // the next bid would reach the buyout
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(1'000'000));
        }
        bool buyoutCapped = testService.QueueSize() == 0;

        testAuction->buyout = 0;  // bid-only; the vendor sells it for less than the next bid
        for (int i = 0; i < 50; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(1'000'000, 1049));
        }
        bool vendorCapped = testService.QueueSize() == 0;

        for (int i = 0; i < 50 && testService.QueueSize() == 0; i++)
        {
            testService.ConsiderForBid(testAuction, FlatLimits(1'000'000, 1050));  // equal to vendor price is allowed
        }
        bool allowedAtVendor = testService.QueueSize() == 1;

        delete testAuction;

        if (!buyoutCapped)
        {
            return Fail("Bid queue respects caps", "a bid reaching the buyout was queued");
        }
        if (!vendorCapped)
        {
            return Fail("Bid queue respects caps", "a bid above the vendor price was queued");
        }
        if (!allowedAtVendor)
        {
            return Fail("Bid queue respects caps", "a bid at the vendor price was never queued");
        }
        return Pass("Bid queue respects caps");
    }

    TestResult TestBidQueueSharesBuyoutDedupe(Bot& bot)
    {
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE1, GameTime::GetGameTime().count() + 100000);
        testAuction->itemCount = 1;
        testAuction->bid = 10;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00001u);

        AuctionBuyingService testService(bot);
        testService.RollTolerance();
        testService.ConsiderForPurchase(testAuction, 1, 1'000'000, 2'000'000);  // always-buy
        testService.ConsiderForBid(testAuction, FlatLimits(1'000'000));        // must be a no-op
        bool ok = testService.QueueSize() == 1;

        delete testAuction;

        if (!ok)
        {
            return Fail(
                "Bid queue shares buyout dedupe",
                "an auction already queued for buyout was also queued for a bid");
        }
        return Pass("Bid queue shares buyout dedupe");
    }

    TestResult TestProcessDueQueueBidRevalidatesMissing(Bot& bot)
    {
        time_t now = GameTime::GetGameTime().count();
        AuctionEntry* testAuction = MakeTestAuctionEntry(0xFFFFFFE2, now + 100000);
        testAuction->houseId = AuctionHouseId::Alliance;  // a real map that does not hold this id
        testAuction->itemCount = 1;
        testAuction->bid = 10;
        testAuction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00002u);

        AuctionBuyingService testService(bot);
        testService.EnqueueBidForTest(testAuction, now - 1, 1'000'000);  // already due
        testService.ProcessDueQueue();  // PlaceBid re-fetches by id, finds nothing, no-ops
        bool ok = testService.QueueSize() == 0;

        delete testAuction;

        if (!ok)
        {
            return Fail("Bid queue revalidates missing auction", "queue was not drained");
        }
        return Pass("Bid queue revalidates missing auction");
    }

    TestResult TestDrainQueueRunsAllActions(Bot& bot)
    {
        time_t future = GameTime::GetGameTime().count() + 100000;
        AuctionEntry* a1 = MakeTestAuctionEntry(0xFFFFFFD0, future);
        AuctionEntry* a2 = MakeTestAuctionEntry(0xFFFFFFD1, future);
        AuctionEntry* a3 = MakeTestAuctionEntry(0xFFFFFFD2, future);
        for (AuctionEntry* a : {a1, a2, a3})
        {
            a->houseId = AuctionHouseId::Alliance;  // none are actually in the map -> executions no-op
            a->itemCount = 1;
        }
        a3->bid = 10;
        a3->bidder = ObjectGuid::Create<HighGuid::Player>(0x00F00003u);

        AuctionBuyingService testService(bot);
        testService.EnqueueForTest(a1, future);       // not due
        testService.EnqueueForTest(a2, future);       // not due
        testService.EnqueueBidForTest(a3, future, 1'000'000);

        testService.ProcessDueQueue();               // nothing due -> processes none
        bool noneDue = testService.QueueSize() == 3;

        size_t ran = testService.DrainQueue();       // forces all three through regardless of buyTime
        bool drained = ran == 3 && testService.QueueSize() == 0;

        delete a1;
        delete a2;
        delete a3;

        if (!noneDue)
        {
            return Fail("Drain queue runs all actions", "ProcessDueQueue ran a not-yet-due action");
        }
        if (!drained)
        {
            return Fail(
                "Drain queue runs all actions",
                Acore::StringFormat("DrainQueue ran {} of 3 and left {} queued", ran, testService.QueueSize()));
        }
        return Pass("Drain queue runs all actions");
    }

    ScannedItem const* FindListableCandidate(ASConfig const& config, AuctionHouseId houseId)
    {
        for (ScannedItem const& item : config.ScanData)
        {
            if (item.GetFactionNum() != static_cast<uint8>(houseId))
            {
                continue;
            }

            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.GetItemID());
            if (!proto)
            {
                continue;
            }

            if (!AuctionPricing::IsWithinLevelCap(
                    proto->RequiredLevel, proto->ItemLevel, config.maxRequiredLevel, config.maxItemLevel))
            {
                continue;
            }

            return &item;
        }
        return nullptr;
    }

    // A candidate with a resolvable item_template AND a non-zero RequiredLevel/ItemLevel --
    // used by the level-cap test, which needs real levels to set a meaningful cap against
    // (an item with RequiredLevel/ItemLevel 0 would make the "blocks listing" checks trivially
    // pass without exercising anything). Falls back to any resolvable candidate if the pool
    // has no such item, rather than failing the test over data this module doesn't control.
    ScannedItem const* FindAnyResolvableCandidate(ASConfig const& config, AuctionHouseId houseId)
    {
        ScannedItem const* fallback = nullptr;
        for (ScannedItem const& item : config.ScanData)
        {
            if (item.GetFactionNum() != static_cast<uint8>(houseId))
            {
                continue;
            }
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(item.GetItemID());
            if (!proto)
            {
                continue;
            }
            if (!fallback)
            {
                fallback = &item;
            }
            if (proto->RequiredLevel > 1 && proto->ItemLevel > 1)
            {
                return &item;
            }
        }
        return fallback;
    }

    void CleanUpTestAuction(AuctionEntry* auction, AuctionHouseId houseId)
    {
        auto trans = CharacterDatabase.BeginTransaction();
        auction->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(auction->item_guid, true, &trans);
        sAuctionMgr->GetAuctionsMapByHouseId(houseId)->RemoveAuction(auction);
        CharacterDatabase.CommitTransaction(trans);
    }
}

// --- Market mode -------------------------------------------------------------------
// Pure checks on an in-memory fixture market file: no DB, no auction house.
namespace
{
    // Builds the global POLICY fallback rows (every mb, offset 0, zero quantiles) for one faction.
    std::string FallbackPolicyRows(uint32 house, int32 skipMb = -2)
    {
        std::string rows;
        for (int32 mb = -1; mb <= Market::kMaxCheapestBin; ++mb)
        {
            if (mb != skipMb)
            {
                rows += Acore::StringFormat("{}:-1:-1:-1:-1:{}:0:0:0:0:0:0:0:0\n", house, mb);
            }
        }
        return rows;
    }

    // A small two-faction market. Faction 2 (Alliance): Linen Cloth (2589), Bolt of
    // Linen Cloth (2996, crafted from 2 Linen at margin 1.1) and Minor Healing Potion
    // (118, vendor price above its ref). Faction 6: Linen only. Quantiles are zero, so
    // prices and stacks are exact; offsets tell the POLICY fallback levels apart.
    std::string MarketFixture(std::string const& extraBasket = "", int32 skipMb = -2)
    {
        std::string policy = FallbackPolicyRows(2, skipMb) + FallbackPolicyRows(6) +
                             "2:3:7:2:1:5:0:0:0:0:0:0:0:0.1\n"
                             "2:3:7:-1:-1:5:0:0:0:0:0:0:0:0.2\n"
                             "2:3:-1:-1:-1:5:0:0:0:0:0:0:0:0.3\n";
        size_t policyRows = 0;
        for (char c : policy)
        {
            policyRows += c == '\n';
        }
        std::string basket = "2:0:2589:3:4.0:0.4:1.5\n"
                             "2:1:2996:3:1.0:0.0:1.0\n"
                             "2:2:118:3:2.0:1.0:1.0\n"
                             "6:0:2589:1:4.0:0.4:1.0\n" +
                             extraBasket;
        size_t basketRows = 0;
        for (char c : basket)
        {
            basketRows += c == '\n';
        }
        return "AUCTIONSIM_MARKET 1\n"
               "META 2\n"
               "2:1.5:0.8:60000:4000\n"
               "6:1.0:1.0:50000:3500\n"
               "CLASS 3\n"
               "0:Consumable\n"
               "7:Trade Goods\n"
               "-1:Other\n"
               "ITEM 4\n"
               "2:2589:7:120:20:20:13:3.5\n"
               "2:2996:7:100:1:5:40:0.5\n"
               "2:118:0:50:5:5:80:1.0\n"
               "6:2589:7:100:20:20:13:3.0\n"
               "CURVE 3\n"
               "2:-1:1:0.9:0.8:0.7:0.5:0.3:0.2:0.1:0.05\n"
               "2:7:1:1:1:1:0:0:0:0:0\n"
               "6:-1:1:0.9:0.8:0.7:0.5:0.3:0.2:0.1:0.05\n"
               "WEEKDAY 2\n"
               "2:-1:1:1:1:1:1:1:1\n"
               "2:7:0.5:1:1:1:1:1.5:1\n" +
               Acore::StringFormat("POLICY {}\n", policyRows) + policy +
               "STACK 3\n"
               "2:-1:-1:0:0:0:0:0:0:0\n"
               "6:-1:-1:0:0:0:0:0:0:0\n"
               "2:3:7:0.6931472:0.6931472:0.6931472:0.6931472:0.6931472:0.6931472:0.6931472\n"
               "FUTURE 2\n"
               "a section a newer exporter added\n"
               "and its second row\n"
               "CRAFT 1\n"
               "2:2996:2589:2:1.1\n"
               "BOT 4\n"
               "2:1:Bravo:3\n"
               "2:0:Alpha:3\n"
               "6:0:Gamma:1\n"
               "6:1:Delta:1\n" +
               Acore::StringFormat("BASKET {}\n", basketRows) + basket;
    }

    // Every fixture item exists, stacks to 20, with no level cap or vendor guard.
    Market::ItemFacts FixtureFacts(uint32 /*itemId*/)
    {
        Market::ItemFacts facts;
        facts.exists = true;
        facts.maxStack = 20;
        return facts;
    }

    bool ParseFixture(std::string const& text, Market::Data& data, std::string& error)
    {
        std::istringstream in(text);
        if (!data.Parse(in, error))
        {
            return false;
        }
        data.Resolve(FixtureFacts);
        return true;
    }

    TestResult TestMarketParse()
    {
        std::string const name = "Market file parse";
        Market::Data data;
        std::string error;
        if (!ParseFixture(MarketFixture(), data, error))
        {
            return Fail(name, Acore::StringFormat("fixture refused: {}", error));
        }
        Market::Faction const& alliance = data.factions[0];
        Market::Faction const& horde = data.factions[1];
        if (!alliance.present || !horde.present || alliance.demandScale != 1.5f || alliance.supplyScale != 0.8f)
        {
            return Fail(name, "META not read");
        }
        // 3 listed items + none extra (Linen is both an item and Bolt's reagent).
        if (alliance.items.size() != 3 || horde.items.size() != 1 || alliance.basket.size() != 3 ||
            horde.basket.size() != 1)
        {
            return Fail(name, Acore::StringFormat("items {}/{}, basket {}/{}", alliance.items.size(),
                horde.items.size(), alliance.basket.size(), horde.basket.size()));
        }
        if (alliance.bots.size() != 2 || alliance.bots[0].name != "Alpha" || alliance.bots[1].name != "Bravo")
        {
            return Fail(name, "BOT rows not sorted by index");
        }
        if (data.classNames.size() != 3 || data.classNames.at(7) != "Trade Goods")
        {
            return Fail(name, "CLASS rows not read");
        }
        Market::Item const& bolt = alliance.items[alliance.FindItem(2996)];
        if (bolt.craftEnd - bolt.craftBegin != 1 || alliance.craft[bolt.craftBegin].qty != 2.0f ||
            bolt.craftMargin != 1.1f || alliance.items[alliance.craft[bolt.craftBegin].itemIdx].itemId != 2589)
        {
            return Fail(name, "CRAFT not linked to the bolt");
        }
        if (data.stats.skippedRows != 0)
        {
            return Fail(name, Acore::StringFormat("{} rows skipped (unknown section rows count as known?)",
                data.stats.skippedRows));
        }
        return Pass(name, Acore::StringFormat("{} rows, unknown section skipped", data.stats.rows));
    }

    TestResult TestMarketFallbackLookups()
    {
        std::string const name = "Market fallback lookups";
        Market::Data data;
        std::string error;
        if (!ParseFixture(MarketFixture(), data, error))
        {
            return Fail(name, error);
        }
        Market::Faction const& fac = data.factions[0];
        struct Case
        {
            int32 type, itemClass, ub, sb, mb;
            float offset;
        };
        Case const cases[] = {
            {3, 7, 2, 1, 5, 0.1f},   // exact row
            {3, 7, 0, 0, 5, 0.2f},   // (type, class, -1, -1, mb)
            {3, 0, 2, 1, 5, 0.3f},   // (type, -1, -1, -1, mb)
            {9, 7, 2, 1, 5, 0.0f},   // global
            {3, 7, 2, 1, 4, 0.0f},   // no specific row for mb 4: global
            {3, 7, 2, 1, -1, 0.0f},  // nothing up: global mb -1
        };
        for (Case const& c : cases)
        {
            Market::PolicyRow const* row = fac.FindPolicy(c.type, c.itemClass, c.ub, c.sb, c.mb);
            if (!row || row->offset != c.offset)
            {
                return Fail(name, Acore::StringFormat("POLICY ({},{},{},{},{}) resolved to offset {}", c.type,
                    c.itemClass, c.ub, c.sb, c.mb, row ? row->offset : -99.0f));
            }
        }
        int32 specific = fac.FindStack(3, 7);
        int32 global = fac.FindStack(-1, -1);
        if (specific < 0 || global < 0 || specific == global || fac.FindStack(3, 0) != global ||
            fac.FindStack(5, 7) != global)
        {
            return Fail(name, "STACK fallback wrong");
        }
        Market::Item const& linen = fac.items[fac.FindItem(2589)];
        Market::Item const& potion = fac.items[fac.FindItem(118)];
        if (linen.curve == potion.curve || potion.curve < 0 || linen.weekday == potion.weekday ||
            fac.weekdays[linen.weekday][0] != 0.5f || fac.weekdays[potion.weekday][0] != 1.0f)
        {
            return Fail(name, "CURVE / WEEKDAY class fallback wrong");
        }
        return Pass(name);
    }

    TestResult TestMarketMalformedFiles()
    {
        std::string const name = "Market file refusals";
        struct Case
        {
            char const* what;
            std::string text;
            uint32 version;
        };
        std::string fixture = MarketFixture();
        std::string truncated = fixture.substr(0, fixture.rfind('\n', fixture.size() - 2) + 1);  // drop last row
        std::string duplicate = fixture + "CLASS 1\n9:Again\n";
        std::vector<Case> const cases = {
            {"empty file", "", 0},
            {"no stamp", "AUCTIONSIM_DAT 1\nMETA 0\n", 0},
            {"wrong schema", "AUCTIONSIM_MARKET 99\nMETA 0\n", 99},
            {"truncated section", truncated, 1},
            {"duplicate section", duplicate, 1},
            {"missing fallback POLICY", MarketFixture("", 3), 1},
            {"no META", "AUCTIONSIM_MARKET 1\nITEM 0\n", 1},
        };
        for (Case const& c : cases)
        {
            Market::Data data;
            std::string error;
            std::istringstream in(c.text);
            if (data.Parse(in, error))
            {
                return Fail(name, Acore::StringFormat("accepted a file with {}", c.what));
            }
            if (data.foundVersion != c.version || error.empty())
            {
                return Fail(name, Acore::StringFormat("{}: version {} / error '{}'", c.what, data.foundVersion, error));
            }
        }

        // One bad row is skipped, not fatal.
        Market::Data data;
        std::string error;
        if (!ParseFixture(MarketFixture("2:3:notanitem:3:1.0:0.5:1.0\n"), data, error) ||
            data.stats.skippedRows != 1 || data.factions[0].basket.size() != 3)
        {
            return Fail(name, Acore::StringFormat("a malformed BASKET row wasn't skipped cleanly ({})", error));
        }
        // A basket row for an item without an ITEM row is dropped.
        if (!ParseFixture(MarketFixture("2:3:9999:3:1.0:0.5:1.0\n"), data, error) || data.stats.droppedBasket != 1)
        {
            return Fail(name, "a BASKET row for an unknown item wasn't dropped");
        }
        // Resolve drops items the realm doesn't have, and their basket rows.
        std::istringstream in(MarketFixture());
        data.Parse(in, error);
        data.Resolve([](uint32 itemId) {
            Market::ItemFacts facts = FixtureFacts(itemId);
            facts.exists = itemId != 118;
            facts.postable = itemId != 2996;
            return facts;
        });
        if (data.factions[0].basket.size() != 1 || data.stats.droppedItems != 1)
        {
            return Fail(name, "Resolve kept a missing or unpostable item's basket rows");
        }
        return Pass(name, Acore::StringFormat("{} refusals, bad rows skipped", cases.size()));
    }

    TestResult TestMarketMissingFile(ASConfig const& config)
    {
        Market::Data data;
        std::string error;
        uint32 found = 7;
        if (MarketService::LoadFile("/nonexistent/auctionsim_market.dat", config, data, error, found) || found != 0 ||
            error.empty())
        {
            return Fail("Market missing file", "a missing file wasn't refused with version 0");
        }
        return Pass("Market missing file", error);
    }

    TestResult TestMarketBins()
    {
        std::string const name = "Market bin functions";
        struct UnitsCase
        {
            uint64 units;
            int32 bin;
        };
        // log1p: 1 -> 0.69, 2 -> 1.10, 6 -> 1.95, 7 -> 2.08, 1096 -> 7.0
        UnitsCase const unitsCases[] = {{0, 0}, {1, 0}, {2, 1}, {6, 1}, {7, 2}, {1095, 6}, {1096, 7}, {1000000, 7}};
        for (UnitsCase c : unitsCases)
        {
            if (Market::UnitsBin(c.units) != c.bin)
            {
                return Fail(name, Acore::StringFormat("UnitsBin({}) = {}, want {}", c.units,
                    Market::UnitsBin(c.units), c.bin));
            }
        }
        int32 const sellers[] = {0, 1, 2, 3, 3, 4, 4, 4, 4, 5, 5};
        for (uint32 s = 0; s < 11; ++s)
        {
            if (Market::SellersBin(s) != sellers[s])
            {
                return Fail(name, Acore::StringFormat("SellersBin({}) = {}", s, Market::SellersBin(s)));
            }
        }
        if (Market::SellersBin(1000) != 5)
        {
            return Fail(name, "SellersBin(1000) != 5");
        }
        struct CheapCase
        {
            double logRatio;
            int32 bin;
        };
        // Edges -1, -0.5, -0.25, -0.1, 0, 0.1, 0.25, 0.5, 1: below -1 -> 0, >= 1 -> 9.
        CheapCase const cheapCases[] = {{-2.0, 0}, {-1.001, 0}, {-0.999, 1}, {-0.3, 2}, {-0.2, 3}, {-0.05, 4},
            {0.0, 5}, {0.05, 5}, {0.2, 6}, {0.3, 7}, {0.7, 8}, {1.001, 9}, {3.0, 9}};
        for (CheapCase c : cheapCases)
        {
            double cheapest = 1000.0 * std::exp(c.logRatio);
            if (Market::CheapestBin(true, cheapest, 1000.0) != c.bin)
            {
                return Fail(name, Acore::StringFormat("CheapestBin(ln ratio {}) = {}, want {}", c.logRatio,
                    Market::CheapestBin(true, cheapest, 1000.0), c.bin));
            }
        }
        if (Market::CheapestBin(false, 10.0, 10.0) != -1)
        {
            return Fail(name, "CheapestBin without a buyout listing isn't -1");
        }
        return Pass(name);
    }

    TestResult TestMarketQuantileDraw()
    {
        Market::Quantiles q = {0, 1, 2, 3, 4, 5, 6};
        struct Case
        {
            double u, want;
        };
        Case const cases[] = {{0.0, 0}, {0.01, 0}, {0.02, 0}, {0.06, 0.5}, {0.175, 1.5}, {0.5, 3}, {0.825, 4.5},
            {0.98, 6}, {0.999, 6}};
        for (Case c : cases)
        {
            double got = Market::QuantileDraw(q, c.u);
            if (std::fabs(got - c.want) > 1e-9)
            {
                return Fail("Market quantile interpolation", Acore::StringFormat("u {} gave {}, want {}", c.u, got,
                    c.want));
            }
        }
        return Pass("Market quantile interpolation");
    }

    TestResult TestMarketReservationBounds()
    {
        std::string const name = "Market reservation sampling";
        Market::Rng rng(12345);
        Market::Curve full = Market::CumulativeCurve({1, 0.9f, 0.8f, 0.7f, 0.5f, 0.3f, 0.2f, 0.1f, 0.05f});
        for (int i = 0; i < 20000; ++i)
        {
            double r = Market::ReservationRatio(full, rng.Uniform(), rng.Uniform());
            if (r < 0.25 || r >= 5.0)
            {
                return Fail(name, Acore::StringFormat("ratio {} outside [0.25, 5)", r));
            }
        }
        // Everyone pays at least 0.85 x ref, nobody 1.0: all draws in [0.85, 1.0).
        Market::Curve narrow = Market::CumulativeCurve({1, 1, 1, 1, 0, 0, 0, 0, 0});
        for (int i = 0; i < 2000; ++i)
        {
            double r = Market::ReservationRatio(narrow, rng.Uniform(), rng.Uniform());
            if (r < 0.85 || r >= 1.0)
            {
                return Fail(name, Acore::StringFormat("narrow curve drew {}", r));
            }
        }
        // Half the buyers in bin 0, half in bin 1.
        Market::Curve half = Market::CumulativeCurve({1, 0.5f, 0, 0, 0, 0, 0, 0, 0});
        int lowBin = 0;
        int const draws = 20000;
        for (int i = 0; i < draws; ++i)
        {
            lowBin += Market::ReservationRatio(half, rng.Uniform(), rng.Uniform()) < 0.5;
        }
        if (std::abs(lowBin - draws / 2) > draws / 50)
        {
            return Fail(name, Acore::StringFormat("{} of {} draws in bin 0, want about half", lowBin, draws));
        }
        return Pass(name);
    }

    TestResult TestMarketScaleMath()
    {
        std::string const name = "Market scale math";
        Market::Data data;
        std::string error;
        if (!ParseFixture(MarketFixture(), data, error))
        {
            return Fail(name, error);
        }
        data.SetRates(0.1, 0.5);
        // rateH 4 x supplyScale 0.8 x scale 0.1 x dt 0.5
        Market::BasketRow const& linen = data.factions[0].basket[0];
        if (std::fabs(linen.lambda - 0.16f) > 1e-6f || std::fabs(linen.expNegLambda - std::exp(-0.16f)) > 1e-6f)
        {
            return Fail(name, Acore::StringFormat("posting rate {} for linen, want 0.16", linen.lambda));
        }

        Market::Rng rng(7);
        for (double lambda : {0.05, 2.0, 25.0, 80.0})
        {
            double sum = 0.0;
            int const n = 40000;
            for (int i = 0; i < n; ++i)
            {
                sum += rng.Poisson(lambda);
            }
            double mean = sum / n;
            if (std::fabs(mean - lambda) > 0.03 * lambda + 0.01)
            {
                return Fail(name, Acore::StringFormat("Poisson({}) mean {}", lambda, mean));
            }
        }

        // The market's volume doesn't depend on the bots in use: the same draws give the
        // same posts, only re-assigned to bot mod N.
        data.SetRates(50.0, 0.5);
        Market::Engine engine;
        engine.BuildState(data.factions[0].items.size());
        std::vector<Market::PostOrder> few, many;
        Market::Rng a(99), b(99);
        engine.PlanPosts(data.factions[0], 1, a, few);
        engine.PlanPosts(data.factions[0], 100, b, many);
        if (few.empty() || few.size() != many.size())
        {
            return Fail(name, Acore::StringFormat("{} posts with 1 bot, {} with 100", few.size(), many.size()));
        }
        for (size_t i = 0; i < few.size(); ++i)
        {
            if (few[i].botSlot != 0 || many[i].botSlot >= 100 || few[i].unitPrice != many[i].unitPrice)
            {
                return Fail(name, "bot mod N changed more than the poster");
            }
        }
        std::vector<Market::PostOrder> zero;
        engine.PlanPosts(data.factions[0], 0, a, zero);
        if (!zero.empty())
        {
            return Fail(name, "posted with no bots in use");
        }
        return Pass(name, Acore::StringFormat("{} posts at scale 50", few.size()));
    }

    TestResult TestMarketPricingAndBuyers()
    {
        std::string const name = "Market pricing and buyers";
        Market::Data data;
        std::string error;
        if (!ParseFixture(MarketFixture(), data, error))
        {
            return Fail(name, error);
        }
        Market::Faction const& fac = data.factions[0];
        uint32 const linen = static_cast<uint32>(fac.FindItem(2589));

        Market::Engine engine;
        auto add = [&engine](uint32 item, uint32 perUnit, uint32 count, uint32 owner, uint32 id, bool buyable) {
            Market::Listing listing;
            listing.itemIdx = item;
            listing.perUnit = perUnit;
            listing.count = count;
            listing.owner = owner;
            listing.auctionId = id;
            listing.flags = buyable ? Market::Listing::kBuyable : 0;
            engine.Listings().push_back(listing);
        };
        add(linen, 70, 20, 1, 101, true);
        add(linen, Market::Listing::kNoBuyout, 5, 2, 102, false);
        add(linen, 60, 10, 3, 103, false);  // e.g. vendor-guarded or already queued
        add(linen, 50, 20, 1, 104, true);
        engine.BuildState(fac.items.size());

        Market::ItemState const& state = engine.State(linen);
        if (state.cheapest != 50 || state.units != 55 || state.sellers != 3)
        {
            return Fail(name, Acore::StringFormat("linen state cheapest {} units {} sellers {}", state.cheapest,
                state.units, state.sellers));
        }

        Market::Rng rng(5);
        // Bolt: nothing up (mb -1) -> ref 100, but the craft floor is 1.1 x 2 x cheapest linen 50 = 110.
        uint32 boltPrice = engine.DrawUnitPrice(fac, fac.basket[1], rng);
        // Potion: ref 50 but its vendor price is 80.
        uint32 potionPrice = engine.DrawUnitPrice(fac, fac.basket[2], rng);
        // Linen: mb 5 (cheapest 50 = 0.42 x ref -> ln -0.87 -> bin 1), global offset 0 -> the cheapest.
        uint32 linenPrice = engine.DrawUnitPrice(fac, fac.basket[0], rng);
        if (boltPrice != 110 || potionPrice != 80 || linenPrice != 50)
        {
            return Fail(name, Acore::StringFormat("prices bolt {} potion {} linen {}, want 110 / 80 / 50",
                boltPrice, potionPrice, linenPrice));
        }
        // Bolt's (3, 7) STACK draws ln 2: conv 1 -> 2. Linen's conv 20 is its cap.
        uint32 boltCount = engine.DrawCount(fac, fac.basket[1], rng);
        uint32 linenCount = engine.DrawCount(fac, fac.basket[0], rng);
        if (boltCount != 2 || linenCount != 20)
        {
            return Fail(name, Acore::StringFormat("stacks bolt {} linen {}, want 2 / 20", boltCount, linenCount));
        }

        // Linen's class-7 CURVE puts every reservation in [0.85, 1.0) x 120 = [102, 120): a
        // flood of buyers takes 50 then 70 (60 isn't buyable), then finds only a bid-only
        // listing and leaves.
        std::vector<uint32> claims;
        uint32 buyers = engine.PlanBuys(fac, 100.0, 0, rng, claims);
        std::vector<uint32> ids;
        for (uint32 index : claims)
        {
            ids.push_back(engine.Listings()[index].auctionId);
        }
        if (buyers < 10 || ids != std::vector<uint32>{104, 101})
        {
            return Fail(name, Acore::StringFormat("{} buyers took {} listing(s)", buyers, ids.size()));
        }
        // Nobody's reservation reaches 3 x ref: an overpriced listing is never bought.
        engine.Listings().clear();
        add(linen, 400, 20, 1, 105, true);
        engine.BuildState(fac.items.size());
        claims.clear();
        engine.PlanBuys(fac, 100.0, 0, rng, claims);
        if (!claims.empty())
        {
            return Fail(name, "a buyer paid 3.3 x ref with a curve that stops at 1.0 x ref");
        }
        return Pass(name);
    }

    TestResult TestMarketBotNameSkipping()
    {
        std::vector<Market::BotName> rows;
        for (char const* botName : {"Alpha", "Bravo", "Charlie", "Dana", "Echo"})
        {
            rows.push_back({static_cast<uint32>(rows.size()), botName, 1});
        }
        auto owned = [](std::string const& n) -> uint32 { return n == "Bravo" ? 77 : 0; };
        auto available = [](std::string const& n) { return n != "Alpha" && n != "Charlie" && n != "Bravo"; };

        std::vector<Market::BotPick> two = Market::PickBots(rows, 2, owned, available);
        std::vector<Market::BotPick> all = Market::PickBots(rows, 10, owned, available);
        if (two.size() != 2 || two[0].name != "Bravo" || two[0].guid != 77 || two[1].name != "Dana" ||
            two[1].guid != 0)
        {
            return Fail("Market bot name skipping", "the first two usable names weren't Bravo (owned), Dana (new)");
        }
        if (all.size() != 3 || all[2].name != "Echo")
        {
            return Fail("Market bot name skipping", Acore::StringFormat("{} picks from 5 names with 2 taken",
                all.size()));
        }
        return Pass("Market bot name skipping");
    }

    // A Lordaeron-sized synthetic market: 10k items and 30k basket rows per faction, 200
    // bot names. Times the parse, then the per-step arithmetic on a house of `listings`
    // auctions at `scale`.
    TestResult TestMarketStepCost()
    {
        std::string const name = "Market step cost";
        constexpr uint32 kItems = 10000;
        constexpr uint32 kBasket = 30000;
        constexpr uint32 kBots = 200;

        std::string text = "AUCTIONSIM_MARKET 1\nMETA 2\n2:1:1:60000:4000\n6:1:1:60000:4000\n";
        std::string items, basket, bots, policy, curves;
        Market::Rng gen(2024);
        for (uint32 h : {2u, 6u})
        {
            for (uint32 i = 0; i < kItems; ++i)
            {
                items += Acore::StringFormat("{}:{}:{}:{}:{}:20:{}:{:.3f}\n", h, 1000 + i, i % 16,
                    100 + gen.Next() % 100000, 1 + i % 20, gen.Next() % 50, 0.02 + gen.Uniform() * 2.0);
            }
            for (uint32 r = 0; r < kBasket; ++r)
            {
                basket += Acore::StringFormat("{}:{}:{}:{}:{:.4f}:0.4:1.3\n", h, r % kBots, 1000 + gen.Next() % kItems,
                    r % 8, gen.Uniform() * 0.6);
            }
            for (uint32 b = 0; b < kBots; ++b)
            {
                bots += Acore::StringFormat("{}:{}:Bot{}{}:1\n", h, b, h == 2 ? "a" : "h", b);
            }
            policy += FallbackPolicyRows(h);
            for (int32 type = 0; type < 8; ++type)
            {
                for (int32 cls = 0; cls < 16; ++cls)
                {
                    for (int32 mb = -1; mb <= Market::kMaxCheapestBin; ++mb)
                    {
                        policy += Acore::StringFormat(
                            "{}:{}:{}:-1:-1:{}:-0.3:-0.2:-0.1:0:0.1:0.2:0.4:0\n", h, type, cls, mb);
                    }
                }
            }
            curves += Acore::StringFormat("{}:-1:1:0.95:0.9:0.8:0.6:0.4:0.2:0.1:0.05\n", h);
        }
        auto count = [](std::string const& s) { return std::count(s.begin(), s.end(), '\n'); };
        text += Acore::StringFormat("ITEM {}\n", count(items)) + items;
        text += Acore::StringFormat("CURVE {}\n", count(curves)) + curves;
        text += Acore::StringFormat("POLICY {}\n", count(policy)) + policy;
        text += "STACK 2\n2:-1:-1:-0.7:-0.3:0:0:0:0.3:0.7\n6:-1:-1:-0.7:-0.3:0:0:0:0.3:0.7\n";
        text += Acore::StringFormat("BOT {}\n", count(bots)) + bots;
        text += Acore::StringFormat("BASKET {}\n", count(basket)) + basket;

        Market::Data data;
        std::string error;
        auto parseStart = std::chrono::steady_clock::now();
        if (!ParseFixture(text, data, error))
        {
            return Fail(name, Acore::StringFormat("synthetic market refused: {}", error));
        }
        long long parseMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - parseStart).count();

        Market::Faction const& fac = data.factions[0];
        std::string detail = Acore::StringFormat("{} KB of text parsed in {} ms into ~{} KB", text.size() / 1024,
            parseMs, data.MemoryBytes() / 1024);

        struct Load
        {
            double scale;
            uint32 listings;
            int steps;
        };
        Market::Engine engine;
        std::vector<Market::Listing> house;
        std::vector<Market::PostOrder> orders;
        std::vector<uint32> claims;
        long long worstAvg = 0;
        Load const loads[] = {{0.1, 6000, 20}, {1.0, 60000, 5}};
        for (Load load : loads)
        {
            data.SetRates(load.scale, 0.5);
            house.clear();
            for (uint32 i = 0; i < load.listings; ++i)
            {
                Market::Listing listing;
                listing.itemIdx = static_cast<uint32>(gen.Next() % fac.items.size());
                listing.count = 1 + static_cast<uint32>(gen.Next() % 20);
                listing.perUnit = static_cast<uint32>(fac.items[listing.itemIdx].ref * (0.5 + gen.Uniform()));
                listing.owner = static_cast<uint32>(gen.Next() % 500);
                listing.auctionId = i;
                listing.flags = Market::Listing::kBuyable;
                house.push_back(listing);
            }

            uint64 posts = 0, buyers = 0, bought = 0;
            auto start = std::chrono::steady_clock::now();
            for (int step = 0; step < load.steps; ++step)
            {
                // What MarketService::StepHouse does, minus the core calls.
                engine.Listings().assign(house.begin(), house.end());
                engine.BuildState(fac.items.size());
                orders.clear();
                engine.PlanPosts(fac, 100, gen, orders);
                for (Market::PostOrder const& order : orders)
                {
                    Market::Listing listing;
                    listing.itemIdx = order.itemIdx;
                    listing.perUnit = order.unitPrice;
                    listing.count = order.count;
                    listing.flags = Market::Listing::kBuyable;
                    engine.Listings().push_back(listing);
                }
                engine.BuildState(fac.items.size());
                claims.clear();
                buyers += engine.PlanBuys(fac, load.scale * 0.5, static_cast<size_t>(step % 7), gen, claims);
                posts += orders.size();
                bought += claims.size();
            }
            long long avg = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count() / load.steps;
            worstAvg = std::max(worstAvg, avg);
            detail += Acore::StringFormat(
                "; scale {:g}, {} listings: {} us/step ({} posts, {} buyers, {} buys per step)",
                load.scale, load.listings, avg, posts / load.steps, buyers / load.steps, bought / load.steps);
        }

        // A sanity bound only (a Debug build is many times slower than -O2): the numbers in
        // the detail are the point. At -O2 Scale 0.1 is about 0.5 ms, Lordaeron scale ~7 ms.
        if (worstAvg > 1000000)
        {
            return Fail(name, detail);
        }
        return Pass(name, detail);
    }

    // A mid-sized synthetic market (one faction, Alliance) for the fill tests.
    std::string SyntheticMarket(uint32 items, uint32 basketRows, uint32 bots, uint64 seed)
    {
        Market::Rng gen(seed);
        std::string itemRows, basket, botRows;
        for (uint32 i = 0; i < items; ++i)
        {
            itemRows += Acore::StringFormat("2:{}:{}:{}:{}:20:{}:{:.3f}\n", 1000 + i, i % 8,
                100 + gen.Next() % 10000, 1 + i % 10, gen.Next() % 20, 0.05 + gen.Uniform() * 1.5);
        }
        for (uint32 r = 0; r < basketRows; ++r)
        {
            basket += Acore::StringFormat("2:{}:{}:{}:{:.4f}:0.4:1.3\n", r % bots, 1000 + gen.Next() % items, r % 4,
                0.05 + gen.Uniform() * 0.6);
        }
        for (uint32 b = 0; b < bots; ++b)
        {
            botRows += Acore::StringFormat("2:{}:Bot{}:1\n", b, b);
        }
        std::string policy = FallbackPolicyRows(2);
        auto count = [](std::string const& t) { return std::count(t.begin(), t.end(), '\n'); };
        return "AUCTIONSIM_MARKET 1\nMETA 1\n2:1:1:6000:400\n" + Acore::StringFormat("ITEM {}\n", count(itemRows)) +
               itemRows + "CURVE 1\n2:-1:1:0.95:0.9:0.8:0.6:0.4:0.2:0.1:0.05\n" +
               Acore::StringFormat("POLICY {}\n", count(policy)) + policy +
               "STACK 1\n2:-1:-1:-0.7:-0.3:0:0:0:0.3:0.7\n" + Acore::StringFormat("BOT {}\n", count(botRows)) +
               botRows + Acore::StringFormat("BASKET {}\n", count(basket)) + basket;
    }

    // The runtime from an empty house for `hours`, as the parity harness runs it; returns
    // the house at the end (bot listings, flagged as the sellers' own).
    std::vector<Market::Listing> LongRun(Market::Faction const& fac, uint32 bots, double scale, uint32 hours,
        uint64 endClock, Market::Rng& rng)
    {
        Market::Engine engine;
        std::vector<Market::Listing> live;
        std::vector<Market::PostOrder> orders;
        std::vector<uint32> claims;
        uint32 const steps = hours * 2;
        uint64 const startClock = endClock - uint64(hours) * 3600;
        uint32 nextId = 1;
        for (uint32 step = 0; step < steps; ++step)
        {
            uint64 const clock = startClock + uint64(step) * 1800;
            live.erase(std::remove_if(live.begin(), live.end(),
                           [clock](Market::Listing const& l) { return l.expire <= clock; }),
                live.end());
            engine.Listings().assign(live.begin(), live.end());
            engine.BuildState(fac.items.size());
            orders.clear();
            engine.PlanPosts(fac, bots, rng, orders);
            for (Market::PostOrder const& order : orders)
            {
                for (uint32 k = 0; k < order.listings; ++k)
                {
                    Market::Listing listing;
                    listing.itemIdx = order.itemIdx;
                    listing.perUnit = order.unitPrice;
                    listing.count = order.count;
                    listing.owner = order.botSlot;
                    listing.auctionId = nextId++;
                    listing.expire = static_cast<uint32>(clock + uint64(order.hours) * 3600);
                    listing.flags = Market::Listing::kBuyable | Market::Listing::kBotOwned;
                    engine.Listings().push_back(listing);
                }
            }
            engine.BuildState(fac.items.size());
            claims.clear();
            engine.PlanBuys(fac, scale * 0.5, 0, rng, claims);
            std::vector<char> gone(engine.Listings().size(), 0);
            for (uint32 index : claims)
            {
                gone[index] = 1;
            }
            live.clear();
            for (size_t i = 0; i < engine.Listings().size(); ++i)
            {
                if (!gone[i])
                {
                    live.push_back(engine.Listings()[i]);
                }
            }
        }
        live.erase(std::remove_if(live.begin(), live.end(),
                       [endClock](Market::Listing const& l) { return l.expire <= endClock; }),
            live.end());
        return live;
    }

    size_t ItemsUp(std::vector<Market::Listing> const& listings)
    {
        std::vector<uint32> items;
        for (Market::Listing const& listing : listings)
        {
            items.push_back(listing.itemIdx);
        }
        std::sort(items.begin(), items.end());
        return static_cast<size_t>(std::unique(items.begin(), items.end()) - items.begin());
    }

    // Fills `house` (fixed competitors) and returns what the fill would create.
    std::vector<Market::Listing> RunFill(Market::Faction const& fac, std::vector<Market::Listing> const& house,
        uint32 bots, double scale, uint64 endClock, Market::Rng& rng, long long* micros = nullptr)
    {
        std::vector<uint32> owners;
        for (uint32 b = 0; b < bots; ++b)
        {
            owners.push_back(b);
        }
        Market::FillRun fill;
        auto start = std::chrono::steady_clock::now();
        fill.Begin(fac, house, owners, std::vector<uint8>(96, 0), scale, 0.5, endClock);
        while (!fill.Advance(8, rng))
        {
        }
        std::vector<Market::Listing> out;
        fill.Result(out, rng);
        if (micros)
        {
            *micros = std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start).count();
        }
        return out;
    }

    // One fill check on a synthetic market of `items` items: from empty it should reach
    // the steady state of a 4-day run (count and items up within 15%), and on that full
    // house it should add at most 10%. Appends a summary to `detail`.
    bool CheckFill(uint32 items, uint64 seed, std::string& detail)
    {
        Market::Data data;
        std::string error;
        if (!ParseFixture(SyntheticMarket(items, 1500, 50, seed), data, error))
        {
            detail += Acore::StringFormat("synthetic market refused: {}", error);
            return false;
        }
        double const scale = 0.5;
        data.SetRates(scale, 0.5);
        Market::Faction const& fac = data.factions[0];
        uint64 const now = 1783296000ULL + 21 * 86400;
        Market::Rng rng(seed + 20);

        // Steady state: 4 days from empty (twice the longest duration).
        std::vector<Market::Listing> steady = LongRun(fac, 50, scale, 96, now, rng);
        long long micros = 0;
        std::vector<Market::Listing> fromEmpty = RunFill(fac, {}, 50, scale, now, rng, &micros);
        std::vector<Market::Listing> onFull = RunFill(fac, steady, 50, scale, now, rng);

        for (Market::Listing const& listing : fromEmpty)
        {
            if (listing.expire <= now || listing.expire > now + 48 * 3600)
            {
                detail += Acore::StringFormat("a survivor expires at {} (now {})", listing.expire, now);
                return false;
            }
        }

        double const steadyCount = static_cast<double>(steady.size());
        double const countRatio = static_cast<double>(fromEmpty.size()) / std::max(1.0, steadyCount);
        double const itemsRatio =
            static_cast<double>(ItemsUp(fromEmpty)) / std::max<double>(1.0, static_cast<double>(ItemsUp(steady)));
        double const topUp = static_cast<double>(onFull.size()) / std::max(1.0, steadyCount);
        detail += Acore::StringFormat(
            "{}{:.1f} listings/item: long run {} listings / {} items; fill from empty {} / {} ({:.0f}% / {:.0f}%) "
            "in {} us; on the full house adds {} ({:.0f}%)",
            detail.empty() ? "" : "; ",
            steadyCount / std::max<double>(1.0, static_cast<double>(ItemsUp(steady))),
            steady.size(),
            ItemsUp(steady),
            fromEmpty.size(),
            ItemsUp(fromEmpty),
            countRatio * 100.0,
            itemsRatio * 100.0,
            micros,
            onFull.size(),
            topUp * 100.0);
        return std::fabs(countRatio - 1.0) <= 0.15 && std::fabs(itemsRatio - 1.0) <= 0.15 && topUp <= 0.10;
    }

    TestResult TestMarketFill()
    {
        // Thick items (~20 listings each) and thin ones (~4, like the real file, where a
        // per-item subtraction alone added half a house to a full one). About 0.3 s at
        // -O2 on the world thread.
        auto start = std::chrono::steady_clock::now();
        std::string detail;
        bool ok = CheckFill(300, 11, detail);
        ok = CheckFill(4000, 12, detail) && ok;
        detail += Acore::StringFormat("; test {} ms",
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        return ok ? Pass("Market fill", detail) : Fail("Market fill", detail);
    }

    // Profession crafts the module lists carry the listing character as their maker, as a
    // player's would: only items a profession makes, and only those whose template takes
    // a signature (Saronite Bar is crafted but stacks, so it never shows a maker).
    TestResult TestCraftedItemSignature()
    {
        std::string const name = "Crafted item signature";
        CraftedItems::Load();
        uint32 const bag = 41599;     // Frostweave Bag: tailoring, stack of 1
        uint32 const bar = 36913;     // Saronite Bar: smelting, stacks of 20
        uint32 const stone = 6948;    // Hearthstone: no profession makes it
        if (!CraftedItems::IsCrafted(bag) || !CraftedItems::IsCrafted(bar) || CraftedItems::IsCrafted(stone))
        {
            return Fail(name, Acore::StringFormat("crafted? bag {} bar {} hearthstone {}", CraftedItems::IsCrafted(bag),
                                                  CraftedItems::IsCrafted(bar), CraftedItems::IsCrafted(stone)));
        }
        ObjectGuid const maker = ObjectGuid::Create<HighGuid::Player>(1);
        std::string problem;
        for (auto [itemId, signed_] : {std::pair{bag, true}, std::pair{bar, false}, std::pair{stone, false}})
        {
            Item* item = Item::CreateItem(itemId, 1, nullptr);
            if (!item)
            {
                problem = Acore::StringFormat("item {} could not be created", itemId);
                break;
            }
            CraftedItems::SignIfCrafted(item, maker);
            bool const hasMaker = item->GetGuidValue(ITEM_FIELD_CREATOR) == maker;
            delete item;
            if (hasMaker != signed_)
            {
                problem = Acore::StringFormat("item {} maker {} (expected {})", itemId, hasMaker, signed_);
                break;
            }
        }
        return problem.empty() ? Pass(name, "bag signed; bar and hearthstone not") : Fail(name, problem);
    }

    TestResult TestMailSwallowing()
    {
        std::string const name = "Mail to module characters";
        struct Case
        {
            bool toModule;
            MailMessageType type;
            bool swallow;
        };
        Case const cases[] = {
            {true, MAIL_AUCTION, true},      // sale proceeds, won / expired items, outbid refunds
            {true, MAIL_NORMAL, false},      // a player's or GM's mail: delivered, returns on expiry
            {true, MAIL_CREATURE, false},    // NPC / quest mail
            {true, MAIL_GAMEOBJECT, false},
            {true, MAIL_CALENDAR, false},
            {false, MAIL_AUCTION, false},    // a player's auction mail is never touched
            {false, MAIL_NORMAL, false},
        };
        for (Case const& c : cases)
        {
            if (AuctionSim::ShouldSwallowMail(c.toModule, MailSender(c.type, 1)) != c.swallow)
            {
                return Fail(name, Acore::StringFormat("type {} to {} character: swallow should be {}",
                    static_cast<uint32>(c.type), c.toModule ? "a module" : "a player", c.swallow));
            }
        }
        return Pass(name, "only auction-house mail to module characters is discarded");
    }

    TestResult TestMarketPurgePlan()
    {
        std::string const name = "Market purge selection";
        using Market::PurgeAccount;
        using Market::PurgeCharacter;
        auto seller = [](uint32 guid, uint32 account, std::string n, uint8 race, uint8 cls) {
            PurgeCharacter c;
            c.guid = guid;
            c.account = account;
            c.name = std::move(n);
            c.race = race;
            c.playerClass = cls;
            c.level = 1;
            c.atLogin = AT_LOGIN_FIRST;
            return c;
        };
        std::vector<PurgeAccount> accounts = {
            {10, "AHSIMMKTA01", false},
            {11, "AHSIMMKTH01", false},
            {12, "AHSIMMKTX", false},
            {13, "AHSIMMKTA1B", false}};
        std::vector<PurgeCharacter> chars = {
            seller(100, 10, "Alpha", RACE_HUMAN, CLASS_WARRIOR),
            seller(101, 10, "Bravo", RACE_DRAENEI, CLASS_WARRIOR),
            seller(200, 11, "Gamma", RACE_BLOODELF, CLASS_PALADIN),
            seller(300, 12, "Notours", RACE_HUMAN, CLASS_MAGE),
        };

        Market::PurgePlan plan = Market::PlanPurge(accounts, chars, 1, 5);
        if (!plan.problems.empty() || plan.accounts.size() != 2 || plan.characters.size() != 3)
        {
            return Fail(name, Acore::StringFormat("clean case: {} accounts, {} characters, {} problems",
                plan.accounts.size(), plan.characters.size(), plan.problems.size()));
        }
        for (PurgeCharacter const& c : plan.characters)
        {
            if (c.account == 12)
            {
                return Fail(name, "selected a character of an account that only shares the prefix");
            }
        }

        struct Case
        {
            char const* what;
            std::function<void(std::vector<PurgeAccount>&, std::vector<PurgeCharacter>&, uint32&, uint32&)> change;
        };
        std::vector<Case> const refusals = {
            {"a levelled character", [](auto&, auto& c, auto&, auto&) { c[0].level = 80; }},
            {"a played character", [](auto&, auto& c, auto&, auto&) { c[1].totalTime = 60; }},
            {"first login done", [](auto&, auto& c, auto&, auto&) { c[0].atLogin = 0; }},
            {"a Horde race on an Alliance account", [](auto&, auto& c, auto&, auto&) { c[0].race = RACE_ORC; }},
            {"an online seller", [](auto&, auto& c, auto&, auto&) { c[2].online = true; }},
            {"GM access", [](auto& a, auto&, auto&, auto&) { a[1].hasAccess = true; }},
            {"the buyer's account", [](auto&, auto&, auto& acc, auto&) { acc = 11; }},
            {"the buyer's character", [](auto&, auto&, auto&, auto& chr) { chr = 101; }},
        };
        for (Case const& c : refusals)
        {
            std::vector<PurgeAccount> a = accounts;
            std::vector<PurgeCharacter> ch = chars;
            uint32 buyerAccount = 1;
            uint32 buyerCharacter = 5;
            c.change(a, ch, buyerAccount, buyerCharacter);
            Market::PurgePlan refused = Market::PlanPurge(a, ch, buyerAccount, buyerCharacter);
            if (refused.problems.empty() || !refused.accounts.empty() || !refused.characters.empty())
            {
                return Fail(name, Acore::StringFormat("{} didn't refuse the whole purge", c.what));
            }
        }
        if (!Market::PlanPurge({}, {}, 1, 5).accounts.empty())
        {
            return Fail(name, "selected something with no accounts");
        }
        return Pass(name, Acore::StringFormat("{} refusal cases", refusals.size()));
    }

    TestResult TestMarketLoaded(Market::Data const& data, MarketService const* market)
    {
        std::string const name = "Market data loaded";
        std::string detail;
        for (size_t slot = 0; slot < Market::kFactions; ++slot)
        {
            Market::Faction const& fac = data.factions[slot];
            if (!fac.present)
            {
                continue;
            }
            if (fac.items.empty() || fac.basket.empty() || fac.bots.empty())
            {
                return Fail(name, Acore::StringFormat("faction {} has no items, basket or bots",
                    Market::FactionHouse(slot)));
            }
            detail += Acore::StringFormat(
                "{}faction {}: {} items, {} basket rows, {} names",
                detail.empty() ? "" : "; ",
                Market::FactionHouse(slot), fac.items.size(), fac.basket.size(), fac.bots.size());
            if (market && market->IsReady())
            {
                detail += Acore::StringFormat(", {} sellers", market->BotsInUse(slot));
            }
        }
        return Pass(name, detail);
    }

    TestResult TestMarketSellersOnRealm(MarketService const& market)
    {
        std::string const name = "Market sellers on the realm";
        if (!market.IsReady())
        {
            return Fail(name, Acore::StringFormat("not ready: {}", market.SetupNote()));
        }
        // Every seller is a real character the client can name, of its house's faction.
        size_t checked = 0;
        for (size_t slot = 0; slot < Market::kFactions; ++slot)
        {
            for (ObjectGuid guid : market.Slots(slot))
            {
                CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(guid);
                if (!entry || entry->Name.empty())
                {
                    return Fail(name, Acore::StringFormat("seller {} has no name", guid.ToString()));
                }
                TeamId want = slot == 0 ? TEAM_ALLIANCE : TEAM_HORDE;
                if (Player::TeamIdForRace(entry->Race) != want)
                {
                    return Fail(name, Acore::StringFormat("seller {} is on the wrong faction", entry->Name));
                }
                ++checked;
            }
        }
        return Pass(name, Acore::StringFormat("{} sellers named, factions right", checked));
    }
}

namespace AuctionSimTests
{
    std::vector<TestResult> RunLogicTests(Bot& bot, ASConfig const& config)
    {
        return {
            TestBotValid(bot),
            TestPriceDataLoaded(config),
            TestBothFactionsHavePriceData(config),
            TestListingMasksConfigured(config),
            TestFindScannedItemRoundTrip(config),
            TestRollStackSizeBounds(),
            TestIsListablePriceBoundary(),
            TestRollAuctionDurationBounds(),
            TestScannedItemParse(),
            TestDataVersionHeaderParse(),
            TestCategoryDepthParse(config),
            TestRollBuyoutPriceSanity(),
            TestRollStartBidBounds(),
            TestRollBuyToleranceBounds(),
            TestShouldBuyAtPriceBoundaries(),
            TestShouldBidAtPriceBoundaries(),
            TestRollBidValuationBounds(),
            TestRollBidAmountBounds(),
            TestBidTimingNoSniping(),
            TestRollBuyTimeBounds(),
            TestCalculateRemainingScans(),
            TestListingCountMath(),
            TestWeightedPick(),
            TestIsWithinLevelCapBoundary(),
            TestIsWithinVendorBuyPriceBoundary(),
            TestIsBuyableQuality(),
            TestBuyQueuePopulatesOnQualifyingPrice(bot),
            TestBuyQueueDedupesRescan(bot),
            TestBuyQueueNotYetDue(bot),
            TestBidQueueHardGate(bot),
            TestBidQueueSkipsLastMinutes(bot),
            TestBidValuationSticks(bot),
            TestBidQueueCheapestBuyoutCap(bot),
            TestBidQueueOpeningBids(bot),
            TestBidQueueRespectsBuyoutAndVendorCaps(bot),
            TestBidQueueSharesBuyoutDedupe(bot),
            TestProcessDueQueueBidRevalidatesMissing(bot),
            TestDrainQueueRunsAllActions(bot),
            TestReplayBiddingConfig(),
            TestBidQueueBiddingOff(bot),
        };
    }

    TestResult RunLiveListingTest(
        Bot& bot, ASConfig const& config, AuctionListingService& listingService, AuctionHouseId houseId)
    {
        char const* houseName = houseId == AuctionHouseId::Alliance ? "Alliance" : "Horde";
        std::string name = Acore::StringFormat("Live listing round-trip ({})", houseName);

        if (!bot.GetPlayer())
        {
            return Fail(name, "bot has no Player");
        }

        ScannedItem const* candidate = FindListableCandidate(config, houseId);
        if (!candidate)
        {
            return Fail(name, "no usable price data entry found for this house");
        }

        AuctionEntry* auction = listingService.ListTestItem(*candidate, houseId);
        if (!auction)
        {
            return Fail(name, Acore::StringFormat("ListTestItem returned null for item {}", candidate->GetItemID()));
        }

        bool foundInHouse = false;
        for (auto const& pair : sAuctionMgr->GetAuctionsMapByHouseId(houseId)->GetAuctions())
        {
            if (pair.second == auction)
            {
                foundInHouse = true;
                break;
            }
        }

        auto trans = CharacterDatabase.BeginTransaction();
        auction->DeleteFromDB(trans);
        sAuctionMgr->RemoveAItem(auction->item_guid);
        sAuctionMgr->GetAuctionsMapByHouseId(houseId)->RemoveAuction(auction);
        CharacterDatabase.CommitTransaction(trans);

        if (!foundInHouse)
        {
            return Fail(name, "auction was not found in the house's auction map immediately after listing");
        }
        return Pass(
            name,
            Acore::StringFormat("listed and cleaned up item {} (auction {})", candidate->GetItemID(), auction->Id));
    }

    TestResult RunLiveBuyingTest(
        Bot& bot, ASConfig const& config, AuctionListingService& listingService, AuctionHouseId houseId)
    {
        char const* houseName = houseId == AuctionHouseId::Alliance ? "Alliance" : "Horde";
        std::string name = Acore::StringFormat("Live buying round-trip ({})", houseName);

        if (!bot.GetPlayer())
        {
            return Fail(name, "bot has no Player");
        }

        ScannedItem const* candidate = FindListableCandidate(config, houseId);
        if (!candidate)
        {
            return Fail(name, "no usable price data entry found for this house");
        }

        AuctionEntry* auction = listingService.ListTestItem(*candidate, houseId);
        if (!auction)
        {
            return Fail(name, Acore::StringFormat("ListTestItem returned null for item {}", candidate->GetItemID()));
        }
        uint32 auctionId = auction->Id;

        // Throwaway service so this never touches the real bot's live buy queue.
        AuctionBuyingService testService(bot);
        testService.EnqueueForTest(auction, GameTime::GetGameTime().count() - 1);  // already due
        testService.ProcessDueQueue();

        if (testService.QueueSize() != 0)
        {
            return Fail(name, "queue was not drained after processing a due purchase");
        }

        // auction is dangling past this point (BuyItem's RemoveAuction deletes it) --
        // check by id, never by pointer.
        for (auto const& pair : sAuctionMgr->GetAuctionsMapByHouseId(houseId)->GetAuctions())
        {
            if (pair.first == auctionId)
            {
                return Fail(name, "auction still present in the house's auction map after being bought");
            }
        }

        return Pass(
            name,
            Acore::StringFormat("bought and removed item {} (auction {})", candidate->GetItemID(), auctionId));
    }

    TestResult RunLiveBiddingTest(
        Bot& bot, ASConfig const& config, AuctionListingService& listingService, AuctionHouseId houseId)
    {
        char const* houseName = houseId == AuctionHouseId::Alliance ? "Alliance" : "Horde";
        std::string name = Acore::StringFormat("Live bidding round-trip ({})", houseName);

        if (!bot.GetPlayer())
        {
            return Fail(name, "bot has no Player");
        }

        ScannedItem const* candidate = FindListableCandidate(config, houseId);
        if (!candidate)
        {
            return Fail(name, "no usable price data entry found for this house");
        }

        AuctionEntry* auction = listingService.ListTestItem(*candidate, houseId);
        if (!auction)
        {
            return Fail(name, Acore::StringFormat("ListTestItem returned null for item {}", candidate->GetItemID()));
        }
        uint32 auctionId = auction->Id;

        // Simulate a real player already holding the high bid at the starting bid.
        // The guid resolves to no character, so SendAuctionOutbiddedMail is a no-op.
        uint32 playerBid = auction->startbid;
        auction->bid = playerBid;
        auction->bidder = ObjectGuid::Create<HighGuid::Player>(0x00FB1D00u);
        uint32 expectedBid = playerBid + AuctionEntry::CalculateAuctionOutBid(playerBid);
        // The rolled starting bid can sit within one increment of the buyout, where the
        // bot rightly refuses to bid; make it bid-only (in memory) so the cap can't trip.
        auction->buyout = 0;

        // Throwaway service; a huge ceiling so the walk-away guard never trips.
        AuctionBuyingService testService(bot);
        testService.EnqueueBidForTest(auction, GameTime::GetGameTime().count() - 1, 0xFFFFFFFu);
        testService.ProcessDueQueue();

        if (testService.QueueSize() != 0)
        {
            return Fail(name, "queue was not drained after processing a due bid");
        }

        // The auction must still be live (a bid does not consume it).
        AuctionEntry* live = sAuctionMgr->GetAuctionsMapByHouseId(houseId)->GetAuction(auctionId);
        if (!live)
        {
            return Fail(name, "auction was removed from the house after a bid (should stay live)");
        }
        if (live->bidder != bot.GetPlayer()->GetGUID())
        {
            return Fail(name, "bot did not become the high bidder");
        }
        // The bot may round its bid up the way a player types it; never below the minimum.
        if (live->bid < expectedBid || live->bid >= expectedBid + 3 * AuctionPricing::BidRoundingStep(expectedBid))
        {
            return Fail(
                name,
                Acore::StringFormat("bid is {} (expected startbid+outbid = {}, maybe rounded up)", live->bid,
                    expectedBid));
        }

        uint32 placedBid = live->bid;  // CleanUpTestAuction frees `live`
        CleanUpTestAuction(live, houseId);

        return Pass(
            name,
            Acore::StringFormat(
                "outbid player on item {} (auction {}): {} -> {}",
                candidate->GetItemID(), auctionId, playerBid, placedBid));
    }

    TestResult RunLiveLevelCapTest(
        Bot& bot, ASConfig& config, AuctionListingService& listingService, AuctionHouseId houseId)
    {
        char const* houseName = houseId == AuctionHouseId::Alliance ? "Alliance" : "Horde";
        std::string name = Acore::StringFormat("Level cap enforcement ({})", houseName);

        if (!bot.GetPlayer())
        {
            return Fail(name, "bot has no Player");
        }

        ScannedItem const* candidate = FindAnyResolvableCandidate(config, houseId);
        if (!candidate)
        {
            return Fail(name, "no price data entry with a resolvable item_template found for this house");
        }

        ItemTemplate const* proto = sObjectMgr->GetItemTemplate(candidate->GetItemID());
        uint32 savedMaxRequiredLevel = config.maxRequiredLevel;
        uint32 savedMaxItemLevel = config.maxItemLevel;

        // A cap strictly below the candidate's required level must block the listing,
        // with the item-level check disabled so only the required-level check is exercised.
        // RequiredLevel must be > 1 here: a cap of 0 means "disabled" per IsWithinLevelCap's
        // semantics, not "cap of zero", so RequiredLevel - 1 == 0 would set an ineffective
        // cap and fail the test for the wrong reason.
        bool requiredLevelCapBlocksListing = true;
        if (proto->RequiredLevel > 1)
        {
            config.maxRequiredLevel = proto->RequiredLevel - 1;
            config.maxItemLevel = 0;
            AuctionEntry* blocked = listingService.ListTestItem(*candidate, houseId);
            requiredLevelCapBlocksListing = blocked == nullptr;
            if (blocked)
            {
                CleanUpTestAuction(blocked, houseId);
            }
        }

        // Same, but exercising the item-level check with the required-level check disabled.
        bool itemLevelCapBlocksListing = true;
        if (proto->ItemLevel > 1)
        {
            config.maxRequiredLevel = 0;
            config.maxItemLevel = proto->ItemLevel - 1;
            AuctionEntry* blocked = listingService.ListTestItem(*candidate, houseId);
            itemLevelCapBlocksListing = blocked == nullptr;
            if (blocked)
            {
                CleanUpTestAuction(blocked, houseId);
            }
        }

        // Disabled caps must allow the same candidate through.
        config.maxRequiredLevel = 0;
        config.maxItemLevel = 0;
        AuctionEntry* allowed = listingService.ListTestItem(*candidate, houseId);

        config.maxRequiredLevel = savedMaxRequiredLevel;
        config.maxItemLevel = savedMaxItemLevel;

        if (allowed)
        {
            CleanUpTestAuction(allowed, houseId);
        }

        if (!requiredLevelCapBlocksListing)
        {
            return Fail(name, "a cap below the item's required level did not block listing");
        }
        if (!itemLevelCapBlocksListing)
        {
            return Fail(name, "a cap below the item's item level did not block listing");
        }
        if (!allowed)
        {
            return Fail(name, "the item was not listed once both caps were disabled");
        }

        return Pass(
            name,
            Acore::StringFormat(
                "item {} (required {}, ilvl {}) correctly blocked above cap and allowed when disabled",
                candidate->GetItemID(),
                proto->RequiredLevel,
                proto->ItemLevel));
    }

    std::vector<TestResult> RunMarketTests(
        ASConfig const& config, Market::Data const* loaded, MarketService const* market)
    {
        std::vector<TestResult> results = {
            TestMarketParse(),
            TestMarketFallbackLookups(),
            TestMarketMalformedFiles(),
            TestMarketMissingFile(config),
            TestMarketBins(),
            TestMarketQuantileDraw(),
            TestMarketReservationBounds(),
            TestMarketScaleMath(),
            TestMarketPricingAndBuyers(),
            TestMarketBotNameSkipping(),
            TestMarketStepCost(),
            TestMarketFill(),
            TestMarketPurgePlan(),
            TestMailSwallowing(),
            TestCraftedItemSignature(),
        };
        if (loaded)
        {
            results.push_back(TestMarketLoaded(*loaded, market));
        }
        if (market)
        {
            results.push_back(TestMarketSellersOnRealm(*market));
        }
        return results;
    }
}
