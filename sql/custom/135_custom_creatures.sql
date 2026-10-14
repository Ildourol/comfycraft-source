-- ============================================================================
--  Custom NPCs that are creatures  (tw_world)
-- ============================================================================
--  /admin/npcbots makes two kinds of NPC. A Bot is a player body you can
--  dress (tw_char.bot_npc). A Creature is a creature_template row with a
--  model picked from a picture. This table says which creature_template rows
--  the editor made, and holds the editor's note for each.
--
--  The website reads and writes it. The core never reads it.
--
--  IDS: creature entries 101000-101999, handed out upward and never reused.
-- ============================================================================

CREATE TABLE IF NOT EXISTS custom_creature (
  entry   mediumint(8) unsigned NOT NULL PRIMARY KEY,
  comment varchar(255) NOT NULL DEFAULT '',
  made_at timestamp NOT NULL DEFAULT current_timestamp()
) ENGINE=MyISAM DEFAULT CHARSET=utf8mb3 COMMENT='creature_template rows made on /admin/npcbots';
