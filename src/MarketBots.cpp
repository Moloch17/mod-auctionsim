#include "MarketBots.h"
#include <algorithm>
#include <cstdlib>
#include <string_view>
#include <memory>
#include "AccountMgr.h"
#include "CharacterCache.h"
#include "DatabaseEnv.h"
#include "Log.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QueryResult.h"
#include "Realm.h"
#include "Random.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"

namespace Market
{
    namespace
    {
        // Retries of a Pending Ensure (one every couple of seconds) before giving up on
        // accounts that never appeared.
        constexpr uint32 kMaxPendingTries = 30;

        struct RaceClass
        {
            uint8 race;
            uint8 playerClass;
        };

        // Valid race/class pairs, cycled so a faction's sellers aren't all one race.
        constexpr RaceClass kAllianceLooks[] = {
            {RACE_HUMAN, CLASS_WARRIOR},
            {RACE_DWARF, CLASS_WARRIOR},
            {RACE_NIGHTELF, CLASS_WARRIOR},
            {RACE_GNOME, CLASS_WARRIOR},
            {RACE_DRAENEI, CLASS_WARRIOR},
        };
        constexpr RaceClass kHordeLooks[] = {
            {RACE_ORC, CLASS_WARRIOR},
            {RACE_UNDEAD_PLAYER, CLASS_WARRIOR},
            {RACE_TAUREN, CLASS_WARRIOR},
            {RACE_TROLL, CLASS_WARRIOR},
            {RACE_BLOODELF, CLASS_PALADIN},
        };

        std::string RandomPassword()
        {
            static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
            std::string password(MAX_PASS_STR, 'A');
            for (char& c : password)
            {
                c = kAlphabet[urand(0, sizeof(kAlphabet) - 2)];
            }
            return password;
        }

        // A name the module may create: a well-formed, unreserved character name that no
        // character on the realm (cache or table) has.
        bool IsFreeName(std::string const& name)
        {
            std::string normalized = name;
            if (!normalizePlayerName(normalized) || normalized != name)
            {
                return false;
            }
            if (ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS || sObjectMgr->IsReservedName(name))
            {
                return false;
            }
            if (sCharacterCache->GetCharacterGuidByName(name))
            {
                return false;
            }
            CharacterDatabasePreparedStatement* stmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
            stmt->SetData(0, name);
            return !CharacterDatabase.Query(stmt);
        }
    }

    std::vector<BotPick> PickBots(
        std::vector<BotName> const& rows,
        uint32 wanted,
        std::function<uint32(std::string const&)> const& ownedGuid,
        std::function<bool(std::string const&)> const& available)
    {
        std::vector<BotPick> picks;
        picks.reserve(std::min<size_t>(wanted, rows.size()));
        for (BotName const& row : rows)
        {
            if (picks.size() >= wanted)
            {
                break;
            }
            if (uint32 guid = ownedGuid(row.name))
            {
                picks.push_back({row.name, guid});
            }
            else if (available(row.name))
            {
                picks.push_back({row.name, 0});
            }
        }
        return picks;
    }

    std::string BotRoster::AccountName(size_t faction, uint32 number)
    {
        return Acore::StringFormat("{}{}{:02}", kAccountPrefix, faction == 0 ? 'A' : 'H', number);
    }

    void BotRoster::Discover()
    {
        _accounts.clear();
        _owned.clear();
        _botGuids.clear();

        QueryResult accounts =
            LoginDatabase.Query("SELECT id, username FROM account WHERE username LIKE '{}%'", kAccountPrefix);
        if (!accounts)
        {
            return;
        }

        std::string idList;
        do
        {
            Field* fields = accounts->Fetch();
            Account account;
            account.id = fields[0].Get<uint32>();
            account.name = fields[1].Get<std::string>();
            size_t const prefix = std::string_view(kAccountPrefix).size();
            if (account.name.size() <= prefix || (account.name[prefix] != 'A' && account.name[prefix] != 'H'))
            {
                continue;
            }
            account.faction = account.name[prefix] == 'A' ? 0 : 1;
            idList += (idList.empty() ? "" : ",") + std::to_string(account.id);
            _accounts.push_back(std::move(account));
        } while (accounts->NextRow());

        std::sort(_accounts.begin(), _accounts.end(), [](Account const& a, Account const& b) {
            return a.name < b.name;
        });
        if (idList.empty())
        {
            return;
        }

        QueryResult characters =
            CharacterDatabase.Query("SELECT guid, account, name, race FROM characters WHERE account IN ({})", idList);
        if (!characters)
        {
            return;
        }
        do
        {
            Field* fields = characters->Fetch();
            uint32 guid = fields[0].Get<uint32>();
            uint32 accountId = fields[1].Get<uint32>();
            std::string name = fields[2].Get<std::string>();
            uint8 race = fields[3].Get<uint8>();

            _botGuids.insert(guid);
            for (Account& account : _accounts)
            {
                if (account.id == accountId)
                {
                    account.characters++;
                }
            }
            if (!name.empty())
            {
                size_t faction = Player::TeamIdForRace(race) == TEAM_ALLIANCE ? 0 : 1;
                _owned[name] = {guid, faction};
            }
        } while (characters->NextRow());
    }

    void BotRoster::LoadExisting()
    {
        Discover();
        if (!_botGuids.empty())
        {
            LOG_INFO("module", "AuctionSim: {} market seller character(s) on {} account(s)", _botGuids.size(),
                _accounts.size());
        }
    }

    BotRoster::Result BotRoster::Ensure(Data const& data, uint32 wanted, std::string& note)
    {
        Discover();

        std::unordered_set<std::string> claimed;
        std::array<std::vector<BotPick>, kFactions> picks;
        bool waitingForAccounts = false;

        for (size_t faction = 0; faction < kFactions; ++faction)
        {
            _slots[faction].clear();
            _slotNames[faction].clear();
            if (!data.factions[faction].present)
            {
                continue;
            }

            auto owned = [this, faction](std::string const& name) -> uint32 {
                auto it = _owned.find(name);
                return it != _owned.end() && it->second.faction == faction ? it->second.guid : 0;
            };
            auto available = [&claimed, this](std::string const& name) {
                return !claimed.count(name) && !_owned.count(name) && IsFreeName(name);
            };
            picks[faction] = PickBots(data.factions[faction].bots, wanted, owned, available);
            for (BotPick const& pick : picks[faction])
            {
                claimed.insert(pick.name);
            }

            uint32 toCreate = static_cast<uint32>(
                std::count_if(picks[faction].begin(), picks[faction].end(), [](BotPick const& p) { return !p.guid; }));
            uint32 capacity = 0;
            uint32 highest = 0;
            for (Account const& account : _accounts)
            {
                if (account.faction != faction)
                {
                    continue;
                }
                if (account.characters < kCharactersPerAccount)
                {
                    capacity += kCharactersPerAccount - account.characters;
                }
                // AHSIMMKT<A|H><nn>: the number after the faction letter.
                char const* number = account.name.c_str() + std::string_view(kAccountPrefix).size() + 1;
                highest = std::max<uint32>(highest, static_cast<uint32>(std::strtoul(number, nullptr, 10)));
            }

            // Account creation goes through AccountMgr, whose insert is asynchronous: ask
            // for the missing accounts now and pick them up on a later call.
            for (uint32 number = highest + 1; capacity < toCreate; ++number)
            {
                std::string name = AccountName(faction, number);
                if (!_requestedAccounts.count(name))
                {
                    AccountOpResult result = sAccountMgr->CreateAccount(name, RandomPassword());
                    if (result != AOR_OK && result != AOR_NAME_ALREADY_EXIST)
                    {
                        note = Acore::StringFormat("couldn't create bot account {} (AccountMgr error {})", name,
                            static_cast<uint32>(result));
                        return Result::Failed;
                    }
                    _requestedAccounts.insert(name);
                    LOG_INFO("module", "AuctionSim: requested market bot account {}", name);
                }
                capacity += kCharactersPerAccount;
                waitingForAccounts = true;
            }
        }

        if (waitingForAccounts)
        {
            if (++_pendingTries > kMaxPendingTries)
            {
                note = "market bot accounts were requested but never appeared in the auth database";
                return Result::Failed;
            }
            note = "waiting for market bot accounts to be created";
            return Result::Pending;
        }
        _pendingTries = 0;

        for (size_t faction = 0; faction < kFactions; ++faction)
        {
            if (!CreateCharacters(faction, picks[faction], note))
            {
                return Result::Failed;
            }
            for (BotPick const& pick : picks[faction])
            {
                if (pick.guid)
                {
                    _slots[faction].push_back(ObjectGuid::Create<HighGuid::Player>(pick.guid));
                    _slotNames[faction].push_back(pick.name);
                }
            }
        }
        return Result::Ready;
    }

    bool BotRoster::CreateCharacters(size_t faction, std::vector<BotPick>& picks, std::string& note)
    {
        auto next = picks.begin();
        auto advance = [&next, &picks]() {
            while (next != picks.end() && next->guid)
            {
                ++next;
            }
        };
        advance();
        if (next == picks.end())
        {
            return true;
        }

        uint32 created = 0;
        for (Account& account : _accounts)
        {
            if (account.faction != faction || account.characters >= kCharactersPerAccount)
            {
                continue;
            }

            // One headless session per account: Player::Create reads its permissions.
            WorldSession session(account.id, std::string(account.name), 0, nullptr, SEC_PLAYER,
                sWorld->getIntConfig(CONFIG_EXPANSION), 0, LOCALE_enUS, 0, false, false, 0);
            CharacterDatabaseTransaction trans = CharacterDatabase.BeginTransaction();
            uint32 before = account.characters;

            while (next != picks.end() && account.characters < kCharactersPerAccount)
            {
                RaceClass const look = faction == 0 ? kAllianceLooks[(_botGuids.size() + created) % 5]
                                                    : kHordeLooks[(_botGuids.size() + created) % 5];
                uint8 const gender = static_cast<uint8>(urand(GENDER_MALE, GENDER_FEMALE));
                CharacterCreateInfo createInfo(next->name, look.race, look.playerClass, gender);

                auto player = std::make_unique<Player>(&session);
                player->GetMotionMaster()->Initialize();
                ObjectGuid::LowType guid = sObjectMgr->GetGenerator<HighGuid::Player>().Generate();
                if (!player->Create(guid, &createInfo))
                {
                    LOG_ERROR("module", "AuctionSim: couldn't create market seller '{}', skipping it", next->name);
                    ++next;
                    advance();
                    continue;
                }
                player->setCinematic(1);
                player->SetAtLoginFlag(AT_LOGIN_FIRST);
                player->SaveToDB(trans, true, false);
                sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), account.id, player->GetName(),
                    player->getGender(), player->getRace(), player->getClass(), player->GetLevel());
                player->CleanupsBeforeDelete();

                next->guid = guid;
                _botGuids.insert(guid);
                _owned[next->name] = {guid, faction};
                account.characters++;
                created++;
                ++next;
                advance();
            }

            CharacterDatabase.CommitTransaction(trans);
            if (account.characters != before)
            {
                LoginDatabasePreparedStatement* stmt = LoginDatabase.GetPreparedStatement(LOGIN_REP_REALM_CHARACTERS);
                stmt->SetData(0, static_cast<uint8>(account.characters));
                stmt->SetData(1, account.id);
                stmt->SetData(2, realm.Id.Realm);
                LoginDatabase.Execute(stmt);
            }
            if (next == picks.end())
            {
                break;
            }
        }

        if (created)
        {
            LOG_INFO("module", "AuctionSim: created {} market seller character(s) for faction {}", created,
                FactionHouse(faction));
        }
        if (next != picks.end())
        {
            note = "ran out of bot account capacity while creating sellers";
            return false;
        }
        return true;
    }
}
