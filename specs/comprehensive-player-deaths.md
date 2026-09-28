# Comprehensive Player Death Events

Status: Implemented; deployment verification pending  
Repository: `mod-player-statistics`  
Consumer: `wow-portal/specs/comprehensive-player-deaths.md`  
Contract version: `PLAYER_DEATH` plus migration key `canonical_player_death_v1`

## Problem

The module currently records a death only when one of two specialized hooks fires:

- `PLAYER_KILLED_BY_CREATURE` records a creature killing a player.
- `PVP_KILL` records a player killing another player, with the victim represented as the event target.

Those events do not cover falling, drowning, fatigue, lava, fire, falling into the void, scripted/environmental damage, suicide, or other deaths that are neither a creature kill nor a normal PvP kill. The portal's current Most Deaths leaderboard is therefore incomplete.

## User Outcome

From the deployment of this feature onward, every actual player death produces exactly one canonical event regardless of cause. Human-controlled and Playerbot-controlled deaths remain distinguishable using the control mode at the moment of death.

Existing known creature and PvP death history remains usable through an explicit cutover record. Historical environmental deaths cannot be recovered and are not invented.

## Current Behavior

- `PlayerStatisticsPlayerScript` subscribes to `PLAYERHOOK_ON_PLAYER_KILLED_BY_CREATURE` and `PLAYERHOOK_ON_PVP_KILL`.
- It does not subscribe to `PLAYERHOOK_ON_PLAYER_JUST_DIED`.
- The event table is append-only and has no schema/contract migration metadata.
- Specialized death events are enabled by default.
- `actor_is_bot` and `target_is_bot` use `WorldSession::IsHeadless()`.
- Location and actor metadata are captured by the shared `LogEvent` function.

## Accepted Design

Add one canonical event type:

```text
PLAYER_DEATH
```

Emit it from the deployed Playerbots core's equivalent of:

```cpp
PLAYERHOOK_ON_PLAYER_JUST_DIED
PlayerScript::OnPlayerJustDied(Player* player)
```

The upstream AzerothCore hook is documented as running when a player dies, but implementation must verify the hook name, signature, registration constant, and firing behavior against the exact Dad's MMO Lab Playerbots core revision before changing the module.

`PLAYER_DEATH` becomes the sole canonical event for total-death aggregation after the documented cutover. Specialized events remain enabled for cause- and killer-oriented features but are never added to canonical rows after the cutover when calculating total deaths.

## Event Contract

For every `PLAYER_DEATH` row:

| Field | Meaning |
|---|---|
| `event_type` | `PLAYER_DEATH` |
| `actor_account_id` | Account ID of the dead character |
| `actor_guid` | GUID counter of the dead character |
| `actor_is_bot` | `WorldSession::IsHeadless()` for the dead character at death time |
| `actor_level` | Dead character's level at death time |
| `actor_class` | Dead character's class at death time |
| `actor_race` | Dead character's race at death time |
| `target_type` | `None` (`0`) in version 1 |
| `target_entry` | `0` |
| `target_guid` | `0` |
| `target_is_bot` | `0` |
| `value1` | `0`; reserved for a future versioned death-cause code |
| `value2` | `0`; reserved |
| `map_id` | Dead character's map at death time |
| `instance_id` | Dead character's instance at death time |
| `zone_id` | Dead character's zone at death time |
| `area_id` | Dead character's area at death time |
| `source` | `canonical` |
| `event_time` | Database-assigned death event time |

The actor is always the character who died. Do not put a killer in actor fields.

The version 1 generic hook does not provide a trustworthy killer or environmental cause. Do not infer one from nearby events, combat state, location, health history, account type, or timing. The absence of a cause is explicit and preferable to false classification.

## Exactly-Once Semantics

- Emit one `PLAYER_DEATH` for each real transition to player death handled by the canonical hook.
- Creature deaths produce both one `PLAYER_DEATH` and the existing `PLAYER_KILLED_BY_CREATURE` detail event.
- PvP deaths produce both one victim-owned `PLAYER_DEATH` and the existing killer-owned `PVP_KILL` detail event.
- Environmental and otherwise unclassified deaths produce `PLAYER_DEATH` even when no specialized event exists.
- Feign Death, duel defeat without actual death, taking nonlethal environmental damage, releasing spirit, resurrection, corpse creation/loading, and logging in while dead must not create `PLAYER_DEATH`.
- Repeated hook delivery for one death is a contract violation and must be caught during target-core verification rather than hidden with a time-based guess.

Do not implement in-memory timestamp deduplication unless target-core testing proves duplicate delivery and the specification is revised with an exact identity rule. A heuristic window could incorrectly collapse two legitimate rapid deaths.

## Configuration

Add a default-enabled setting:

```text
PlayerStatistics.Events.PlayerDeaths = 1
```

Requirements:

- Load it with the other core event settings.
- Document it in `conf/mod_player_statistics.conf.dist`.
- The master `PlayerStatistics.Enable` switch still controls it.
- `PlayerStatistics.IgnoreGameMasters` applies through `ShouldLog` exactly as it does to other events.
- Bot activity remains stored and is filtered only by consumers.
- Disabling specialized creature/PvP detail events must not disable canonical deaths.
- Disabling canonical deaths creates incomplete total statistics and must be clearly documented.

## Cutover Metadata

Preserve legacy known death totals without mixing them with canonical rows after deployment.

Add this small metadata table to the fresh-install schema:

```sql
CREATE TABLE IF NOT EXISTS `mod_player_stats_migrations` (
    `migration_key` VARCHAR(64) NOT NULL,
    `cutoff_event_id` BIGINT UNSIGNED NOT NULL,
    `applied_at` TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (`migration_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
```

Fresh installs insert:

```text
migration_key   = canonical_player_death_v1
cutoff_event_id = 0
applied_at      = schema installation time
```

The fresh-install insert is conditional on `mod_player_stats_events` being empty. If the fresh schema is accidentally applied to an existing populated event table, it must leave the migration row absent so the existing-install migration can capture the correct maximum ID.

Provide a separate idempotent upgrade SQL file for existing installations. It must:

1. create `mod_player_stats_migrations` if absent;
2. insert exactly one `canonical_player_death_v1` row;
3. capture `COALESCE(MAX(mod_player_stats_events.id), 0)` as `cutoff_event_id`;
4. preserve the original row unchanged on every subsequent run;
5. set `applied_at` when the row is first created.

The upgrade is run only while the worldserver is stopped. This makes the cutoff unambiguous:

- specialized creature/PvP death rows with `id <= cutoff_event_id` are eligible legacy totals;
- `PLAYER_DEATH` rows with `id > cutoff_event_id` are comprehensive totals;
- specialized rows with `id > cutoff_event_id` remain detail facts and are not totalled.

Do not insert synthetic `PLAYER_DEATH` rows for historical events. Environmental history is unknowable, and transforming historical PvP target rows into actor rows would require inferred metadata. The hybrid consumer query provides continuity without mutating durable facts.

## Upgrade Ordering

For an existing server, the safe order is mandatory:

1. Stop the worldserver cleanly.
2. Back up the characters database according to the operator's normal process.
3. Apply the idempotent cutover migration while no new event rows can be written.
4. Build and deploy the module version that emits `PLAYER_DEATH`.
5. Start the worldserver and verify the module/configuration loaded.
6. Produce controlled deaths and verify canonical rows.
7. Deploy the compatible portal query only after module verification.

If the server cannot be stopped, do not guess a cutoff and do not deploy this contract.

### Recovery from a pre-seeded zero cutoff

An older fresh-install schema could insert `canonical_player_death_v1` with cutoff zero on a populated event table. The existing-install migration then preserved that row because its idempotence contract used `INSERT IGNORE`. Consumers consequently excluded every legacy specialized death even though the event rows remained intact.

The guarded recovery migration `data/sql/db-characters/repair_canonical_player_death_v1_zero_cutoff.sql` derives a candidate cutoff as the greatest event ID whose `event_time` is no later than the migration row's original `applied_at`. It updates the row only when:

- the current cutoff is zero;
- the candidate is nonzero; and
- no canonical `PLAYER_DEATH` row has an ID at or below the candidate.

Run the recovery only with `worldserver` stopped and after backing up the characters database. It is idempotent. A genuine fresh install has no event at or before `applied_at` and remains at zero. A database containing canonical events at or below the inferred boundary is ambiguous and remains unchanged for manual review.

## Schema and Index Impact

- Do not alter existing event rows or columns.
- Do not add a per-death table.
- The metadata table contains one row for this migration and negligible volume.
- Start with existing event-table indexes, including event type/time and actor/event/time.
- Run the portal's final hybrid query through `EXPLAIN` against representative data.
- Add an event index only through a separate additive migration supported by measured query behavior. Avoid increasing write cost speculatively.

## Existing Specialized Events

Retain current contracts:

- `PLAYER_KILLED_BY_CREATURE`: actor is the dead player; target is the creature.
- `PVP_KILL`: actor is the killer; target is the dead player.

They remain useful for deadliest-creature, PvP kill, rivalry, and future cause-detail features. Documentation and example queries must prominently state that total deaths use the cutover-aware canonical contract rather than blindly unioning all death-shaped events.

## Environmental Coverage

`OnPlayerJustDied` is expected to include actual deaths resulting from:

- falling;
- falling into the void;
- drowning;
- fatigue;
- lava;
- fire/environmental spell damage;
- other scripted or unclassified causes that lead to a real player death.

This must be verified in the exact deployed core. If any real death path does not fire the canonical hook, implementation stops and documents the target-core gap rather than adding damage-threshold heuristics.

Version 1 includes these deaths in totals but does not label their exact cause. Exact labels require a separate contract because the generic hook receives only the dead player. If the owner later wants cause breakdowns, first inspect the deployed core for an environmental-damage hook carrying `EnvironmentalDamageType`; otherwise specify a narrowly scoped core hook before adding event semantics.

## Documentation Updates Required During Implementation

Update together:

- `README.md` event table and event-model explanation;
- the `value1`/`value2` contract table;
- `conf/mod_player_statistics.conf.dist`;
- `data/sql/db-characters/mod_player_statistics.sql`;
- the new idempotent upgrade SQL file;
- `examples/dashboard_queries.sql` with canonical and cutover-aware examples;
- repository `AGENTS.md` current event list after implementation;
- sibling portal specification/status if implementation uncovers a contract change.

Remove or correct the README reference to the missing bot-flag upgrade file while touching upgrade documentation; do not claim an upgrade artifact exists when it does not.

## Security and Privacy

- Do not store character names, account logins, credentials, IP addresses, coordinates, or chat/combat text in death events.
- Use existing numeric identifiers and coarse location IDs.
- `actor_is_bot` uses only `WorldSession::IsHeadless()`.
- Do not log complete event rows during normal operation.
- Database writes continue through the normal asynchronous characters-database worker path.
- The migration is additive and must not delete, rewrite, or backfill existing event rows.

## Out of Scope

- Exact environmental cause labels.
- Killer attribution on `PLAYER_DEATH`.
- Damage amount, spell, aura, or combat-log capture.
- Historical environmental-death reconstruction.
- Portal UI/API implementation.
- Changing Playerbots detection.
- Per-hit damage logging.
- Core patches unless target-core verification proves the canonical death hook is absent or incomplete and a new specification is approved.

## Acceptance Criteria

- The exact Dad's MMO Lab Playerbots core exposes and successfully compiles the canonical player-death hook.
- Each tested real death produces exactly one `PLAYER_DEATH` row.
- Creature and PvP deaths are not double-counted by the canonical event itself.
- Falling, void, drowning, fatigue, lava/fire, and at least one other unclassified death path produce canonical rows where those paths are practically testable.
- Nonlethal environmental damage produces no row.
- Feign Death, duel defeat, release, resurrection, relog while dead, and repeated save/load produce no row.
- The dead character owns actor fields and location.
- Human, human GM, altbot, and random-bot control flags follow the accepted settings and `IsHeadless()` behavior.
- `PlayerStatistics.Events.PlayerDeaths` independently enables/disables the event.
- Fresh schema creates the cutover metadata with cutoff zero only when the event table is empty.
- Fresh schema leaves the migration row absent when the event table is already populated.
- Existing-install migration captures the current maximum event ID once and is idempotent.
- The zero-cutoff recovery repairs only an unambiguous pre-seeded row and is idempotent.
- The zero-cutoff recovery leaves legitimate fresh installs and ambiguous boundaries unchanged.
- No existing event row is changed or deleted.
- Existing specialized events continue to work.
- Documentation and example queries explain canonical totals and the legacy cutoff.

## Verification Plan

### Static and Build Verification

1. Confirm the hook declaration and call site in the exact target core.
2. Review event constant, hook registration, configuration key, field semantics, and SQL names for consistency.
3. Compile the module inside that core with warnings treated according to the normal Dad's MMO Lab build.
4. Apply fresh and upgrade SQL to disposable databases.
5. Run the upgrade twice and confirm cutoff ID and applied timestamp do not change.
6. Apply the fresh schema to a populated disposable event table and confirm it does not seed a zero-cutoff migration row.
7. Verify the recovery migration repairs a pre-seeded zero cutoff, remains unchanged on rerun, and refuses both a legitimate fresh-install zero and a boundary containing canonical rows.

### In-Game Verification

For every scenario, compare event counts immediately before and after:

1. Human killed by a creature.
2. Human killed by another player.
3. Human dies from falling.
4. Human dies from drowning.
5. Human dies from fatigue.
6. Human dies from lava or environmental fire.
7. Human falls into the void where safely reproducible.
8. Human takes nonlethal environmental damage.
9. Feign Death and duel defeat.
10. Release, resurrect, log out/in while dead.
11. Human-controlled GM with `IgnoreGameMasters` off and on.
12. Altbot and random bot deaths.

Confirm each real death adds one canonical row and that expected specialized rows may coexist without changing canonical count.

### Operational Cutover Verification

1. Stop the worldserver and record the current maximum event ID through the migration.
2. Confirm the stored cutoff matches that maximum without printing raw event data.
3. Start the new module and cause one creature and one environmental death.
4. Confirm both new canonical IDs are greater than the cutoff.
5. Confirm rerunning the migration does not advance the cutoff.
6. Hand the verified cutoff contract to the portal implementation.

## Follow-Up: Exact Cause Classification

Create a separate specification only after inspecting the target core. It must define a stable cause enum, the authoritative hook carrying that enum, ordering relative to `OnPlayerJustDied`, handling of lethal versus absorbed environmental damage, and compatibility with the canonical total. Do not overload `value1` or `source` until that contract is approved.
