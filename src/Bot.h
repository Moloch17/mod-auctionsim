#pragma once
#include "Player.h"
#include "WorldSession.h"
class Bot
{
    int accountID;
    int characterID;
    std::unique_ptr<WorldSession> session;
    std::unique_ptr<Player> player;

public:
    int GetAccountID();
    int GetCharacterID();
    std::unique_ptr<WorldSession>& GetSession();
    std::unique_ptr<Player>& GetPlayer();
    Bot(bool& _isEnabled);
};
