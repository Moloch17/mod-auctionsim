#pragma once
#include "Player.h"
#include "WorldSession.h"

// A headless character the module drives to list and buy auctions. Its Player is
// only ever torn down at shutdown (see AuctionSim::retiredBots).
class Bot
{
    uint32 accountID = 0;
    uint32 characterID = 0;
    std::unique_ptr<WorldSession> session;
    std::unique_ptr<Player> player;

public:
    // outBuilt is set to false if the account/character ids are unset or don't
    // resolve -- a construction-success flag, not the module's enabled state.
    explicit Bot(bool& outBuilt);

    uint32 GetAccountID() const { return accountID; }
    uint32 GetCharacterID() const { return characterID; }
    std::unique_ptr<WorldSession>& GetSession() { return session; }
    std::unique_ptr<Player>& GetPlayer() { return player; }

    // Non-owning reference to the bot's Player. Prefer this over GetPlayer().get()->
    // at call sites that just need the character. Only call once GetPlayer() is known
    // non-null (i.e. outBuilt was true).
    Player& GetPlayerRef() { return *player; }
};
