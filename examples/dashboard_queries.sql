-- Player Statistics: example dashboard queries
--
-- These examples assume this table lives in the characters DB.
-- Replace YOUR_WORLD_DB with the actual AzerothCore world database name when
-- you want creature names from creature_template.

-- BOT FILTERING
-- actor_is_bot = 0  -> human-controlled character
-- actor_is_bot = 1  -> Playerbot (altbot or random bot)
-- target_is_bot is populated when the event target is another Player.
--
-- Add one of these to any WHERE clause as needed:
--   AND e.actor_is_bot = 0   -- humans only
--   AND e.actor_is_bot = 1   -- bots only
-- Omit the predicate for combined server totals.
--
-- TOTAL DEATHS
-- Never total every death-shaped event together. For fresh post-cutover data,
-- PLAYER_DEATH is canonical. For lifetime totals on upgraded installations,
-- use the cutover-aware query below: specialized rows are legacy totals only
-- at or below the immutable canonical_player_death_v1 cutoff.

-- ---------------------------------------------------------------------------
-- 1. Top characters by total NPC kills (direct + pet kills)
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    e.actor_is_bot,
    COUNT(*) AS npc_kills
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
GROUP BY e.actor_guid, c.name, e.actor_is_bot
ORDER BY npc_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 2. Top accounts by total NPC kills
-- ---------------------------------------------------------------------------
SELECT
    e.actor_account_id,
    e.actor_is_bot,
    COUNT(*) AS npc_kills
FROM mod_player_stats_events e
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
GROUP BY e.actor_account_id, e.actor_is_bot
ORDER BY npc_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 3. Most-killed NPCs on the server
-- ---------------------------------------------------------------------------
SELECT
    e.target_entry AS creature_entry,
    ct.name AS creature_name,
    COUNT(*) AS times_killed
FROM mod_player_stats_events e
LEFT JOIN YOUR_WORLD_DB.creature_template ct ON ct.entry = e.target_entry
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
GROUP BY e.target_entry, ct.name
ORDER BY times_killed DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 4. Deadliest NPCs: creatures that killed players the most
-- ---------------------------------------------------------------------------
SELECT
    e.target_entry AS creature_entry,
    ct.name AS creature_name,
    COUNT(*) AS player_kills
FROM mod_player_stats_events e
LEFT JOIN YOUR_WORLD_DB.creature_template ct ON ct.entry = e.target_entry
WHERE e.event_type = 'PLAYER_KILLED_BY_CREATURE'
GROUP BY e.target_entry, ct.name
ORDER BY player_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 5. Characters with the most deaths to NPCs
-- This is a creature-cause detail query, not a comprehensive death total.
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS npc_deaths
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type = 'PLAYER_KILLED_BY_CREATURE'
GROUP BY e.actor_guid, c.name
ORDER BY npc_deaths DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 5a. Canonical player deaths since the contract cutover
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    e.actor_is_bot,
    COUNT(*) AS deaths_since_cutover
FROM mod_player_stats_events e
JOIN mod_player_stats_migrations m
    ON m.migration_key = 'canonical_player_death_v1'
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type = 'PLAYER_DEATH'
  AND e.id > m.cutoff_event_id
GROUP BY e.actor_guid, c.name, e.actor_is_bot
ORDER BY deaths_since_cutover DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 5b. Cutover-aware lifetime death totals
--
-- Before the cutoff, known NPC deaths are actor-owned while PvP deaths store
-- the victim in target fields. After the cutoff, only PLAYER_DEATH contributes
-- to totals; specialized rows remain detail facts and are intentionally omitted.
-- Historical environmental deaths cannot be reconstructed.
-- ---------------------------------------------------------------------------
WITH death_facts AS (
    SELECT
        e.actor_guid,
        e.actor_account_id,
        e.actor_is_bot
    FROM mod_player_stats_events e
    JOIN mod_player_stats_migrations m
        ON m.migration_key = 'canonical_player_death_v1'
    WHERE e.event_type = 'PLAYER_DEATH'
      AND e.id > m.cutoff_event_id

    UNION ALL

    SELECT
        e.actor_guid,
        e.actor_account_id,
        e.actor_is_bot
    FROM mod_player_stats_events e
    JOIN mod_player_stats_migrations m
        ON m.migration_key = 'canonical_player_death_v1'
    WHERE e.event_type = 'PLAYER_KILLED_BY_CREATURE'
      AND e.id <= m.cutoff_event_id

    UNION ALL

    SELECT
        e.target_guid AS actor_guid,
        CAST(e.value2 AS UNSIGNED) AS actor_account_id,
        e.target_is_bot AS actor_is_bot
    FROM mod_player_stats_events e
    JOIN mod_player_stats_migrations m
        ON m.migration_key = 'canonical_player_death_v1'
    WHERE e.event_type = 'PVP_KILL'
      AND e.id <= m.cutoff_event_id
)
SELECT
    d.actor_guid,
    c.name AS character_name,
    d.actor_is_bot,
    COUNT(*) AS known_lifetime_deaths
FROM death_facts d
LEFT JOIN characters c ON c.guid = d.actor_guid
GROUP BY d.actor_guid, c.name, d.actor_is_bot
ORDER BY known_lifetime_deaths DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 6. PvP leaderboard
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS pvp_kills
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type = 'PVP_KILL'
GROUP BY e.actor_guid, c.name
ORDER BY pvp_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 7. Most common PvP victims
-- ---------------------------------------------------------------------------
SELECT
    e.target_guid AS victim_guid,
    c.name AS victim_name,
    COUNT(*) AS times_killed
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.target_guid
WHERE e.event_type = 'PVP_KILL'
GROUP BY e.target_guid, c.name
ORDER BY times_killed DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 8. NPC kills per day
-- ---------------------------------------------------------------------------
SELECT
    DATE(e.event_time) AS day,
    COUNT(*) AS npc_kills
FROM mod_player_stats_events e
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
GROUP BY DATE(e.event_time)
ORDER BY day DESC;

-- ---------------------------------------------------------------------------
-- 9. Character activity timeline
-- ---------------------------------------------------------------------------
SELECT
    e.event_time,
    e.event_type,
    e.actor_is_bot,
    e.target_type,
    e.target_entry,
    e.target_guid,
    e.target_is_bot,
    e.value1,
    e.value2,
    e.map_id,
    e.zone_id,
    e.source
FROM mod_player_stats_events e
WHERE e.actor_guid = 12345
ORDER BY e.event_time DESC
LIMIT 200;

-- ---------------------------------------------------------------------------
-- 10. Quest completion leaderboard
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS quests_completed
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type = 'QUEST_COMPLETE'
GROUP BY e.actor_guid, c.name
ORDER BY quests_completed DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 11. Achievement leaderboard
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS achievements_earned
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type = 'ACHIEVEMENT'
GROUP BY e.actor_guid, c.name
ORDER BY achievements_earned DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 12. Top NPC kill for one specific character, grouped by NPC
-- ---------------------------------------------------------------------------
SELECT
    e.target_entry AS creature_entry,
    ct.name AS creature_name,
    COUNT(*) AS times_killed
FROM mod_player_stats_events e
LEFT JOIN YOUR_WORLD_DB.creature_template ct ON ct.entry = e.target_entry
WHERE e.actor_guid = 12345
  AND e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
GROUP BY e.target_entry, ct.name
ORDER BY times_killed DESC
LIMIT 25;


-- ---------------------------------------------------------------------------
-- 13. Human-only NPC kill leaderboard
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS npc_kills
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
  AND e.actor_is_bot = 0
GROUP BY e.actor_guid, c.name
ORDER BY npc_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 14. Bot-only NPC kill leaderboard
-- ---------------------------------------------------------------------------
SELECT
    e.actor_guid,
    c.name AS character_name,
    COUNT(*) AS npc_kills
FROM mod_player_stats_events e
LEFT JOIN characters c ON c.guid = e.actor_guid
WHERE e.event_type IN ('CREATURE_KILL', 'CREATURE_KILL_PET')
  AND e.actor_is_bot = 1
GROUP BY e.actor_guid, c.name
ORDER BY npc_kills DESC
LIMIT 25;

-- ---------------------------------------------------------------------------
-- 15. PvP kills split by human/bot killer and human/bot victim
-- ---------------------------------------------------------------------------
SELECT
    e.actor_is_bot AS killer_is_bot,
    e.target_is_bot AS victim_is_bot,
    COUNT(*) AS kills
FROM mod_player_stats_events e
WHERE e.event_type = 'PVP_KILL'
GROUP BY e.actor_is_bot, e.target_is_bot
ORDER BY kills DESC;
