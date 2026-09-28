#include "AccountMgr.h"
#include "Chat.h"
#include "CommandScript.h"
#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "Item.h"
#include "Log.h"
#include "Player.h"
#include "QuestDef.h"
#include "RBAC.h"
#include "ScriptMgr.h"
#include "Unit.h"
#include "World.h"
#include "WorldSession.h"

#include <chrono>
#include <exception>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace Acore::ChatCommands;

namespace PlayerStatistics
{
enum class TargetType : uint8
{
    None        = 0,
    Player      = 1,
    Creature    = 2,
    Item        = 3,
    Quest       = 4,
    Achievement = 5
};

struct Settings
{
    bool Enabled = true;
    bool IgnoreGameMasters = false;
    uint32 RealmId = 1;

    bool CreatureKills = true;
    bool PetCreatureKills = true;
    bool PlayerDeaths = true;
    bool PlayerKilledByCreature = true;
    bool PvPKills = true;
    bool LevelChanges = true;
    bool QuestCompletions = true;
    bool Achievements = true;

    // High-volume events are intentionally off by default.
    bool LootItems = false;
    bool ExperienceGains = false;
    bool MoneyChanges = false;
};

Settings gSettings;

using AccountCacheClock = std::chrono::steady_clock;

constexpr std::chrono::minutes SuccessfulAccountCacheLifetime{ 5 };
constexpr std::chrono::seconds FailedAccountCacheLifetime{ 30 };

struct AccountLoginCacheEntry
{
    std::string Login;
    AccountCacheClock::time_point ExpiresAt;
    bool Found = false;
};

struct OnlinePlayerRow
{
    uint32 AccountId;
    std::string AccountLogin;
    uint64 CharacterGuid;
    std::string CharacterName;
    uint8 RaceId;
    uint8 ClassId;
    uint8 Level;
    uint32 MapId;
    uint32 ZoneId;
    uint32 AreaId;
    std::string Location;
};

std::unordered_map<uint32, AccountLoginCacheEntry> gAccountLoginCache;

void PruneAccountLoginCache(AccountCacheClock::time_point now)
{
    for (auto itr = gAccountLoginCache.begin(); itr != gAccountLoginCache.end();)
    {
        if (itr->second.ExpiresAt <= now)
            itr = gAccountLoginCache.erase(itr);
        else
            ++itr;
    }
}

bool ResolveAccountLogin(uint32 accountId, std::string& login)
{
    AccountCacheClock::time_point const now = AccountCacheClock::now();
    auto const cached = gAccountLoginCache.find(accountId);

    if (cached != gAccountLoginCache.end() && cached->second.ExpiresAt > now)
    {
        if (cached->second.Found)
            login = cached->second.Login;

        return cached->second.Found;
    }

    std::string resolvedLogin;
    bool const found = AccountMgr::GetName(accountId, resolvedLogin) && !resolvedLogin.empty();
    gAccountLoginCache[accountId] = {
        found ? resolvedLogin : std::string(),
        now + (found ? SuccessfulAccountCacheLifetime : FailedAccountCacheLifetime),
        found
    };

    if (!found)
    {
        LOG_WARN("module", "Player Statistics could not resolve an account login for account ID {}.", accountId);
        return false;
    }

    login = std::move(resolvedLogin);
    return true;
}

std::string ResolveLocation(uint32 zoneId, uint32 areaId, uint32 mapId)
{
    LocaleConstant const locale = sWorld->GetDefaultDbcLocale();

    if (AreaTableEntry const* zone = sAreaTableStore.LookupEntry(zoneId))
    {
        if (char const* name = zone->area_name[locale]; name && *name)
            return name;
    }

    if (AreaTableEntry const* area = sAreaTableStore.LookupEntry(areaId))
    {
        if (char const* name = area->area_name[locale]; name && *name)
            return name;
    }

    if (MapEntry const* map = sMapStore.LookupEntry(mapId))
    {
        if (char const* name = map->name[locale]; name && *name)
            return name;
    }

    return "Unknown";
}

bool AppendJsonString(std::string const& value, std::string& output)
{
    static char constexpr HexDigits[] = "0123456789abcdef";

    output.push_back('"');

    for (std::size_t index = 0; index < value.size();)
    {
        unsigned char const character = static_cast<unsigned char>(value[index]);

        switch (character)
        {
            case '"': output += "\\\""; ++index; continue;
            case '\\': output += "\\\\"; ++index; continue;
            case '\b': output += "\\b"; ++index; continue;
            case '\f': output += "\\f"; ++index; continue;
            case '\n': output += "\\n"; ++index; continue;
            case '\r': output += "\\r"; ++index; continue;
            case '\t': output += "\\t"; ++index; continue;
            default: break;
        }

        if (character < 0x20)
        {
            output += "\\u00";
            output.push_back(HexDigits[(character >> 4) & 0x0F]);
            output.push_back(HexDigits[character & 0x0F]);
            ++index;
            continue;
        }

        if (character < 0x80)
        {
            output.push_back(value[index++]);
            continue;
        }

        // Preserve valid UTF-8 verbatim. Reject malformed sequences so the
        // success marker can never prefix a payload that is not valid JSON.
        std::size_t sequenceLength = 0;
        if (character >= 0xC2 && character <= 0xDF)
            sequenceLength = 2;
        else if (character >= 0xE0 && character <= 0xEF)
            sequenceLength = 3;
        else if (character >= 0xF0 && character <= 0xF4)
            sequenceLength = 4;
        else
            return false;

        if (index + sequenceLength > value.size())
            return false;

        unsigned char const second = static_cast<unsigned char>(value[index + 1]);
        if ((second & 0xC0) != 0x80)
            return false;

        if ((character == 0xE0 && second < 0xA0) ||
            (character == 0xED && second > 0x9F) ||
            (character == 0xF0 && second < 0x90) ||
            (character == 0xF4 && second > 0x8F))
        {
            return false;
        }

        for (std::size_t offset = 2; offset < sequenceLength; ++offset)
        {
            if ((static_cast<unsigned char>(value[index + offset]) & 0xC0) != 0x80)
                return false;
        }

        output.append(value, index, sequenceLength);
        index += sequenceLength;
    }

    output.push_back('"');
    return true;
}

bool BuildOnlinePlayersJson(int64 generatedAt, std::vector<OnlinePlayerRow> const& players, std::string& output)
{
    output.clear();
    output.reserve(64 + players.size() * 256);
    output += "{\"generatedAt\":";
    output += std::to_string(generatedAt);
    output += ",\"players\":[";

    bool first = true;
    for (OnlinePlayerRow const& player : players)
    {
        if (!first)
            output.push_back(',');

        first = false;
        output += "{\"accountId\":";
        output += std::to_string(player.AccountId);
        output += ",\"accountLogin\":";
        if (!AppendJsonString(player.AccountLogin, output))
            return false;

        output += ",\"characterGuid\":";
        output += std::to_string(player.CharacterGuid);
        output += ",\"characterName\":";
        if (!AppendJsonString(player.CharacterName, output))
            return false;

        output += ",\"raceId\":";
        output += std::to_string(player.RaceId);
        output += ",\"classId\":";
        output += std::to_string(player.ClassId);
        output += ",\"level\":";
        output += std::to_string(player.Level);
        output += ",\"mapId\":";
        output += std::to_string(player.MapId);
        output += ",\"zoneId\":";
        output += std::to_string(player.ZoneId);
        output += ",\"areaId\":";
        output += std::to_string(player.AreaId);
        output += ",\"location\":";
        if (!AppendJsonString(player.Location, output))
            return false;

        output.push_back('}');
    }

    output += "]}";
    return true;
}

void LoadSettings()
{
    gSettings.Enabled = sConfigMgr->GetOption<bool>("PlayerStatistics.Enable", true);
    gSettings.IgnoreGameMasters = sConfigMgr->GetOption<bool>("PlayerStatistics.IgnoreGameMasters", false);
    gSettings.RealmId = sConfigMgr->GetOption<uint32>("PlayerStatistics.RealmId", 1);

    gSettings.CreatureKills = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.CreatureKills", true);
    gSettings.PetCreatureKills = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.PetCreatureKills", true);
    gSettings.PlayerDeaths = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.PlayerDeaths", true);
    gSettings.PlayerKilledByCreature = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.PlayerKilledByCreature", true);
    gSettings.PvPKills = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.PvPKills", true);
    gSettings.LevelChanges = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.LevelChanges", true);
    gSettings.QuestCompletions = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.QuestCompletions", true);
    gSettings.Achievements = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.Achievements", true);

    gSettings.LootItems = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.LootItems", false);
    gSettings.ExperienceGains = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.ExperienceGains", false);
    gSettings.MoneyChanges = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.MoneyChanges", false);
}

bool ShouldLog(Player const* player)
{
    if (!gSettings.Enabled || !player)
        return false;

    if (gSettings.IgnoreGameMasters && player->IsGameMaster())
        return false;

    return true;
}

uint32 GetAccountId(Player const* player)
{
    return player && player->GetSession() ? player->GetSession()->GetAccountId() : 0;
}

bool IsBot(Player const* player)
{
    // The AzerothCore Playerbots test-staging core represents bot-controlled
    // sessions as headless (socketless) sessions. This covers both altbots and
    // random bots without relying on account IDs.
    return player && player->GetSession() && player->GetSession()->IsHeadless();
}

void LogEvent(
    Player* player,
    char const* eventType,
    TargetType targetType = TargetType::None,
    uint32 targetEntry = 0,
    uint64 targetGuid = 0,
    bool targetIsBot = false,
    int64 value1 = 0,
    int64 value2 = 0,
    char const* source = "")
{
    if (!ShouldLog(player))
        return;

    // eventType and source are module-owned constants, never player/user input.
    // CharacterDatabase.Execute uses the normal DB worker pool rather than a
    // synchronous direct write, keeping the event hook lightweight.
    CharacterDatabase.Execute(
        "INSERT INTO `mod_player_stats_events` "
        "(`realm_id`, `event_type`, `actor_account_id`, `actor_guid`, `actor_is_bot`, `actor_level`, `actor_class`, `actor_race`, "
        "`target_type`, `target_entry`, `target_guid`, `target_is_bot`, `value1`, `value2`, `map_id`, `instance_id`, `zone_id`, `area_id`, `source`) "
        "VALUES ({}, '{}', {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, '{}')",
        gSettings.RealmId,
        eventType,
        GetAccountId(player),
        player->GetGUID().GetCounter(),
        IsBot(player) ? 1 : 0,
        player->GetLevel(),
        player->getClass(),
        player->getRace(),
        static_cast<uint8>(targetType),
        targetEntry,
        targetGuid,
        targetIsBot ? 1 : 0,
        value1,
        value2,
        player->GetMapId(),
        player->GetInstanceId(),
        player->GetZoneId(),
        player->GetAreaId(),
        source);
}

class PlayerStatisticsConfigScript : public WorldScript
{
public:
    PlayerStatisticsConfigScript()
        : WorldScript("PlayerStatisticsConfigScript", { WORLDHOOK_ON_AFTER_CONFIG_LOAD })
    {
    }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        LoadSettings();
    }
};

class PlayerStatisticsCommandScript : public CommandScript
{
public:
    PlayerStatisticsCommandScript()
        : CommandScript("PlayerStatisticsCommandScript")
    {
    }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerStatisticsCommandTable =
        {
            { "online", HandleOnlineCommand, rbac::RBAC_PERM_COMMAND_SERVER_INFO, Console::Yes }
        };

        static ChatCommandTable commandTable =
        {
            { "playerstats", playerStatisticsCommandTable }
        };

        return commandTable;
    }

private:
    static bool HandleOnlineCommand(ChatHandler* handler)
    {
        if (handler->GetSession())
        {
            handler->SendErrorMessage("The playerstats online command is available only through the worldserver console or SOAP.");
            return false;
        }

        try
        {
            AccountCacheClock::time_point const now = AccountCacheClock::now();
            PruneAccountLoginCache(now);

            std::vector<OnlinePlayerRow> players;
            handler->DoForAllValidSessions([&players](Player* player)
            {
                WorldSession* session = player ? player->GetSession() : nullptr;
                if (!session || session->IsHeadless())
                    return;

                std::string accountLogin;
                uint32 const accountId = session->GetAccountId();
                if (!ResolveAccountLogin(accountId, accountLogin))
                    return;

                uint32 const mapId = player->GetMapId();
                uint32 const zoneId = player->GetZoneId();
                uint32 const areaId = player->GetAreaId();
                players.push_back({
                    accountId,
                    std::move(accountLogin),
                    player->GetGUID().GetCounter(),
                    player->GetName(),
                    player->getRace(),
                    player->getClass(),
                    player->GetLevel(),
                    mapId,
                    zoneId,
                    areaId,
                    ResolveLocation(zoneId, areaId, mapId)
                });
            });

            int64 const generatedAt = std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            if (generatedAt < 0)
                throw std::runtime_error("system clock precedes Unix epoch");

            std::string json;
            if (!BuildOnlinePlayersJson(generatedAt, players, json))
                throw std::runtime_error("online roster contains invalid UTF-8");

            handler->SendSysMessage("PLAYERSTATS_ONLINE_V1 " + json);
            return true;
        }
        catch (std::exception const&)
        {
            LOG_ERROR("module", "Player Statistics failed to serialize the online-player roster.");
            handler->SendErrorMessage("The online-player roster could not be generated.");
            return false;
        }
        catch (...)
        {
            LOG_ERROR("module", "Player Statistics failed to serialize the online-player roster.");
            handler->SendErrorMessage("The online-player roster could not be generated.");
            return false;
        }
    }
};

class PlayerStatisticsPlayerScript : public PlayerScript
{
public:
    PlayerStatisticsPlayerScript()
        : PlayerScript("PlayerStatisticsPlayerScript", {
            PLAYERHOOK_ON_PLAYER_JUST_DIED,
            PLAYERHOOK_ON_PLAYER_COMPLETE_QUEST,
            PLAYERHOOK_ON_PVP_KILL,
            PLAYERHOOK_ON_CREATURE_KILL,
            PLAYERHOOK_ON_CREATURE_KILLED_BY_PET,
            PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE,
            PLAYERHOOK_ON_LEVEL_CHANGED,
            PLAYERHOOK_ON_MONEY_CHANGED,
            PLAYERHOOK_ON_GIVE_EXP,
            PLAYERHOOK_ON_ACHI_COMPLETE,
            PLAYERHOOK_ON_LOOT_ITEM
        })
    {
    }

    void OnPlayerJustDied(Player* player) override
    {
        if (!gSettings.PlayerDeaths)
            return;

        LogEvent(
            player,
            "PLAYER_DEATH",
            TargetType::None,
            0,
            0,
            false,
            0,
            0,
            "canonical");
    }

    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        if (!gSettings.CreatureKills || !killed)
            return;

        LogEvent(
            killer,
            "CREATURE_KILL",
            TargetType::Creature,
            killed->GetEntry(),
            killed->GetGUID().GetCounter(),
            false,
            killed->GetLevel(),
            0,
            "direct");
    }

    void OnPlayerCreatureKilledByPet(Player* petOwner, Creature* killed) override
    {
        if (!gSettings.PetCreatureKills || !killed)
            return;

        LogEvent(
            petOwner,
            "CREATURE_KILL_PET",
            TargetType::Creature,
            killed->GetEntry(),
            killed->GetGUID().GetCounter(),
            false,
            killed->GetLevel(),
            0,
            "pet");
    }

    void OnPlayerKilledByCreature(Creature* killer, Player* killed) override
    {
        if (!gSettings.PlayerKilledByCreature || !killer)
            return;

        LogEvent(
            killed,
            "PLAYER_KILLED_BY_CREATURE",
            TargetType::Creature,
            killer->GetEntry(),
            killer->GetGUID().GetCounter(),
            false,
            killer->GetLevel(),
            0,
            "creature");
    }

    void OnPlayerPVPKill(Player* killer, Player* killed) override
    {
        if (!gSettings.PvPKills || !killed)
            return;

        LogEvent(
            killer,
            "PVP_KILL",
            TargetType::Player,
            0,
            killed->GetGUID().GetCounter(),
            IsBot(killed),
            killed->GetLevel(),
            GetAccountId(killed),
            "player");
    }

    void OnPlayerLevelChanged(Player* player, uint8 oldLevel) override
    {
        if (!gSettings.LevelChanges)
            return;

        LogEvent(
            player,
            "LEVEL_CHANGE",
            TargetType::None,
            0,
            0,
            false,
            oldLevel,
            player ? player->GetLevel() : 0,
            "level");
    }

    void OnPlayerCompleteQuest(Player* player, Quest const* quest) override
    {
        if (!gSettings.QuestCompletions || !quest)
            return;

        LogEvent(
            player,
            "QUEST_COMPLETE",
            TargetType::Quest,
            quest->GetQuestId(),
            0,
            false,
            0,
            0,
            "quest");
    }

    void OnPlayerAchievementComplete(Player* player, AchievementEntry const* achievement) override
    {
        if (!gSettings.Achievements || !achievement)
            return;

        LogEvent(
            player,
            "ACHIEVEMENT",
            TargetType::Achievement,
            achievement->ID,
            0,
            false,
            0,
            0,
            "achievement");
    }

    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid lootGuid) override
    {
        if (!gSettings.LootItems || !item)
            return;

        LogEvent(
            player,
            "LOOT_ITEM",
            TargetType::Item,
            item->GetEntry(),
            lootGuid.GetCounter(),
            false,
            count,
            0,
            "loot");
    }

    void OnPlayerGiveXP(Player* player, uint32& amount, Unit* victim, uint8 xpSource) override
    {
        if (!gSettings.ExperienceGains)
            return;

        TargetType targetType = TargetType::None;
        uint32 targetEntry = 0;
        uint64 targetGuid = 0;

        if (victim)
        {
            targetGuid = victim->GetGUID().GetCounter();

            if (Creature* creature = victim->ToCreature())
            {
                targetType = TargetType::Creature;
                targetEntry = creature->GetEntry();
            }
            else if (victim->ToPlayer())
            {
                targetType = TargetType::Player;
            }
        }

        LogEvent(
            player,
            "XP_GAIN",
            targetType,
            targetEntry,
            targetGuid,
            victim && victim->ToPlayer() ? IsBot(victim->ToPlayer()) : false,
            amount,
            xpSource,
            "xp");
    }

    void OnPlayerMoneyChanged(Player* player, int32& amount) override
    {
        if (!gSettings.MoneyChanges)
            return;

        // This hook fires before the modification is applied.
        int64 resultingBalance = player ? static_cast<int64>(player->GetMoney()) + static_cast<int64>(amount) : 0;

        LogEvent(
            player,
            "MONEY_CHANGE",
            TargetType::None,
            0,
            0,
            false,
            amount,
            resultingBalance,
            "money");
    }
};
} // namespace PlayerStatistics

void AddSC_mod_player_statistics()
{
    new PlayerStatistics::PlayerStatisticsConfigScript();
    new PlayerStatistics::PlayerStatisticsCommandScript();
    new PlayerStatistics::PlayerStatisticsPlayerScript();
}
