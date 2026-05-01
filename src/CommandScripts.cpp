#include <chrono>
#include "AuctionSim.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

class AuctionSimCommandScript : public CommandScript
{
public:
    AuctionSimCommandScript() : CommandScript("AuctionSimCommandScript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable commandTable = {
            {"scan", HandleScanAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
            {"delete", HandleDeleteAuctionsCommand, SEC_ADMINISTRATOR, Console::Yes},
        };
        return commandTable;
    }

    static bool HandleScanAuctionsCommand(ChatHandler* handler)
    {
        Player* player = nullptr;
        if (handler->GetSession()) player = handler->GetSession()->GetPlayer();

        auto start = std::chrono::high_resolution_clock::now();

        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Alliance, 0);
        AuctionSim::instance()->ScanAuctions(AuctionHouseId::Horde, 1);

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        handler->SendSysMessage(fmt::format("Auction scan completed in {} ms", elapsed));
        return true;
    }

    static bool HandleDeleteAuctionsCommand(ChatHandler* handler)
    {
        Player* player = nullptr;
        if (handler->GetSession()) player = handler->GetSession()->GetPlayer();

        auto start = std::chrono::high_resolution_clock::now();

        AuctionSim::instance()->DeleteAuctions(player);

        auto end = std::chrono::high_resolution_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
        handler->SendSysMessage(fmt::format("Auction delete completed in {} ms", elapsed));
        return true;
    }
};

void AddSC_AuctionCommandScript() { new AuctionSimCommandScript(); }