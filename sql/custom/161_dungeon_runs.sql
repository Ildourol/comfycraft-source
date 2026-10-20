-- ============================================================================
--  DUNGEON RUNS: LAST BOSS AND FULL RUN TIMES  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\161_dungeon_runs.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  One row in dungeon_run for each run that reached its last boss. The clock
--  starts when the first player enters a fresh instance. last_boss_ms is the
--  kill of the last boss; full_ms is the kill that leaves every required boss
--  dead, NULL until then. Both lists are in
--  source/src/game/Maps/DungeonBosses.h. wing is 0 on a map with one part; on
--  a split map it is the wing number there.
--
--  mangosd gives the ids (MAX(id) at startup, then counts up) and writes a run
--  twice, at the last boss and at the full run. Each write is an upsert that
--  keeps what the other wrote, so their order does not matter.
--
--  boss_level is the last boss's level at the kill. max_level is the highest
--  level any member had during the run (read at each entry and boss kill);
--  last_max_level is the same when the last boss died. /charts ranks a time
--  only when that level is not above boss_level (no margin).
--
--  dungeon_run_boss holds the split for each listed boss, optional ones too.
--  dungeon_run_member holds each player who entered during the run, in order
--  of entry, bots and companions included and marked by is_bot (an RNDBOT
--  or COMPANION account at entry); group_size is their count. A
--  deleted character's rows keep the name and get guid 0
--  (Player::DeleteFromDB): guids are reused.
--
--  mangosd probes information_schema at startup. Without these columns it
--  still sends the chat lines and writes nothing. Apply this BEFORE a binary
--  that has the timer, or restart mangosd after.
--
--  Re-runnable: creates if missing and never touches the rows.

CREATE TABLE IF NOT EXISTS `dungeon_run` (
  `id`           INT UNSIGNED      NOT NULL,
  `map`          SMALLINT UNSIGNED NOT NULL,
  `wing`         TINYINT UNSIGNED  NOT NULL DEFAULT 0,
  `started_at`   DATETIME          NOT NULL,
  `last_boss_ms` INT UNSIGNED      NULL,
  `full_ms`      INT UNSIGNED      NULL,
  `group_size`   TINYINT UNSIGNED  NOT NULL,
  `boss_level`     TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `last_max_level` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `max_level`      TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  KEY `board_last` (`map`, `wing`, `last_boss_ms`),
  KEY `board_full` (`map`, `wing`, `full_ms`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dungeon_run_boss` (
  `run_id`     INT UNSIGNED       NOT NULL,
  `entry`      MEDIUMINT UNSIGNED NOT NULL,
  `elapsed_ms` INT UNSIGNED       NOT NULL,
  PRIMARY KEY (`run_id`, `entry`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS `dungeon_run_member` (
  `run_id` INT UNSIGNED     NOT NULL,
  `slot`   TINYINT UNSIGNED NOT NULL,
  `guid`   INT UNSIGNED     NOT NULL,
  `name`   VARCHAR(12)      NOT NULL,
  `class`  TINYINT UNSIGNED NOT NULL,
  `level`  TINYINT UNSIGNED NOT NULL,
  `is_bot` TINYINT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`run_id`, `slot`),
  KEY `guid` (`guid`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;
