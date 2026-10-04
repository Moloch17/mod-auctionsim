#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "Define.h"
#include "MarketData.h"
#include "ObjectGuid.h"

namespace Market
{
    // One chosen seller: an existing bot character (guid != 0) or a name to create.
    struct BotPick
    {
        std::string name;
        uint32 guid = 0;
    };

    // Walks a faction's BOT rows in order and takes the first `wanted` usable names:
    // a name this module already owns for the faction is reused (ownedGuid != 0); a
    // name free on the realm is taken for creation; anything else is skipped. Pure, so
    // the self-tests can check the skipping rules.
    std::vector<BotPick> PickBots(
        std::vector<BotName> const& rows,
        uint32 wanted,
        std::function<uint32(std::string const&)> const& ownedGuid,
        std::function<bool(std::string const&)> const& available);

    // True for exactly AHSIMMKT<A|H><digits>, the module's seller accounts; sets the
    // faction slot (0 Alliance, 1 Horde).
    bool ParseSellerAccountName(std::string const& name, size_t& faction);

    // --- Purge ---------------------------------------------------------------------
    struct PurgeAccount
    {
        uint32 id = 0;
        std::string name;
        bool hasAccess = false;  // has an account_access (GM) row
    };
    struct PurgeCharacter
    {
        uint32 guid = 0;
        uint32 account = 0;
        std::string name;
        uint8 race = 0;
        uint8 playerClass = 0;
        uint8 level = 0;
        uint32 atLogin = 0;
        uint32 totalTime = 0;
        bool online = false;
    };
    struct PurgePlan
    {
        std::vector<PurgeAccount> accounts;      // to delete
        std::vector<PurgeCharacter> characters;  // to delete (all on those accounts)
        std::vector<std::string> problems;       // non-empty: refuse the whole purge
    };

    // Decides what a purge may delete, from every account whose name starts with the
    // seller prefix and every character on them. Only accounts named exactly like the
    // module's are taken, and only if every character on them looks like one the module
    // created (level 1, never played, first-login flag still set, a race/class pair the
    // module uses, of the account's faction) and the account has no GM access. Anything
    // else -- or the buyer bot's account / character turning up -- is a problem that
    // refuses the purge. Pure, for the self-tests.
    PurgePlan PlanPurge(
        std::vector<PurgeAccount> const& candidates,
        std::vector<PurgeCharacter> const& characters,
        uint32 buyerAccount,
        uint32 buyerCharacter);

    // The named sellers' characters. They live on module-owned accounts named
    // AHSIMMKT<A|H><nn>, ten characters per account (the realm's CharactersPerRealm
    // ceiling), Alliance and Horde on separate accounts. Each account gets a random
    // password nobody is told; a login on one of them is kicked anyway (see
    // AuctionSimMarketGuard). Characters are real rows, so names show in the AH,
    // survive restarts and can never be taken by a player.
    class BotRoster
    {
    public:
        static constexpr uint32 kCharactersPerAccount = 10;
        static constexpr char const* kAccountPrefix = "AHSIMMKT";

        // Reads the seller accounts and their characters from the DB and plans a purge.
        static PurgePlan GatherPurge(uint32 buyerAccount, uint32 buyerCharacter);

        // Forgets every seller (after a purge): IsBot() is false for all, no slots.
        void Clear();

        // Reads every character on the module's market accounts (any mode, so leftover
        // bot auctions keep their mail swallowed after a switch back to Replay).
        void LoadExisting();

        enum class Result
        {
            Ready,
            Pending,  // accounts were just requested (account creation is async); call again
            Failed,
        };

        // Makes sure each present faction has up to `wanted` bot characters, creating
        // accounts and characters as needed, and fills the slot lists. Character creation
        // is synchronous (one-off, at first Market start or when Market.Bots grows).
        Result Ensure(Data const& data, uint32 wanted, std::string& note);

        bool IsBot(uint32 lowGuid) const { return _botGuids.count(lowGuid) > 0; }
        std::vector<ObjectGuid> const& Slots(size_t faction) const { return _slots[faction]; }
        std::vector<std::string> const& SlotNames(size_t faction) const { return _slotNames[faction]; }
        size_t KnownBotCount() const { return _botGuids.size(); }

    private:
        struct Account
        {
            uint32 id = 0;
            std::string name;
            size_t faction = 0;
            uint32 characters = 0;
        };
        struct Owned
        {
            uint32 guid = 0;
            size_t faction = 0;
        };

        static std::string AccountName(size_t faction, uint32 number);
        void Discover();
        bool CreateCharacters(size_t faction, std::vector<BotPick>& picks, std::string& note);

        std::vector<Account> _accounts;
        std::unordered_map<std::string, Owned> _owned;  // character name -> owner
        std::unordered_set<uint32> _botGuids;
        std::array<std::vector<ObjectGuid>, kFactions> _slots;
        std::array<std::vector<std::string>, kFactions> _slotNames;
        std::unordered_set<std::string> _requestedAccounts;
        uint32 _pendingTries = 0;
    };
}
