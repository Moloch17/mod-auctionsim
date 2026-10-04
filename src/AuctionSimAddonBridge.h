#pragma once
#include <string>
#include "ScriptMgr.h"

class Player;

// Server side of the companion "Bot Manager" client addon (interface_addon/ahsim).
// Client -> server: the addon sends "AHSIM\t<request>" as a LANG_ADDON whisper to
// itself; OnPlayerBeforeSendChatMessage is the only chat hook that fires for whispers
// (confirmed against ChatHandler::HandleMessagechatOpcode -- the whisper case calls
// Player::Whisper unconditionally with no cancelable hook), so this intercepts there
// and clears the message so nothing visible reaches the player.
// Server -> client: replies are sent as "AHSIM\t<response>" LANG_ADDON whispers from
// the bot's own Player, which the addon's CHAT_MSG_ADDON handler reads.
class AuctionSimAddonBridge : public PlayerScript
{
public:
    // Scoped to just the hooks this class needs (PlayerScript::PlayerScript falls back
    // to enabling every hook when the list is left empty, so this is a minor precision/
    // efficiency choice, not something required for the hooks to fire).
    AuctionSimAddonBridge()
        : PlayerScript(
              "AuctionSimAddonBridge", {PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE, PLAYERHOOK_ON_LOGIN})
    {
    }

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override;

    // GM-only plain-text warnings for an outdated config / data file, at every login
    // while the condition holds. Reaches GMs whether or not the addon is installed;
    // module<->addon version mismatch is reported separately via the WHOAMI reply.
    void OnPlayerLogin(Player* player) override;
};

void AddAuctionSimAddonBridgeScript();
