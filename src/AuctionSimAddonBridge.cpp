#include "AuctionSimAddonBridge.h"
#include <charconv>
#include <chrono>
#include <string_view>
#include <unordered_set>
#include <vector>
#include "ASConfig.h"
#include "ASConfigWriter.h"
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
    constexpr std::string_view kPrefix = "AHSIM";
    constexpr std::string_view kFullPrefixWithTab = "AHSIM\t";

    // SETCONFIG keys awaiting a SAVECONFIG. One global set: one bot, one GM at a time.
    std::unordered_set<std::string> stagedKeys;

    bool ParseUInt32(std::string_view text, uint32& out)
    {
        auto result = std::from_chars(text.data(), text.data() + text.size(), out);
        return result.ec == std::errc() && result.ptr == text.data() + text.size();
    }

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

    void SendError(Player* target, std::string const& message) { SendMessage(target, "ERROR\t" + message); }

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
    void HandleGetConfig(Player* target)
    {
        ASConfig* config = AuctionSim::instance()->GetConfig();
        if (!config)
        {
            SendError(target, "config not loaded");
            return;
        }

        AuctionSim* sim = AuctionSim::instance();
        SendMessage(target, Acore::StringFormat("CONFIG\tEnabled\t{}", sim->isEnabled ? 1 : 0));
        SendMessage(target, Acore::StringFormat("CONFIG\tStartupScan\t{}", sim->startupScan ? 1 : 0));
        SendMessage(target, Acore::StringFormat("CONFIG\tMaxRequiredLevel\t{}", config->maxRequiredLevel));
        SendMessage(target, Acore::StringFormat("CONFIG\tMaxItemLevel\t{}", config->maxItemLevel));

        for (ASConfig::MaskKeyEntry const& entry : ASConfig::AllMaskKeys())
        {
            float mask = config->ItemSelectionMask[entry.itemClass][entry.quality];
            uint32 percentValue = static_cast<uint32>(mask * 100.0f + 0.5f);
            std::string body =
                Acore::StringFormat("CONFIG\t{}.{}\t{}", entry.percentConfigKey, entry.qualityLabel, percentValue);
            SendMessage(target, body);
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

        uint32 numericValue = 0;
        if (!ParseUInt32(valueStr, numericValue))
        {
            SendError(target, Acore::StringFormat("'{}' is not a valid number", valueStr));
            return;
        }

        if (key == "MaxRequiredLevel")
        {
            config->maxRequiredLevel = numericValue;
            stagedKeys.insert(key);
            return;
        }
        if (key == "MaxItemLevel")
        {
            config->maxItemLevel = numericValue;
            stagedKeys.insert(key);
            return;
        }

        uint32 itemClass = 0;
        uint32 quality = 0;
        if (ASConfig::ResolveMaskKey(key, itemClass, quality))
        {
            config->ItemSelectionMask[itemClass][quality] = numericValue / 100.0f;
            stagedKeys.insert(key);
            return;
        }

        SendError(target, Acore::StringFormat("unrecognized config key '{}'", key));
    }

    // Not gated on RequireEnabled: a save that persists Enabled=0 must still work.
    void HandleSaveConfig(Player* target)
    {
        ASConfig* config = AuctionSim::instance()->GetConfig();
        if (!config)
        {
            SendError(target, "config not loaded");
            return;
        }

        std::string filepath = GetConfFilePath();
        bool allOk = true;

        for (std::string const& key : stagedKeys)
        {
            bool ok = false;
            if (key == "Enabled")
            {
                ok = ASConfigWriter::SetScalarValue(filepath, key, AuctionSim::instance()->isEnabled ? "1" : "0");
            }
            else if (key == "StartupScan")
            {
                ok = ASConfigWriter::SetScalarValue(filepath, key, AuctionSim::instance()->startupScan ? "1" : "0");
            }
            else if (key == "MaxRequiredLevel")
            {
                ok = ASConfigWriter::SetScalarValue(
                    filepath, key, Acore::StringFormat("{}", config->maxRequiredLevel));
            }
            else if (key == "MaxItemLevel")
            {
                ok = ASConfigWriter::SetScalarValue(filepath, key, Acore::StringFormat("{}", config->maxItemLevel));
            }
            else
            {
                uint32 itemClass = 0;
                uint32 quality = 0;
                if (ASConfig::ResolveMaskKey(key, itemClass, quality))
                {
                    uint32 percentValue =
                        static_cast<uint32>(config->ItemSelectionMask[itemClass][quality] * 100.0f + 0.5f);
                    auto dotPos = key.find('.');
                    ok = ASConfigWriter::SetMaskValue(
                        filepath, key.substr(0, dotPos), key.substr(dotPos + 1), percentValue);
                }
            }
            allOk = allOk && ok;
        }

        stagedKeys.clear();
        SendMessage(
            target,
            Acore::StringFormat(
                "CONFIGSAVED\t{}\t{}",
                allOk ? "ok" : "error",
                allOk ? "saved" : "one or more values failed to save, check the server log"));
    }

    void HandleScan(Player* target)
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

        SendMessage(target, Acore::StringFormat("SCANRESULT\t{}\t{}\t{}", elapsed, after - before, after));
    }

    void HandleDelete(Player* target)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        AuctionSim::instance()->DeleteAuctions();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        SendMessage(target, Acore::StringFormat("DELETERESULT\t{}", elapsed));
    }

    void HandleTest(Player* target)
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
                    "TESTRESULT\t{}\t{}\t{}\t{}\t{}",
                    i + 1,
                    results.size(),
                    results[i].passed ? "pass" : "fail",
                    results[i].name,
                    results[i].detail));
        }
        SendMessage(target, Acore::StringFormat("TESTDONE\t{}\t{}", passed, results.size()));
    }

    void HandleCleanOverCap(Player* target)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto start = std::chrono::high_resolution_clock::now();
        uint32 removed = AuctionSim::instance()->CleanOverCapAuctions();
        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

        SendMessage(target, Acore::StringFormat("CLEANRESULT\t{}\t{}", removed, elapsed));
    }

    void HandleShowQueue(Player* target)
    {
        if (!RequireEnabled(target))
        {
            return;
        }

        auto const& queue = AuctionSim::instance()->GetBuyQueue();
        if (queue.empty())
        {
            SendMessage(target, "QUEUEINFO\t0\t0\t0");
            return;
        }

        // queue is sorted by buyTime descending: soonest-due at the back
        time_t now = GameTime::GetGameTime().count();
        time_t nextBuyIn = queue.back().buyTime - now;
        time_t lastBuyIn = queue.front().buyTime - now;

        SendMessage(target, Acore::StringFormat("QUEUEINFO\t{}\t{}\t{}", queue.size(), nextBuyIn, lastBuyIn));
    }

    // Resolve a character name to its guid + account id (only the server can) and
    // write both to auctionsim.conf. If the module is enabled, restart the bot on it.
    void HandleSetBotChar(Player* target, std::vector<std::string_view> const& tokens)
    {
        if (tokens.size() < 2 || tokens[1].empty())
        {
            SendMessage(target, "SETBOTCHARRESULT\tfail\tno character name given");
            return;
        }

        std::string name(tokens[1]);
        if (!normalizePlayerName(name))
        {
            SendMessage(
                target, Acore::StringFormat("SETBOTCHARRESULT\tfail\t'{}' is not a valid character name", name));
            return;
        }

        CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByName(name);
        if (!entry)
        {
            SendMessage(
                target, Acore::StringFormat("SETBOTCHARRESULT\tfail\tno character named '{}' exists", name));
            return;
        }

        // no "not your own character" guard: setup is often done while logged in as the bot
        uint32 characterId = entry->Guid.GetCounter();
        uint32 accountId = entry->AccountId;

        std::string filepath = GetConfFilePath();
        bool wroteChar =
            ASConfigWriter::SetScalarValue(filepath, "BotCharacterID", Acore::StringFormat("{}", characterId));
        bool wroteAccount =
            ASConfigWriter::SetScalarValue(filepath, "BotAccountID", Acore::StringFormat("{}", accountId));
        if (!wroteChar || !wroteAccount)
        {
            SendMessage(
                target,
                "SETBOTCHARRESULT\tfail\tfound the character but couldn't write auctionsim.conf, check the server log");
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
                "SETBOTCHARRESULT\tok\t{}\t{}\t{}\t{}", name, characterId, accountId, note));
    }

    // First request on load. A reply (GM-only) is the client's cue to build the window.
    void HandleWhoAmI(Player* target) { SendMessage(target, "WHOAMI\tok"); }

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

        std::string_view command = tokens[0];
        if (command == "WHOAMI")
        {
            HandleWhoAmI(player);
        }
        else if (command == "GETCONFIG")
        {
            HandleGetConfig(player);
        }
        else if (command == "SETCONFIG")
        {
            HandleSetConfig(player, tokens);
        }
        else if (command == "SAVECONFIG")
        {
            HandleSaveConfig(player);
        }
        else if (command == "SCAN")
        {
            HandleScan(player);
        }
        else if (command == "DELETE")
        {
            HandleDelete(player);
        }
        else if (command == "TEST")
        {
            HandleTest(player);
        }
        else if (command == "CLEANOVERCAP")
        {
            HandleCleanOverCap(player);
        }
        else if (command == "SHOWQUEUE")
        {
            HandleShowQueue(player);
        }
        else if (command == "SETBOTCHAR")
        {
            HandleSetBotChar(player, tokens);
        }
        else
        {
            SendError(player, "unknown command");
        }
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
