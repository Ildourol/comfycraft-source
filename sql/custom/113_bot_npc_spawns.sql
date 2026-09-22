-- ============================================================================
--  ONE DESIGN, SEVERAL BODIES  (tw_char)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\113_bot_npc_spawns.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_char
--
--  APPLY BEFORE THE BINARY THAT READS IT. BotNpcMgr::LoadSpawns selects from
--  this table inside World::SetInitialWorldSettings, and a missing table is a
--  failed query, which on this core asserts and writes a crash dump.
--
--  Safe under the OLD binary too: it only creates and copies. The old columns
--  on bot_npc (character_guid, map, position_*, orientation, look_applied) are
--  left in place so a server still running the previous build keeps working;
--  114 drops them once the new binary is in.
--
--  ------------------------------------------------------- WHY THIS EXISTS
--
--  A design owned one body, so the same NPC in every starting zone meant one
--  design per zone, each with its own copy of the look, the menu, the shop and
--  the idle rules to keep in step by hand. Now a design is the NPC and each row
--  here is one place it stands.
--
--  KEYED BY CHARACTER GUID. A spawn always has a body (placing is what makes
--  one), and a character can only stand for one design, so the guid is the
--  natural key and there is no second id to keep in step with it. Unplacing
--  deletes the row and keeps the character, as it always did.
--
--  look_applied MOVED HERE because it is per body: each copy is dressed on its
--  own, and one copy being dressed says nothing about the others.
--
--  Re-runnable: the copy skips any character already here.
-- ============================================================================

CREATE TABLE IF NOT EXISTS `bot_npc_spawn` (
  `character_guid` int(10) unsigned NOT NULL COMMENT 'characters.guid of the body',
  `bot_npc_id`     int(10) unsigned NOT NULL COMMENT 'bot_npc.id, the design it stands for',
  `map`            smallint(5) unsigned NOT NULL DEFAULT 0,
  `position_x`     float NOT NULL DEFAULT 0,
  `position_y`     float NOT NULL DEFAULT 0,
  `position_z`     float NOT NULL DEFAULT 0,
  `orientation`    float NOT NULL DEFAULT 0,
  `look_applied`   varchar(600) NOT NULL DEFAULT '' COMMENT 'the outfit actually on this body; dressing is skipped while it equals the design look',
  PRIMARY KEY (`character_guid`),
  KEY `bot_npc_id` (`bot_npc_id`)
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COLLATE=utf8mb3_general_ci
  COMMENT='Where each bot NPC design stands. One design may have several';

INSERT INTO `bot_npc_spawn`
       (`character_guid`, `bot_npc_id`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `look_applied`)
SELECT b.`character_guid`, b.`id`, b.`map`, b.`position_x`, b.`position_y`, b.`position_z`, b.`orientation`, b.`look_applied`
  FROM `bot_npc` b
 WHERE b.`character_guid` <> 0
   AND NOT EXISTS (SELECT 1 FROM `bot_npc_spawn` s WHERE s.`character_guid` = b.`character_guid`);
