#include "AuctionSimAddonBridge.h"
#include <chrono>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "ASConfig.h"
#include "ASConfigWriter.h"
#include "ASParse.h"
#include "AuctionHouseMgr.h"
#include "AuctionSim.h"
#include "CharacterCache.h"
#include "Common.h"
#include "Config.h"
#include "GameTime.h"
#include "ObjectGuid.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Tokenize.h"
#include "WorldSession.h"

namespace
{
    // The wire is "MSGTYPE\tfield\tfield...". The send path formats "{}\t{}" with the
    // bare prefix; the receive path matches prefix + '\t' so a lone "AHSIM" can't
    // be mistaken for a command. interface_addon/ahsim/AHSim.lua mirrors AHSim.OP.
    constexpr std::string_view kPrefix = "AHSIM";
    constexpr std::string_view kFullPrefixWithTab = "AHSIM\t";

    namespace Msg
    {
        // Inbound: client -> server command verbs.
        constexpr std::string_view WhoAmI = "WHOAMI";
        constexpr std::string_view GetConfig = "GETCONFIG";
        constexpr std::string_view SetConfig = "SETCONFIG";
        constexpr std::string_view SaveConfig = "SAVECONFIG";
        constexpr std::string_view Scan = "SCAN";
        constexpr std::string_view Delete = "DELETE";
        constexpr std::string_view Test = "TEST";
        constexpr std::string_view CleanOverCap = "CLEANOVERCAP";
        constexpr std::string_view ShowQueue = "SHOWQUEUE";
        constexpr std::string_view SetBotChar = "SETBOTCHAR";

        // Outbound: server -> client message types.
        constexpr std::string_view Error = "ERROR";
        constexpr std::string_view Config = "CONFIG";
        constexpr std::string_view ConfigSaved = "CONFIGSAVED";
        constexpr std::string_view ScanResult = "SCANRESULT";
        constexpr std::string_view DeleteResult = "DELETERESULT";
        constexpr std::string_view TestResult = "TESTRESULT";
        constexpr std::string_view TestDone = "TESTDONE";
        constexpr std::string_view QueueInfo = "QUEUEINFO";
        constexpr std::string_view CleanResult = "CLEANRESULT";
        constexpr std::string_view SetBotCharResult = "SETBOTCHARRESULT";
    }

    // SETCONFIG keys awaiting a SAVECONFIG. One global set: worldserver hooks are
    // single-threaded and there is one bot / one GM editing at a time.
    std::unordered_set<std::string> stagedKeys;

    void SendMessage(Player* target, std::string const& body)
    {
        if (!target)
        {
            return;
        }
        // Self-whisper as LANG_ADDON: reaches the client's CHAT_MSG_ADDON with no
        // live bot needed, so the window works while the module is disabled.
        target->Whisper(Acore::StringFormat("{}\t{}", kPrefix, body), LANG_ADDON, target);
    }

    void SendError(Player* target, std::string const& message)
    {
        SendMessage(target, Acore::StringFormat("{}\t{}", Msg::Error, message));
    }

    // Same SEC_ADMINISTRATOR bar as the ".auctionsim" chat commands. A non-GM gets
    // no reply at all, so their client never builds the window.
    bool IsAuthorizedGm(Player* player)
    {
        return player && player->GetSession() && player->GetSession()->GetSecurity() >= SEC_ADMINISTRATOR;
    }

    bool RequireEnabled(Player* target)
    {
        AuctionSim* sim = AuctionSim::instance();
        if (!sim || !sim->isEnabled)
        {
            SendError(target, "AuctionSim module is disabled.");
            return false;
        }
        // Enabled can be set before a bot exists; the action commands need one.
        if (!sim->GetBotPlayer())
        {
            SendError(target, "AuctionSim is enabled but the bot character isn't set up yet -- use Set Bot Char.");
            return false;
        }
        return true;
    }

    std::string GetConfFilePath() { return sConfigMgr->GetConfigPath() + "/modules/auctionsim.conf"; }

    // Not gated on RequireEnabled: the panel reads/edits config while disabled too.
    // config loads at startup, so a null here means auctionsim.dat failed to parse.
    void HandleGetConfig(Player* target, std::vector<std::string_view> const&)
    {
        ASConfig* config = AuctionSim::instance()->GetConfig();
        if (!config)
        {
            SendError(target, "config not loaded");
            return;
        }

        AuctionSim* sim = AuctionSim::instance();
        SendMessage(target, Acore::StringFormat("{}\tEnabled\t{}", Msg::Config, sim->isEnabled ? 1 : 0));
        SendMessage(target, Acore::StringFormat("{}\tStartupScan\t{}", Msg::Config, sim->startupScan ? 1 : 0));
        SendMessage(target, Acore::StringFormat("{}\tMaxRequiredLevel\t{}", Msg::Config, config->maxRequiredLevel));
        SendMessage(target, Acore::StringFormat("{}\tMaxItemLevel\t{}", Msg::Config, config->maxItemLevel));

        for (ASConfig::MaskKeyEntry const& entry : ASConfig::AllMaskKeys())
        {
            float multiplier = config->ItemSelectionMask[entry.itemClass][entry.quality];
            SendMessage(
                target,
                Acore::StringFormat(
                    "{}\t{}.{}\t{:g}", Msg::Config, entry.percentConfigKey, entry.qualityLabel, multiplier));
        }
    }

    // Not gated on RequireEnabled -- must stay reachable to re-enable the module.
    void HandleSetConfig(Player* target, std::vector<std::string_view> const& tokens)
    {
        if (tokens.size() < 3)
        {
            SendError(target, "SETCONFIG requires a key and a value");
            return;
        }

        std::string key(tokens[1]);
        std::string_view valueStr = tokens[2];

        if (key == "Enabled")
        {
            AuctionSim* sim = AuctionSim::instance();
            sim->isEnabled = (valueStr == "1");
            stagedKeys.insert(key);
            // Cold start: bring the bot up now. Fine if no bot char yet -- Set Bot
            // Char starts it.
            if (sim->isEnabled && !sim->GetBotPlayer() && !sim->StartOrReloadBot())
            {
                SendError(
                    target,
                    "Enabled saved, but the bot can't run until a valid bot character is set (use Set Bot Char).");
            }
            return;
        }
        if (key == "StartupScan")
        {
            AuctionSim::instance()->startupScan = (valueStr == "1");
            stagedKeys.insert(key);
            return;
        }

        ASConfig* config = AuctionSim::instance()->GetConfig();
        if (!config)
        {
            SendError(target, "config not loaded");
            return;
        }

        if (key == "MaxRequiredLevel" || key == "MaxItemLevel")
        {
            uint32 numericValue = 0;
            if (!ASParse::Integer(valueStr, numericValue))
            {
                SendError(target, Acore::StringFormat("'{}' is not a valid number", valueStr));
                return;
            }
            if (key == "MaxRequiredLevel")
            {
                config->maxRequiredLevel = numericValue;
            }
            else
            {
                config->maxItemLevel = numericValue;
            }
            stagedKeys.insert(key);
            return;
        }

        uint32 itemClass = 0;
        uint32 quality = 0;
        if (ASConfig::ResolveMaskKey(key, itemClass, quality))
        {
            float multiplier = 0.0f;
            if (!ASParse::Float(valueStr, multiplier) || multiplier < 0.0f)
            {
                SendError(target, Acore::StringFormat("'{}' is not a valid multiplier", valueStr));
                return;
            }
            config->ItemSelectionMask[itemClass][quality] = multiplier;
            stagedKeys.insert(key);
            return;
        }

        SendError(target, Acore::StringFormat("unrecognized config key '{}'", key));
    }

    // Not gated on RequireEnabled: a save that persists Enabled=0 must still work.
    void HandleSaveConfig(Player* target, std::vector<std::string_view> const&)
    {
        ASConfig* config = AuctionSim::instance()->GetConfig();
        if (!config)
        {
            SendError(target, "config not loaded");
            return;
        }

        AuctionSim* sim = AuctionSim::instance();

        // Collect every staged change, then persist them in one read + one write.
        std::vector<ASConfigWriter::Edit> edits;
        edits.reserve(stagedKeys.size());
        for (std::string const& key : stagedKeys)
        {
            if (key == "Enabled")
            {
                edits.push_back({key, "", sim->isEnabled ? "1" : "0"});
            }
            else if (key == "StartupScan")
            {
                edits.push_back({key, "", sim->startupScan ? "1" : "0"});
            }
            else if (key == "MaxRequiredLevel")
            {
                edits.push_back({key, "", Acore::StringFormat("{}", config->maxRequiredLevel)});
            }
            else if (key == "MaxItemLevel")
            {
                edits.push_back({key, "", Acore::StringFormat("{}", config->maxItemLevel)});
            }
            else
            {
                uint32 itemClass = 0;
                uint32 quality = 0;
                if (ASConfig::ResolveMaskKey(key, itemClass, quality))
                {
                    auto dotPos = key.find('.');
                    edits.push_back(
                        {key.substr(0, dotPos),
                         key.substr(dotPos + 1),
                         Acore::StringFormat("{:g}", config->ItemSelectionMask[itemClass][quality])});
                }
            }
        }

        bool allOk = ASConfigWriter::SetMany(GetConfFilePath(), edits);

        stagedKeys.clear();
        SendMessage(
            target,
            Acore::StringFormat(
                "{}\t{}\t{}",
                Msg::ConfigSaved,
                allOk ? "ok" : "error",
                allOk ? "saved" : "one or more values failed to save, check the server log"));
    }

    void HandleScan(Player* target, std::vector<std::string_view> const&)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        size_t before = AuctionSim::instance()->GetBuyQueue().size();

        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Alliance);
        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Horde);

        size_t after = AuctionSim::instance()->GetBuyQueue().size();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        SendMessage(target, Acore::StringFormat("{}\t{}\t{}\t{}", Msg::ScanResult, elapsed, after - before, after));
    }

    void HandleDelete(Player* target, std::vector<std::string_view> const&)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        AuctionSim::instance()->DeleteAuctions();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        SendMessage(target, Acore::StringFormat("{}\t{}", Msg::DeleteResult, elapsed));
    }

    void HandleTest(Player* target, std::vector<std::string_view> const&)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto results = AuctionSim::instance()->RunTests();
        uint32 passed = 0;
        for (size_t i = 0; i < results.size(); i++)
        {
            if (results[i].passed)
            {
                passed++;
            }
            SendMessage(
                target,
                Acore::StringFormat(
                    "{}\t{}\t{}\t{}\t{}\t{}",
                    Msg::TestResult,
                    i + 1,
                    results.size(),
                    results[i].passed ? "pass" : "fail",
                    results[i].name,
                    results[i].detail));
        }
        SendMessage(target, Acore::StringFormat("{}\t{}\t{}", Msg::TestDone, passed, results.size()));
    }

    void HandleCleanOverCap(Player* target, std::vector<std::string_view> const&)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint32 removed = AuctionSim::instance()->CleanOverCapAuctions();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        SendMessage(target, Acore::StringFormat("{}\t{}\t{}", Msg::CleanResult, removed, elapsed));
    }

    void HandleShowQueue(Player* target, std::vector<std::string_view> const&)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        AuctionSim::BuyQueueStatus status =
            AuctionSim::instance()->GetBuyQueueStatus(GameTime::GetGameTime().count());

        SendMessage(
            target,
            Acore::StringFormat(
                "{}\t{}\t{}\t{}",
                Msg::QueueInfo,
                status.size,
                status.nextBuyInSeconds,
                status.lastBuyInSeconds));
    }

    // Resolve a character name to its guid + account id (only the server can) and
    // write both to auctionsim.conf. If the module is enabled, restart the bot on it.
    void HandleSetBotChar(Player* target, std::vector<std::string_view> const& tokens)
    {
        if (tokens.size() < 2 || tokens[1].empty())
        {
            SendMessage(target, Acore::StringFormat("{}\tfail\tno character name given", Msg::SetBotCharResult));
            return;
        }

        std::string name(tokens[1]);
        if (!normalizePlayerName(name))
        {
            SendMessage(
                target,
                Acore::StringFormat(
                    "{}\tfail\t'{}' is not a valid character name", Msg::SetBotCharResult, name));
            return;
        }

        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByName(name);
        if (!entry)
        {
            SendMessage(
                target,
                Acore::StringFormat("{}\tfail\tno character named '{}' exists", Msg::SetBotCharResult, name));
            return;
        }

        // no "not your own character" guard: setup is often done while logged in as the bot
        uint32 characterId = entry->Guid.GetCounter();
        uint32 accountId = entry->AccountId;

        bool wrote = ASConfigWriter::SetMany(
            GetConfFilePath(),
            {{"BotCharacterID", "", Acore::StringFormat("{}", characterId)},
             {"BotAccountID", "", Acore::StringFormat("{}", accountId)}});
        if (!wrote)
        {
            SendMessage(
                target,
                Acore::StringFormat(
                    "{}\tfail\tfound the character but couldn't write auctionsim.conf, check the server log",
                    Msg::SetBotCharResult));
            return;
        }

        // enabled -> swap the bot live; disabled -> it waits for enable
        AuctionSim* sim = AuctionSim::instance();
        std::string note;
        if (!sim->isEnabled)
        {
            note = "Saved. The bot will start on this character when you enable the module.";
        }
        else if (sim->StartOrReloadBot())
        {
            note = "Bot reloaded; no restart needed.";
        }
        else
        {
            note = "Written to auctionsim.conf, but the live reload failed -- restart to apply.";
        }

        SendMessage(
            target,
            Acore::StringFormat(
                "{}\tok\t{}\t{}\t{}\t{}", Msg::SetBotCharResult, name, characterId, accountId, note));
    }

    // First request on load. A reply (GM-only) is the client's cue to build the window.
    void HandleWhoAmI(Player* target, std::vector<std::string_view> const&)
    {
        SendMessage(target, Acore::StringFormat("{}\tok", Msg::WhoAmI));
    }

    using CommandHandler = void (*)(Player*, std::vector<std::string_view> const&);

    struct CommandRoute
    {
        std::string_view verb;
        CommandHandler handler;
    };

    CommandRoute const kCommandRoutes[] = {
        {Msg::WhoAmI, HandleWhoAmI},
        {Msg::GetConfig, HandleGetConfig},
        {Msg::SetConfig, HandleSetConfig},
        {Msg::SaveConfig, HandleSaveConfig},
        {Msg::Scan, HandleScan},
        {Msg::Delete, HandleDelete},
        {Msg::Test, HandleTest},
        {Msg::CleanOverCap, HandleCleanOverCap},
        {Msg::ShowQueue, HandleShowQueue},
        {Msg::SetBotChar, HandleSetBotChar},
    };

    void HandleRequest(Player* player, std::string const& payload)
    {
        if (!IsAuthorizedGm(player))
        {
            return;  // no reply; non-GM clients never show the window
        }

        std::vector<std::string_view> tokens = Acore::Tokenize(payload, '\t', true);
        if (tokens.empty())
        {
            return;
        }

        for (CommandRoute const& route : kCommandRoutes)
        {
            if (route.verb == tokens[0])
            {
                route.handler(player, tokens);
                return;
            }
        }
        SendError(player, "unknown command");
    }
}

void AuctionSimAddonBridge::OnPlayerBeforeSendChatMessage(
    Player* player, uint32& /*type*/, uint32& lang, std::string& msg)
{
    if (lang != LANG_ADDON)
    {
        return;
    }

    std::string_view view = msg;
    if (view.substr(0, kFullPrefixWithTab.size()) != kFullPrefixWithTab)
    {
        return;
    }

    std::string payload(view.substr(kFullPrefixWithTab.size()));
    msg.clear();  // swallow -- never let this reach the client as a visible whisper

    HandleRequest(player, payload);
}

void AddAuctionSimAddonBridgeScript() { new AuctionSimAddonBridge(); }
