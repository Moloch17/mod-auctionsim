#include "Bot.h"

Bot::Bot(bool& _isEnabled)
{
    this->accountID = sConfigMgr->GetOption<int>("AuctionSim.BotAccountID", 0);
    this->characterID = sConfigMgr->GetOption<int>("AuctionSim.BotCharacterID", 0);

    if (this->accountID == 0 || this->characterID == 0)
    {
        LOG_ERROR("module", "AuctionSim: invalid account/character id");
        _isEnabled = false;
    }
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_USERNAME_BY_ID);
    stmt->SetData(0, this->accountID);
    PreparedQueryResult result = LoginDatabase.Query(stmt);

    if (!result)
    {
        LOG_ERROR("module", "AuctionSim: Bot creation failed: didn't get info from database");
        _isEnabled = false;
        return;
    }

    Field* fields = result->Fetch();
    std::string accountName = fields[0].Get<std::string>();
    this->session = std::make_unique<WorldSession>(
        this->accountID,
        accountName.c_str(),
        0,
        nullptr,
        SEC_PLAYER,
        sWorld->getIntConfig(CONFIG_EXPANSION),
        0,
        LOCALE_enUS,
        0,
        false,
        false,
        0);
    this->player = std::make_unique<Player>(this->session.get());
    this->player->Initialize(this->characterID);

    LOG_INFO("module", "AuctionSim: created bot");
}

int Bot::GetAccountID() { return this->accountID; }
int Bot::GetCharacterID() { return this->characterID; }
std::unique_ptr<WorldSession>& Bot::GetSession() { return this->session; }
std::unique_ptr<Player>& Bot::GetPlayer() { return this->player; }
