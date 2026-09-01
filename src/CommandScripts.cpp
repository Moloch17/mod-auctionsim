#include <chrono>
#include <utility>
#include "AuctionSim.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "GameTime.h"
#include "Log.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

namespace
{
    // Shared "is the module usable?" gate for every subcommand. Replies to the GM
    // and returns false when it isn't.
    bool RequireEnabled(ChatHandler* handler)
    {
        if (AuctionSim::instance() && AuctionSim::instance()->isEnabled)
        {
            return true;
        }
        handler->SendSysMessage("AuctionSim module is disabled.");
        return false;
    }

    // Runs `fn` and returns how long it took, in whole milliseconds.
    template <typename Fn>
    long long TimedMs(Fn&& fn)
    {
        auto start = std::chrono::steady_clock::now();
        std::forward<Fn>(fn)();
        auto end = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    }
}

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
        if (!RequireEnabled(handler))
        {
            return true;
        }

        size_t queueSizeBefore = AuctionSim::instance()->GetBuyQueue().size();
        long long elapsed = TimedMs([] {
            AuctionSim::instance()->ScanAuctions(AuctionHouseId::Alliance);
            AuctionSim::instance()->ScanAuctions(AuctionHouseId::Horde);
        });
        size_t queueSizeAfter = AuctionSim::instance()->GetBuyQueue().size();

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
        if (!RequireEnabled(handler))
        {
            return true;
        }

        long long elapsed = TimedMs([] { AuctionSim::instance()->DeleteAuctions(); });
        handler->SendSysMessage(fmt::format("Auction delete completed in {} ms", elapsed));
        return true;
    }

    static bool HandleTestCommand(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
        {
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
        if (!RequireEnabled(handler))
        {
            return true;
        }

        uint32 removedCount = 0;
        long long elapsed = TimedMs([&] { removedCount = AuctionSim::instance()->CleanOverCapAuctions(); });
        handler->SendSysMessage(
            fmt::format("Removed {} over-cap auction(s) in {} ms", removedCount, elapsed));
        return true;
    }

    static bool HandleShowQueueCommand(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
        {
            return true;
        }

        AuctionSim::BuyQueueStatus status =
            AuctionSim::instance()->GetBuyQueueStatus(GameTime::GetGameTime().count());

        std::string message = status.size == 0
            ? std::string("AuctionSim buy queue is empty.")
            : fmt::format(
                  "AuctionSim buy queue: {} item(s) | next buy in {}s | last buy in {}s",
                  status.size,
                  status.nextBuyInSeconds,
                  status.lastBuyInSeconds);
        LOG_INFO("module", "{}", message);
        handler->SendSysMessage(message);
        return true;
    }
};

void AddSC_AuctionCommandScript() { new AuctionSimCommandScript(); }
