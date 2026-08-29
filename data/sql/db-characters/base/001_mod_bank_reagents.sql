CREATE TABLE IF NOT EXISTS `character_bank_reagent_settings` (
  `guid` INT UNSIGNED NOT NULL,
  `auto_deposit` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `character_bank_reagents` (
  `guid` INT UNSIGNED NOT NULL,
  `item_entry` INT UNSIGNED NOT NULL,
  `quantity` BIGINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`guid`, `item_entry`),
  KEY `idx_item_entry` (`item_entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
