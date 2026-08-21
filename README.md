# mod-player-statistics

A generalized AzerothCore event logger intended to feed a player/server statistics website.

Instead of maintaining a separate table for every statistic, the module writes normalized player-centric events to one append-only table in the **characters database**. Your website can aggregate those events into leaderboards, timelines, records, and server-wide totals.

## What it logs

Enabled by default:

| Event type | Meaning | Useful dashboard stats |
|---|---|---|
| `CREATURE_KILL` | Player directly killed a creature | NPC kills, favorite victim, total kills |
| `CREATURE_KILL_PET` | Player's pet killed a creature | Same, while attributing the kill to the owner |
| `PLAYER_KILLED_BY_CREATURE` | Creature killed a player | Deadliest NPCs, player deaths |
| `PVP_KILL` | Player killed another player | PvP leaderboard, rivalries |
| `LEVEL_CHANGE` | Character changed level | Level history, leveling timeline |
| `QUEST_COMPLETE` | Character completed a quest | Quest totals, activity |
| `ACHIEVEMENT` | Character earned an achievement | Achievement totals, recent milestones |

Optional high-volume events, disabled by default:

- `LOOT_ITEM`
- `XP_GAIN`
- `MONEY_CHANGE`

## Event model

The **actor is always the player whose statistics are being recorded**. Every event also stores `actor_is_bot`, allowing the website to include or exclude Playerbot activity without losing the underlying data. Player-vs-player events also populate `target_is_bot`.

For example:

- A player kills Hogger: actor = player, target = Hogger creature entry.
- Hogger kills a player: actor = player who died, target = Hogger creature entry.
- A hunter pet kills a wolf: actor = hunter, target = wolf, source = `pet`.
- A player kills another player: actor = killer, target GUID = victim.

`value1` and `value2` are generic event-specific numeric payloads:

| Event | value1 | value2 |
|---|---:|---:|
| `CREATURE_KILL` | creature level | 0 |
| `CREATURE_KILL_PET` | creature level | 0 |
| `PLAYER_KILLED_BY_CREATURE` | creature level | 0 |
| `PVP_KILL` | victim level | victim account ID |
| `LEVEL_CHANGE` | old level | new level |
| `LOOT_ITEM` | item count | 0 |
| `XP_GAIN` | XP amount | AzerothCore XP source enum value |
| `MONEY_CHANGE` | copper delta | resulting copper balance |

## Installation

1. Copy this folder to your AzerothCore source tree:

   ```text
   azerothcore-wotlk/modules/mod-player-statistics
   ```

2. Import the character-database schema:

   ```text
   data/sql/db-characters/mod_player_statistics.sql
   ```

   If your normal AzerothCore/module database assembler picks up module SQL, you can use that instead. The important part is that `mod_player_stats_events` exists in the **characters DB** before starting the server.

3. Reconfigure/rebuild AzerothCore exactly as you do for your other compiled modules.

4. Copy/merge `conf/mod_player_statistics.conf.dist` into your module config setup. AzerothCore's normal module config handling should load the `.conf` generated from the `.conf.dist` file.

5. Start `worldserver` and kill a creature with a character.

6. Verify:

   ```sql
   SELECT *
   FROM mod_player_stats_events
   ORDER BY id DESC
   LIMIT 20;
   ```

You should see a `CREATURE_KILL` row immediately after a normal player creature kill.

### Upgrading from the first module version

If you already created `mod_player_stats_events` using the original schema, run:

```text
data/sql/db-characters/upgrade_add_bot_flags.sql
```

Do not run that upgrade script on a fresh install because the main schema already contains both bot columns. Existing historical rows default to `actor_is_bot = 0`; only newly logged events can be reliably classified automatically.

## Website/database design notes

The event table deliberately stores IDs rather than copying display names into every row:

- `actor_guid` joins to the characters DB `characters.guid`.
- `actor_account_id` lets you aggregate across all characters on one account.
- `actor_is_bot` is `1` when the actor is controlled by AzerothCore Playerbots and `0` for a human-controlled session.
- `target_is_bot` is populated for player targets such as PvP victims.
- `target_entry` for creature events joins to the world DB `creature_template.entry`.
- `target_entry` for quest/item/achievement events is their corresponding entry/ID.
- `map_id`, `zone_id`, `area_id`, and `instance_id` allow geographic and dungeon/raid statistics later.

This keeps the logging path simple and allows character/NPC names to change without rewriting historical data.

See `examples/dashboard_queries.sql` for ready-to-use examples.

## Recommended starting configuration

Keep the seven default events enabled and leave loot/XP/money disabled at first. The default events are relatively low-volume and cover most of the fun statistics we discussed. XP in particular can create a very large event table because it can fire constantly while players level.

## Playerbots

Playerbot activity is intentionally logged. The module uses the Playerbots AzerothCore fork's `WorldSession::IsBot()` flag, so both altbots and random bots are tagged without relying on account naming conventions or hard-coded account ranges.

- `actor_is_bot = 0`: human-controlled character
- `actor_is_bot = 1`: Playerbot-controlled character
- `target_is_bot`: identifies bot victims/targets when the target is a Player, currently most useful for `PVP_KILL`

This means the website can show combined server totals while still offering Human Only and Bots Only views. Example filters are included in `examples/dashboard_queries.sql`.

This implementation targets the custom AzerothCore Playerbot branch used by `mod-playerbots`, because `WorldSession::IsBot()` is part of that Playerbot core.

## Why damage is not logged here

Do **not** write one SQL row per damage hit for a statistics site. Combat damage is orders of magnitude noisier than kills/quests/levels and would turn this compact event log into a combat-log database.

If you want damage done/healing done later, add an in-memory accumulator and periodically flush rollups such as:

```text
(character_guid, date/hour, damage_done, healing_done, damage_taken)
```

That gives you the fun dashboard numbers without millions of per-swing rows.
