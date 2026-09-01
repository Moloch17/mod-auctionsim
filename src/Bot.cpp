#include "Bot.h"

Bot::Bot(bool& outBuilt)
{
    this->accountID = sConfigMgr->GetOption<uint32>("AuctionSim.BotAccountID", 0);
    this->characterID = sConfigMgr->GetOption<uint32>("AuctionSim.BotCharacterID", 0);

    if (this->accountID == 0 || this->characterID == 0)
    {
        LOG_ERROR("module", "AuctionSim: invalid account/character id");
        outBuilt = false;
        return;
    }
    LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_GET_USERNAME_BY_ID);
    stmt->SetData(0, this->accountID);
    PreparedQueryResult result = LoginDatabase.Query(stmt);

    if (!result)
    {
        LOG_ERROR("module", "AuctionSim: Bot creation failed: didn't get info from database");
        outBuilt = false;
        return;
    }

    Field* fields = result->Fetch();
    std::string accountName = fields[0].Get<std::string>();
    // WorldSession(id, name, accountFlags=0, sock=nullptr, sec=SEC_PLAYER, expansion,
    //              mute_time=0, locale, recruiter=0, isARecruiter=false, skipQueue=false, TotalTime=0)
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
