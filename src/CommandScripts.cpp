#include <algorithm>
#include <cctype>
#include <chrono>
#include <string>
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
        static ChatCommandTable marketSubCommandTable = {
            {"status", HandleMarketStatusCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"reload", HandleMarketReloadCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"fill", HandleMarketFillCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"purge", HandleMarketPurgeCommand, SEC_ADMINISTRATOR, Console::Yes},
        };
        static ChatCommandTable auctionSimSubCommandTable = {
            {"market", marketSubCommandTable},
            {"scan", HandleScanAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"delete", HandleDeleteAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"test", HandleTestCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"cleanovercap", HandleCleanOverCapCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"showqueue", HandleShowQueueCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"runqueue", HandleRunQueueCommand, SEC_ADMINISTRATOR, Console::Yes},
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
        long long elapsed = TimedMs([] { AuctionSim::instance()->RunScan(); });
        size_t queueSizeAfter = AuctionSim::instance()->GetBuyQueue().size();

        std::string message = fmt::format(
            "Auction {} completed in {} ms. Added {} item(s) to buy queue ({} total).",
            AuctionSim::instance()->IsMarketMode() ? "market step" : "scan",
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

    static bool HandleMarketStatusCommand(ChatHandler* handler)
    {
        AuctionSim* sim = AuctionSim::instance();
        if (!sim || !sim->IsMarketMode())
        {
            handler->SendSysMessage("AuctionSim is in Replay mode (AuctionSim.Mode = Replay).");
            return true;
        }
        if (sim->IsMarketUnavailable())
        {
            handler->SendSysMessage(fmt::format(
                "Market mode can't run: auctionsim_market.dat {} (file schema v{}, needs v{}).",
                sim->MarketError(),
                sim->MarketHaveVersion(),
                sim->MarketNeedVersion()));
            return true;
        }
        MarketService* market = sim->GetMarket();
        if (!market)
        {
            handler->SendSysMessage(sim->IsMarketPurged()
                    ? "Market mode: stopped by a purge until restart or \".auctionsim market reload\"."
                    : "Market mode: data loaded, market not running (module disabled?).");
            return true;
        }
        if (!market->IsReady())
        {
            handler->SendSysMessage(fmt::format("Market mode: setting up sellers -- {}", market->SetupNote()));
            return true;
        }
        handler->SendSysMessage(fmt::format(
            "Market mode: scale {:g}, {} Alliance / {} Horde sellers, {} listing(s) waiting to post, buy queue {}.",
            market->GetScale(),
            market->BotsInUse(0),
            market->BotsInUse(1),
            market->PendingListings(),
            sim->GetBuyQueue().size()));
        for (size_t faction = 0; faction < Market::kFactions; ++faction)
        {
            MarketService::HouseStats const& stats = market->LastStats(faction);
            handler->SendSysMessage(fmt::format(
                "  last step, {}: {} listings seen, {} post events, {} created ({} carried), {} buyers, {} buys "
                "queued, {} us",
                faction == 0 ? "Alliance" : "Horde",
                stats.listingsSeen,
                stats.postEvents,
                stats.listingsCreated,
                stats.listingsCarried,
                stats.buyers,
                stats.buysQueued,
                stats.micros));
            MarketService::FillStatus fill = market->GetFillStatus(faction);
            if (fill.running)
            {
                handler->SendSysMessage(
                    fmt::format("  fill running: {}/{} simulated steps", fill.stepsDone, fill.stepsTotal));
            }
            else if (fill.stepsTotal > 0)
            {
                handler->SendSysMessage(fmt::format(
                    "  last fill: {} listings queued ({} us of simulation)", fill.result, fill.micros));
            }
        }
        return true;
    }

    static bool HandleMarketFillCommand(ChatHandler* handler, Optional<std::string> which)
    {
        AuctionSim* sim = AuctionSim::instance();
        if (!sim)
        {
            return true;
        }
        std::string arg = which ? *which : std::string();
        std::transform(arg.begin(), arg.end(), arg.begin(), [](unsigned char c) { return std::tolower(c); });
        bool ok = false;
        for (std::string const& line : sim->FillMarket(arg, ok))
        {
            handler->SendSysMessage(line);
        }
        return true;
    }

    static bool HandleMarketPurgeCommand(ChatHandler* handler, Optional<std::string> confirm)
    {
        AuctionSim* sim = AuctionSim::instance();
        if (!sim)
        {
            return true;
        }
        bool const doIt = confirm && *confirm == "confirm";
        AuctionSim::PurgeReport report = sim->PurgeMarket(doIt);
        for (std::string const& line : AuctionSim::DescribePurge(report, doIt, sim->IsMarketMode()))
        {
            LOG_INFO("module", "AuctionSim: {}", line);
            handler->SendSysMessage(line);
        }
        return true;
    }

    static bool HandleMarketReloadCommand(ChatHandler* handler)
    {
        AuctionSim* sim = AuctionSim::instance();
        if (!sim)
        {
            return true;
        }
        std::string note;
        bool ok = false;
        long long elapsed = TimedMs([&] { ok = sim->ReloadMarket(note); });
        std::string message =
            fmt::format("Market reload {} in {} ms: {}", ok ? "done" : "failed", elapsed, note);
        LOG_INFO("module", "AuctionSim: {}", message);
        handler->SendSysMessage(message);
        return true;
    }

    static bool HandleRunQueueCommand(ChatHandler* handler)
    {
        if (!RequireEnabled(handler))
        {
            return true;
        }

        size_t ran = 0;
        long long elapsed = TimedMs([&] { ran = AuctionSim::instance()->RunQueue(); });

        std::string message = fmt::format("Ran {} queued action(s) in {} ms", ran, elapsed);
        LOG_INFO("module", "{}", message);
        handler->SendSysMessage(message);
        return true;
    }
};

void AddSC_AuctionCommandScript() { new AuctionSimCommandScript(); }
