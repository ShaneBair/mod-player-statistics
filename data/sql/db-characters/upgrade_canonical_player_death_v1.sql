-- Apply only while worldserver is stopped so the event-ID cutoff is unambiguous.
CREATE TABLE IF NOT EXISTS `mod_player_stats_migrations` (
    `migration_key` VARCHAR(64) NOT NULL,
    `cutoff_event_id` BIGINT UNSIGNED NOT NULL,
    `applied_at` TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    PRIMARY KEY (`migration_key`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;

-- INSERT IGNORE preserves both the original cutoff and applied_at on reruns.
INSERT IGNORE INTO `mod_player_stats_migrations`
    (`migration_key`, `cutoff_event_id`)
SELECT
    'canonical_player_death_v1',
    COALESCE(MAX(`id`), 0)
FROM `mod_player_stats_events`;

-- If a previous fresh-schema import already created this migration row with a
-- zero cutoff on a populated event table, apply the separately guarded repair:
-- repair_canonical_player_death_v1_zero_cutoff.sql
