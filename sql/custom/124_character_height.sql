-- ============================================================================
--  CHARACTER HEIGHT  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\124_character_height.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT READS IT. The login query selects this column
--  and Player::SaveToDB writes it, and a failed query on this core asserts and
--  writes a crash dump.
--
--  The height a character bought in the barbershop chair, in thousandths of
--  the race's own scale: 1000 is the size the race is drawn at, 900 is 90%.
--  The chair sells 900 to 1025 in steps of 5 (BarbershopMgr.cpp). The core
--  applies it with Unit::SetNativeScale, so a druid form or a transform keeps
--  it and puts it back when it ends.
--
--  A COLUMN, NOT A TABLE. SaveToDB writes the whole row with REPLACE INTO, so
--  a column it did not list would go back to the default on every save; it is
--  listed there. A column also dies with the character, so the reused-guid
--  trap of a per-character table does not apply.
-- ============================================================================

SET @have := (SELECT COUNT(*) FROM information_schema.COLUMNS
              WHERE TABLE_SCHEMA = DATABASE() AND TABLE_NAME = 'characters' AND COLUMN_NAME = 'height');
SET @sql := IF(@have = 0,
    'ALTER TABLE characters ADD COLUMN height SMALLINT UNSIGNED NOT NULL DEFAULT 1000',
    'SELECT ''height already there''');
PREPARE stmt FROM @sql;
EXECUTE stmt;
DEALLOCATE PREPARE stmt;
