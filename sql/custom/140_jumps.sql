-- ============================================================================
--  HOW MANY TIMES EACH CHARACTER HAS JUMPED  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\140_jumps.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT WRITES IT. A failed statement on this core
--  asserts and writes a crash dump rather than warning.
--
--  ---------------------------------------------------------- WHAT IT IS FOR
--
--  The jump board at /charts: most jumps, a plain total.
--
--  The core counts every MSG_MOVE_JUMP a real client sends for its own
--  character (bots have no socket and are not counted). It holds the count in
--  memory and adds it to this row every 30 s while the player jumps, and at
--  every save, logout included. The board on /charts asks again every 30 s.
--
--  ------------------------------------- DELETING A CHARACTER MUST SWEEP IT
--
--  CHARACTER GUIDS ARE REUSED. Player::DeleteFromDB gains a DELETE for this
--  table in the same change. A row left behind would hand a new character
--  somebody else's score.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and nothing here is seeded. There
--  is no backfill; nothing was recorded before this.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_jumps` (
    `guid`   INT(10) UNSIGNED NOT NULL DEFAULT 0
             COMMENT 'characters.guid',
    `jumps`  INT(10) UNSIGNED NOT NULL DEFAULT 0
             COMMENT 'every jump this character has made',
    PRIMARY KEY (`guid`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='The jump board: how many times each character has jumped';
