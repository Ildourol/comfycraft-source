-- ============================================================================
--  LOOT LUCK: BAD-LUCK PROTECTION ON QUEST DROPS  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\160_loot_luck.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  The switch for PRD (pseudo-random distribution) on quest drops. 1 is on,
--  0 is off. Off, each quest roll uses the plain table chance.
--
--  Each player who needs a quest item rolls for it alone, on or off. The
--  switch only changes the chance of that roll. source/src/game/LootLuck.h
--  has the rule.
--
--  mangosd reads the row at startup and on the console command `lootluck`.
--  The Workshop page on the website (/admin) writes the row and sends that
--  command. `lootluck on` and `lootluck off` in the console write it too.
--
--  In tw_char and not tw_world: it is a setting of this realm, and content
--  moved between local and prod must not carry it.
--
--  mangosd probes information_schema before it reads, so a binary that meets
--  a database without this table runs with PRD off.
--
--  Re-runnable: creates if missing and never touches the row.

CREATE TABLE IF NOT EXISTS `loot_luck` (
  `id`         TINYINT UNSIGNED NOT NULL,
  `prd`        TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `changed_at` DATETIME NULL,
  `changed_by` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `loot_luck` (`id`, `prd`) VALUES (1, 0);
