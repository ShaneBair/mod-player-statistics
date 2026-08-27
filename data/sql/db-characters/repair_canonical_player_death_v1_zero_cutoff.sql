-- Repair only while worldserver is stopped so event_time and event IDs are stable.
--
-- This targets the specific state caused by applying the fresh-install schema to
-- an existing event table before the canonical-death upgrade migration:
--   * the migration row exists with cutoff_event_id = 0;
--   * legacy events existed when that row was created; and
--   * no canonical PLAYER_DEATH event existed at or before that creation time.
--
-- A legitimate fresh install remains at cutoff zero because it had no events at
-- or before applied_at. Ambiguous databases remain unchanged for manual review.

SET @canonical_death_repair_candidate := (
    SELECT COALESCE(MAX(`events`.`id`), 0)
    FROM `mod_player_stats_events` AS `events`
    INNER JOIN `mod_player_stats_migrations` AS `migration`
        ON `migration`.`migration_key` = 'canonical_player_death_v1'
    WHERE `events`.`event_time` <= `migration`.`applied_at`
);

UPDATE `mod_player_stats_migrations` AS `migration`
SET `migration`.`cutoff_event_id` = @canonical_death_repair_candidate
WHERE `migration`.`migration_key` = 'canonical_player_death_v1'
  AND `migration`.`cutoff_event_id` = 0
  AND @canonical_death_repair_candidate > 0
  AND NOT EXISTS (
      SELECT 1
      FROM `mod_player_stats_events` AS `canonical_event`
      WHERE `canonical_event`.`event_type` = 'PLAYER_DEATH'
        AND `canonical_event`.`id` <= @canonical_death_repair_candidate
  );

SELECT
    `migration_key`,
    `cutoff_event_id`,
    `applied_at`
FROM `mod_player_stats_migrations`
WHERE `migration_key` = 'canonical_player_death_v1';

SET @canonical_death_repair_candidate := NULL;
