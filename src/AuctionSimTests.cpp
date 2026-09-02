#include "AuctionSimTests.h"
#include "ASConfig.h"
#include "AuctionBuyingService.h"
#include "AuctionListingService.h"
#include "AuctionPricing.h"
#include "Bot.h"
#include "GameTime.h"
#include "ObjectMgr.h"
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
        // Old shorter formats are rejected: rows are a fixed 41 fields now.
        if (ScannedItem::TryParse("6:6543:-19:23229:8000:99000"))
        {
            return Fail("ScannedItem parse", "6-field (old format) line was accepted");
        }
        if (ScannedItem::TryParse(
                "6:6543:-19:12:8000:99000:23229:30000:8000:19000:30843:8100:31686:23000:29500:8000"))
        {
            return Fail("ScannedItem parse", "16-field (old gear format) line was accepted");
        }

        // 4 identity + 12 price + 12 stack + 12 listing-count + listingSnapshotCount.
        //   price : adjLow 8100  adjHigh 31686  adjMedian 29500  q3 30843
        //   stack : adjMode 20   adjLow 2       adjHigh 20
        //   list  : adjMedian 3                 snapshotCount 47
        auto parsed = ScannedItem::TryParse(
            "6:6543:-19:12"
            ":8000:99000:23229:30000:8000:19000:30843:8100:31686:23000:29500:8000"
            ":2:20:14:20:20:5:20:2:20:14:20:20"
            ":1:12:4:3:2:2:6:1:9:4:3:2"
            ":47");
        if (!parsed)
        {
            return Fail("ScannedItem parse", "valid 41-field line was rejected");
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
        return Pass("ScannedItem parse");
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

        // Plenty of time left -- delay must be capped at 45 minutes and never past expiry.
        time_t farBuyTime = AuctionPricing::RollBuyTime(now + 100000, now);
        if (farBuyTime < now || farBuyTime > now + 2700)
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
            TestCategoryDepthParse(config),
            TestRollBuyoutPriceSanity(),
            TestRollBuyToleranceBounds(),
            TestShouldBuyAtPriceBoundaries(),
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
}
