-- NovaCore: mod-warmode - karo režimo būsena
CREATE TABLE IF NOT EXISTS `character_warmode` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `enabled` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '1 - karo režimas įjungtas',
  `changed_at` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'paskutinio pakeitimo laikas (unix)',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-warmode būsena';
