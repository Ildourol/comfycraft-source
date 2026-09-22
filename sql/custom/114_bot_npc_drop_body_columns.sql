-- ============================================================================
--  THE BODY COLUMNS LEAVE bot_npc  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\114_bot_npc_drop_body_columns.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY AFTER THE BINARY FROM 113 IS RUNNING, never before. The previous
--  build selects and updates these columns, and a missing column is a hard
--  crash. The new build and the website read bot_npc_spawn and nothing here.
--
--  113 copied every placed body into bot_npc_spawn. What is left in these
--  columns is a second, stale answer to "where does it stand", which is the
--  kind of thing somebody reads by hand one day and believes.
-- ============================================================================

ALTER TABLE `bot_npc`
  DROP COLUMN `character_guid`,
  DROP COLUMN `map`,
  DROP COLUMN `position_x`,
  DROP COLUMN `position_y`,
  DROP COLUMN `position_z`,
  DROP COLUMN `orientation`,
  DROP COLUMN `look_applied`;
