#include <chrono>
#include "AuctionSim.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "GameTime.h"
#include "Log.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

class AuctionSimCommandScript : public CommandScript
{
public:
    AuctionSimCommandScript() : CommandScript("AuctionSimCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable auctionSimSubCommandTable = {
            {"scan", HandleScanAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"delete", HandleDeleteAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"test", HandleTestCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"cleanovercap", HandleCleanOverCapCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"showqueue", HandleShowQueueCommand, SEC_ADMINISTRATOR, Console::Yes},
        };
        static ChatCommandTable commandTable = {
            {"auctionsim", auctionSimSubCommandTable},
        };
        return commandTable;
    }

    static bool HandleScanAuctionsCommand(ChatHandler* handler)
    {
        if (!AuctionSim::instance()->isEnabled)
        {
            handler->SendSysMessage("AuctionSim module is disabled.");
            return true;
        }

        auto start = std::chrono::high_resolution_clock::now();

        size_t queueSizeBefore = AuctionSim::instance()->GetBuyQueue().size();

        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Alliance);
        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Horde);

        size_t queueSizeAfter = AuctionSim::instance()->GetBuyQueue().size();

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        std::string message = fmt::format(
            "Auction scan completed in {} ms. Added {} item(s) to buy queue ({} total).",
            elapsed,
            queueSizeAfter - queueSizeBefore,
            queueSizeAfter);
        LOG_INFO("module", "{}", message);
        handler->SendSysMessage(message);
        return true;
    }

    static bool HandleDeleteAuctionsCommand(ChatHandler* handler)
    {
        if (!AuctionSim::instance()->isEnabled)
        {
            handler->SendSysMessage("AuctionSim module is disabled.");
            return true;
        }

        auto start = std::chrono::high_resolution_clock::now();

        AuctionSim::instance()->DeleteAuctions();

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        handler->SendSysMessage(fmt::format("Auction delete completed in {} ms", elapsed));
        return true;
    }

    static bool HandleTestCommand(ChatHandler* handler)
    {
        if (!AuctionSim::instance()->isEnabled)
        {
            handler->SendSysMessage("AuctionSim module is disabled.");
            return true;
        }

        auto results = AuctionSim::instance()->RunTests();

        uint32 passed = 0;
        for (auto const& result : results)
        {
            if (result.passed)
            {
                passed++;
                LOG_INFO("module", "AuctionSim test [PASS] {}: {}", result.name, result.detail);
                handler->SendSysMessage(fmt::format("[PASS] {}: {}", result.name, result.detail));
            }
            else
            {
                LOG_ERROR("module", "AuctionSim test [FAIL] {}: {}", result.name, result.detail);
                handler->SendSysMessage(fmt::format("[FAIL] {}: {}", result.name, result.detail));
            }
        }

        std::string summary = fmt::format("AuctionSim test suite: {}/{} passed", passed, results.size());
        LOG_INFO("module", "{}", summary);
        handler->SendSysMessage(summary);
        return true;
    }

    static bool HandleCleanOverCapCommand(ChatHandler* handler)
    {
        if (!AuctionSim::instance()->isEnabled)
        {
            handler->SendSysMessage("AuctionSim module is disabled.");
            return true;
        }

        auto start = std::chrono::high_resolution_clock::now();

        uint32 removedCount = AuctionSim::instance()->CleanOverCapAuctions();

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        handler->SendSysMessage(
            fmt::format("Removed {} over-cap auction(s) in {} ms", removedCount, elapsed));
        return true;
    }

    static bool HandleShowQueueCommand(ChatHandler* handler)
    {
        if (!AuctionSim::instance()->isEnabled)
        {
            handler->SendSysMessage("AuctionSim module is disabled.");
            return true;
        }

        auto const& queue = AuctionSim::instance()->GetBuyQueue();

        if (queue.empty())
        {
            std::string message = "AuctionSim buy queue is empty.";
            LOG_INFO("module", "{}", message);
            handler->SendSysMessage(message);
            return true;
        }

        // Sorted descending by buyTime (see AuctionBuyingService::SortQueue): soonest-due
        // purchase is at the back, furthest-due is at the front.
        time_t now = GameTime::GetGameTime().count();
        time_t nextBuyIn = queue.back().buyTime - now;
        time_t lastBuyIn = queue.front().buyTime - now;

        std::string message = fmt::format(
            "AuctionSim buy queue: {} item(s) | next buy in {}s | last buy in {}s",
            queue.size(),
            nextBuyIn,
            lastBuyIn);
        LOG_INFO("module", "{}", message);
        handler->SendSysMessage(message);
        return true;
    }
};

void AddSC_AuctionCommandScript() { new AuctionSimCommandScript(); }