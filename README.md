# mod-player-statistics

A generalized AzerothCore event logger intended to feed a player/server statistics website.

Instead of maintaining a separate table for every statistic, the module writes normalized player-centric events to one append-only table in the **characters database**. Your website can aggregate those events into leaderboards, timelines, records, and server-wide totals.

The module also exposes a live, machine-readable roster of human-controlled characters through the worldserver console and SOAP:

```text
playerstats online
```

The command emits one `PLAYERSTATS_ONLINE_V1 ` line followed by compact JSON. It reads current in-memory sessions, includes human-controlled GMs, filters Playerbots with `WorldSession::IsBot()`, and never uses historical events or `characters.online` to infer current control. In-game invocations are rejected even when the account has command permission. The command uses the existing read-only `server info` RBAC permission.

## What it logs

Enabled by default:

| Event type | Meaning | Useful dashboard stats |
|---|---|---|
| `CREATURE_KILL` | Player directly killed a creature | NPC kills, favorite victim, total kills |
| `CREATURE_KILL_PET` | Player's pet killed a creature | Same, while attributing the kill to the owner |
| `PLAYER_DEATH` | Player died from any cause; actor is the dead player | Canonical total deaths |
| `PLAYER_KILLED_BY_CREATURE` | Creature killed a player | Deadliest NPCs, NPC death details |
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
- A player dies from any cause: `PLAYER_DEATH` actor = player who died, target = none, source = `canonical`.
- A hunter pet kills a wolf: actor = hunter, target = wolf, source = `pet`.
- A player kills another player: actor = killer, target GUID = victim.

`value1` and `value2` are generic event-specific numeric payloads:

| Event | value1 | value2 |
|---|---:|---:|
| `CREATURE_KILL` | creature level | 0 |
| `CREATURE_KILL_PET` | creature level | 0 |
| `PLAYER_DEATH` | 0 (reserved for a future versioned cause code) | 0 (reserved) |
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

   If your normal AzerothCore/module database assembler picks up module SQL, you can use that instead. The important part is that both `mod_player_stats_events` and `mod_player_stats_migrations` exist in the **characters DB** before starting the server. A fresh install records `canonical_player_death_v1` with cutoff event ID `0`.

3. Reconfigure/rebuild AzerothCore exactly as you do for your other compiled modules.

4. Copy/merge `conf/mod_player_statistics.conf.dist` into your module config setup. AzerothCore's normal module config handling should load the `.conf` generated from the `.conf.dist` file.

5. Start `worldserver`, kill a creature with a character, and produce one controlled player death.

6. Verify:

   ```sql
   SELECT *
   FROM mod_player_stats_events
   ORDER BY id DESC
   LIMIT 20;
   ```

You should see a `CREATURE_KILL` row after the creature kill and exactly one `PLAYER_DEATH` row owned by the character who died.

### Upgrading an existing installation for canonical deaths

Use the additive, idempotent migration:

```text
data/sql/db-characters/upgrade_canonical_player_death_v1.sql
```

The upgrade must be performed in this order:

1. Stop `worldserver` cleanly and back up the characters database.
2. Apply the upgrade SQL while no event rows can be written.
3. Build and deploy this module version.
4. Start `worldserver`, confirm configuration and SQL load cleanly, and verify controlled deaths.
5. Deploy the compatible portal query only after module verification.

The migration captures the current maximum event ID once. Rerunning it preserves the original cutoff and timestamp. Do not run it while `worldserver` is active, do not advance the cutoff manually, and do not synthesize historical `PLAYER_DEATH` rows.

This checkout does not include the previously documented `upgrade_add_bot_flags.sql`. Installations old enough to lack `actor_is_bot` or `target_is_bot` must reconcile those columns with the current fresh-install schema separately before deploying this version.

## Website/database design notes

The event table deliberately stores IDs rather than copying display names into every row:

- `actor_guid` joins to the characters DB `characters.guid`.
- `actor_account_id` lets you aggregate across all characters on one account.
- `actor_is_bot` is `1` when the actor is controlled by AzerothCore Playerbots and `0` for a human-controlled session.
- `target_is_bot` is populated for player targets such as PvP victims.
- `target_entry` for creature events joins to the world DB `creature_template.entry`.
- `target_entry` for quest/item/achievement events is their corresponding entry/ID.
- `map_id`, `zone_id`, `area_id`, and `instance_id` allow geographic and dungeon/raid statistics later.
- `mod_player_stats_migrations` records immutable event-contract cutovers used by consumers.

This keeps the logging path simple and allows character/NPC names to change without rewriting historical data.

For total deaths, use the `canonical_player_death_v1` cutoff: count legacy `PLAYER_KILLED_BY_CREATURE` and `PVP_KILL` facts only at or below the cutoff, then count only victim-owned `PLAYER_DEATH` facts above it. Specialized death events above the cutoff remain detail facts and must not be added to total deaths. Historical environmental deaths are unknowable and are intentionally not reconstructed.

`PLAYER_DEATH` version 1 has no killer or exact cause. Its target fields, `target_is_bot`, `value1`, and `value2` are zero, and its source is `canonical`. Disabling `PlayerStatistics.Events.PlayerDeaths` makes total-death statistics incomplete; it is independent of the creature- and PvP-detail settings.

See `examples/dashboard_queries.sql` for canonical-only and legacy-cutover-aware examples.

## Recommended starting configuration

Keep the eight default events enabled and leave loot/XP/money disabled at first. The default events are relatively low-volume and cover most of the fun statistics we discussed. Keep canonical player deaths enabled for complete totals. XP in particular can create a very large event table because it can fire constantly while players level.

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
