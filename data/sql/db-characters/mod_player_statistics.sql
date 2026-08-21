CREATE TABLE IF NOT EXISTS `mod_player_stats_events` (
    `id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
    `event_time` TIMESTAMP(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
    `realm_id` INT UNSIGNED NOT NULL DEFAULT 1,
    `event_type` VARCHAR(32) NOT NULL,

    `actor_account_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `actor_guid` INT UNSIGNED NOT NULL DEFAULT 0,
    `actor_is_bot` TINYINT(1) UNSIGNED NOT NULL DEFAULT 0,
    `actor_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `actor_class` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `actor_race` TINYINT UNSIGNED NOT NULL DEFAULT 0,

    `target_type` TINYINT UNSIGNED NOT NULL DEFAULT 0,
    `target_entry` INT UNSIGNED NOT NULL DEFAULT 0,
    `target_guid` BIGINT UNSIGNED NOT NULL DEFAULT 0,
    `target_is_bot` TINYINT(1) UNSIGNED NOT NULL DEFAULT 0,

    `value1` BIGINT SIGNED NOT NULL DEFAULT 0,
    `value2` BIGINT SIGNED NOT NULL DEFAULT 0,

    `map_id` SMALLINT UNSIGNED NOT NULL DEFAULT 0,
    `instance_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `zone_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `area_id` INT UNSIGNED NOT NULL DEFAULT 0,
    `source` VARCHAR(24) NOT NULL DEFAULT '',

    PRIMARY KEY (`id`),
    KEY `idx_event_time` (`event_time`),
    KEY `idx_event_type_time` (`event_type`, `event_time`),
    KEY `idx_actor_event_time` (`actor_guid`, `event_type`, `event_time`),
    KEY `idx_bot_event_time` (`actor_is_bot`, `event_type`, `event_time`),
    KEY `idx_account_event_time` (`actor_account_id`, `event_type`, `event_time`),
    KEY `idx_target_event_time` (`target_entry`, `event_type`, `event_time`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci;
