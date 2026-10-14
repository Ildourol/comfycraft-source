-- ============================================================================
--  LEVEL CAP  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\134_level_cap.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  The highest level XP can take a character to, for testing the game in
--  brackets. 0 is no cap: XP runs to MaxPlayerLevel in mangosd.conf (60). A
--  value at or above MaxPlayerLevel is the same as 0.
--
--  The cap only stops XP. A character already above it keeps its level, and
--  random bot levels do not follow it. At the cap a quest pays the max-level
--  gold in place of XP, as at 60.
--
--  mangosd reads the row at startup and on the console command `levelcap`.
--  The Workshop page on the website (/admin) writes the row and sends that
--  command. `levelcap <n>` in the console writes it too.
--
--  In tw_char and not tw_world: it is a setting of this realm, and content
--  moved between local and prod must not carry it.
--
--  mangosd probes information_schema before it reads, so a binary that meets
--  a database without this table runs with no cap.
--
--  Re-runnable: creates if missing and never touches the row.

CREATE TABLE IF NOT EXISTS `level_cap` (
  `id`         TINYINT UNSIGNED NOT NULL,
  `level`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `changed_at` DATETIME NULL,
  `changed_by` VARCHAR(32) NOT NULL DEFAULT '',
  PRIMARY KEY (`id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

INSERT IGNORE INTO `level_cap` (`id`, `level`) VALUES (1, 0);
