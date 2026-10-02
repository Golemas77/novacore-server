-- NovaCore: mod-hardcore - hardkoro (viena gyvybe) rezimo busena
CREATE TABLE IF NOT EXISTS `character_hardcore` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `status` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '0 - nepradeta, 1 - aktyvus, 2 - zuvo (rezimas panaikintas)',
  `start_time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'kada pradeta (unix laikas)',
  `start_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'kokiame lygyje pradeta',
  `end_time` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'kada zuvo (unix laikas)',
  `end_level` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'kokiame lygyje zuvo',
  `end_map` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'zemelapio ID mirties metu',
  `end_zone` SMALLINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'zonos ID mirties metu',
  `failures` TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT 'kiek kartu hardkoras buvo panaikintas mirtimi',
  `killer` VARCHAR(100) NOT NULL DEFAULT '' COMMENT 'zudikas (tuscia - aplinka)',
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-hardcore busena';
