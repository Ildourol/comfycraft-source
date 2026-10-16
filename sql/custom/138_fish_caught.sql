-- ============================================================================
--  EVERY ITEM A CHARACTER HAS FISHED UP  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\138_fish_caught.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT WRITES IT. A failed statement on this core
--  asserts and writes a crash dump rather than warning.
--
--  ---------------------------------------------------------- WHAT IT IS FOR
--
--  The fish board at /charts: most fish caught, a plain total.
--
--  A ROW IS ONE ITEM KIND PER CHARACTER, AND `caught` IS HOW MANY. The core
--  counts EVERY item taken from a fishing bobber or a fishing school, junk
--  included. Which items are fish is decided by the website at read time
--  (FISH in website/lib/charts.js), so a change to that list changes the whole
--  history and nothing caught is lost.
--
--  Counted when the item goes into the bags, not when the bobber is clicked:
--  a fish left in the loot window was not caught.
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

CREATE TABLE IF NOT EXISTS `character_fish_catch` (
    `guid`    INT(10) UNSIGNED NOT NULL DEFAULT 0
              COMMENT 'characters.guid',
    `item`    INT(10) UNSIGNED NOT NULL DEFAULT 0
              COMMENT 'item_template.entry',
    `caught`  INT(10) UNSIGNED NOT NULL DEFAULT 0
              COMMENT 'how many of this item the character has fished up',
    PRIMARY KEY (`guid`, `item`),
    KEY `item` (`item`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='The fish board: every item a character has taken from fishing loot';
