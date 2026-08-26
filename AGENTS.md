# mod-player-statistics Guidance

## Repository Role

This repository is an AzerothCore WotLK C++ module that records normalized, player-centric events for later use by the sibling `wow-portal` project. It is intended for the custom Playerbots-compatible AzerothCore server operated with Dad's MMO Lab scripts.

This repository is a standalone Git root. The parent workspace file may not be loaded when Codex starts here, so the essential shared constraints are repeated below.

## Current Layout

- `src/mod_player_statistics.cpp` contains configuration loading, filtering, event insertion, and AzerothCore player hooks.
- `src/mod_player_statistics_loader.cpp` registers the module script.
- `conf/mod_player_statistics.conf.dist` defines the supported module settings and safe defaults.
- `data/sql/db-characters/mod_player_statistics.sql` creates the event table in the characters database.
- `data/sql/db-characters/upgrade_canonical_player_death_v1.sql` records the immutable death-contract cutover for existing installations.
- `examples/dashboard_queries.sql` demonstrates intended aggregation and joins.
- `README.md` documents event meanings, installation, Playerbot behavior, and design rationale.

There is no standalone CMake project or test harness here. The module must be placed in a compatible AzerothCore source tree and built with that core.

## Event Contract

`mod_player_stats_events` is an append-only fact table in the AzerothCore characters database.

- The actor is always the player whose statistics are being recorded.
- `actor_guid` is the character GUID counter; `actor_account_id` supports account-level aggregation.
- `actor_is_bot` and, where applicable, `target_is_bot` use the Playerbots core's `WorldSession::IsBot()` result. Do not replace this with account-name or account-ID heuristics.
- Creature targets use `target_entry` for `creature_template.entry`; quest, item, and achievement targets use their corresponding entry or ID.
- Player targets use `target_guid`; PvP `value1` is victim level and `value2` is victim account ID.
- `PLAYER_DEATH` is victim-owned, has no target or numeric payload in version 1, and uses source `canonical`.
- Location fields capture the actor's map, instance, zone, and area when the hook runs.
- `event_time` is assigned by the database with millisecond precision.

Current event types are:

- Default: `CREATURE_KILL`, `CREATURE_KILL_PET`, `PLAYER_DEATH`, `PLAYER_KILLED_BY_CREATURE`, `PVP_KILL`, `LEVEL_CHANGE`, `QUEST_COMPLETE`, and `ACHIEVEMENT`.
- Optional high-volume: `LOOT_ITEM`, `XP_GAIN`, and `MONEY_CHANGE`.

The precise `value1`, `value2`, target, and source meanings are part of the public integration contract. Update the schema, README, configuration, examples, and sibling portal specification together when changing them.

## Implementation Rules

- Follow the existing AzerothCore script-hook style and APIs used by the target Playerbots-compatible core. Verify hook signatures against that exact core when available.
- Keep gameplay hooks lightweight. Event writes currently use the characters database worker pool through `CharacterDatabase.Execute`.
- Only interpolate module-owned constants into SQL. Any future player-controlled or external text must use a safe prepared/escaped database mechanism supported by AzerothCore.
- Call `ShouldLog` consistently so the master switch and GM exclusion policy apply to every event.
- Preserve human and bot activity in storage. Filtering belongs in queries or presentation unless a specification explicitly changes collection policy.
- Keep noisy event types off by default. Never store one row per damage or healing hit; use bounded in-memory aggregation and periodic rollups for high-frequency combat totals.
- Keep configuration names under `PlayerStatistics.*`, document new options in `.conf.dist`, and choose conservative defaults.
- Avoid synchronous database reads or writes in hot gameplay hooks.
- Use fixed-width AzerothCore types consistently and consider overflow, negative deltas, duplicate hooks, pets, groups, instances, and disconnecting sessions.

## Database Changes

- Treat existing rows as durable history. Prefer additive migrations and backward-compatible semantics.
- New columns require indexes only when supported by real query patterns; every index increases event-write cost.
- New or changed event payloads require a migration/versioning decision so the portal can distinguish old and new rows.
- Put fresh-install SQL under `data/sql/db-characters/` and provide a separate, idempotent upgrade path when existing installations need alteration.
- Do not apply SQL to a live database without explicit authorization.

## Specifications

Place module work in `specs/<feature-name>.md`. A module specification should define:

- the AzerothCore hook and when it fires;
- actor, target, `value1`, `value2`, source, bot, and location semantics;
- volume expectations and enablement default;
- schema/index/migration impact;
- duplicate and edge-case behavior;
- portal consumption impact;
- compile, SQL, and in-game acceptance checks.

Read only the specification relevant to the active task unless it explicitly depends on another.

## Verification

For every change, perform all checks available in the environment and state what could not be run:

1. Review configuration keys, SQL columns, event constants, and documentation for consistency.
2. Compile the module within the target AzerothCore Playerbots source tree.
3. Apply fresh-install or upgrade SQL to a disposable/test characters database.
4. Start `worldserver` and confirm the module loads without configuration or SQL errors.
5. Trigger the affected event in game and inspect the resulting row, including bot and location fields.
6. Run representative queries and check that event volume and query plans remain reasonable.

Do not claim compile or gameplay verification when the external AzerothCore environment is unavailable.

## Cross-Repository Boundary

The sibling `wow-portal` repository will consume these events for statistics. It must not infer undocumented payload meanings. When an implementation changes the event contract, call out the required portal work explicitly; do not edit the sibling repository unless the active task includes it.
