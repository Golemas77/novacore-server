-- NovaCore: mod-transmog - atrakintos islvaizdos (visai paskyrai) ir pasirinkimai (lizdui)
CREATE TABLE IF NOT EXISTS `account_transmog_collection` (
  `account_id` INT UNSIGNED NOT NULL COMMENT 'account.id',
  `item_entry` INT UNSIGNED NOT NULL COMMENT 'item_template.entry, kurio islvaizda atrakinta',
  PRIMARY KEY (`account_id`, `item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-transmog: paskyros islvaizdu kolekcija';

CREATE TABLE IF NOT EXISTS `character_transmog` (
  `guid` INT UNSIGNED NOT NULL COMMENT 'characters.guid',
  `slot` TINYINT UNSIGNED NOT NULL COMMENT 'EQUIPMENT_SLOT_*',
  `item_entry` INT UNSIGNED NOT NULL COMMENT 'daiktas, kurio islvaizda rodoma siame lizde',
  PRIMARY KEY (`guid`, `slot`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_unicode_ci COMMENT='mod-transmog: pasirinktos islvaizdos';
