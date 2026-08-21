#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "DBCStructure.h"
#include "Item.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "Unit.h"
#include "WorldSession.h"

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

void LoadSettings()
{
    gSettings.Enabled = sConfigMgr->GetOption<bool>("PlayerStatistics.Enable", true);
    gSettings.IgnoreGameMasters = sConfigMgr->GetOption<bool>("PlayerStatistics.IgnoreGameMasters", false);
    gSettings.RealmId = sConfigMgr->GetOption<uint32>("PlayerStatistics.RealmId", 1);

    gSettings.CreatureKills = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.CreatureKills", true);
    gSettings.PetCreatureKills = sConfigMgr->GetOption<bool>("PlayerStatistics.Events.PetCreatureKills", true);
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
    // The AzerothCore Playerbots fork marks bot-controlled sessions explicitly.
    // This covers both altbots and random bots without relying on account IDs.
    return player && player->GetSession() && player->GetSession()->IsBot();
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

class PlayerStatisticsPlayerScript : public PlayerScript
{
public:
    PlayerStatisticsPlayerScript()
        : PlayerScript("PlayerStatisticsPlayerScript", {
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
    new PlayerStatistics::PlayerStatisticsPlayerScript();
}
