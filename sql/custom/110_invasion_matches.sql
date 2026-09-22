-- ============================================================================
--  EVERY INVASION THAT CAME TO BLOWS  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\110_invasion_matches.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT WRITES IT. A failed query on this core asserts
--  and writes a crash dump rather than warning.
--
--  ---------------------------------------------------------- WHAT IT IS FOR
--
--  The Invasion heroes board at /charts: most invasions won. Until now an
--  invasion lived in one vector in InvasionManager and was forgotten when it
--  ended (docs/features/invasions.md, "Nothing is persisted, on purpose").
--  That rule was about the MATCH, so a restart can never strand an invader;
--  this is the RESULT, written once when the match is over, and nothing reads
--  it back into the game.
--
--  A ROW IS ONE MATCH THAT REACHED THE FIGHT. A hunt that went cold during the
--  warning never met anybody and is not here.
--
--  `winner` IS SET ONLY WHEN SOMEBODY SURVIVED AND WON: the other side died,
--  it was the winner (or their pet) that killed them, and the winner was still
--  alive at that moment. Either side can win; a defender who drives off their
--  invader is as much a hero as an invader who fells their quarry. Everything
--  else is 0: time running out, a defender reaching a sanctuary, either side
--  logging out, or a death to something that was not the opponent.
--
--  ------------------------------------- DELETING A CHARACTER ZEROES, NOT DELETES
--
--  CHARACTER GUIDS ARE REUSED, so a deleted character's guid must leave this
--  table or a new character would inherit their wins. But a match has two
--  people in it, and deleting the row would take a win away from the one who
--  is still here. So Player::DeleteFromDB sets the gone character's guid to 0
--  wherever it appears, and the row stays.
--
--  Re-runnable: CREATE TABLE IF NOT EXISTS, and nothing here is seeded.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `character_invasion_match` (
    `id`       INT(10) UNSIGNED NOT NULL AUTO_INCREMENT,
    `invader`  INT(10) UNSIGNED NOT NULL DEFAULT 0
               COMMENT 'characters.guid; 0 once that character is deleted',
    `defender` INT(10) UNSIGNED NOT NULL DEFAULT 0
               COMMENT 'characters.guid; 0 once that character is deleted',
    `winner`   INT(10) UNSIGNED NOT NULL DEFAULT 0
               COMMENT 'invader or defender, or 0 when nobody survived and won',
    `ended_at` TIMESTAMP NOT NULL DEFAULT CURRENT_TIMESTAMP,
    PRIMARY KEY (`id`),
    KEY `winner` (`winner`),
    KEY `invader` (`invader`),
    KEY `defender` (`defender`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='Invasion heroes: every invasion that came to blows, and who won';
