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
    // Scoped to just the one hook this class needs (PlayerScript::PlayerScript falls back
    // to enabling every hook when the list is left empty, so this is a minor precision/
    // efficiency choice, not something required for the hook to fire).
    AuctionSimAddonBridge() : PlayerScript("AuctionSimAddonBridge", {PLAYERHOOK_ON_BEFORE_SEND_CHAT_MESSAGE}) {}

    void OnPlayerBeforeSendChatMessage(Player* player, uint32& type, uint32& lang, std::string& msg) override;
};

void AddAuctionSimAddonBridgeScript();
