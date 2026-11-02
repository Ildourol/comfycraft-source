-- ============================================================================
--  The Ashenvale Hunt beasts are hostile to everyone  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\227_ashenvale_hunt_beasts.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. mangosd reads factionless_peace_creature once at startup,
--  and spawns already in the world keep their old faction until they
--  respawn.
--
--  Bug 131. Ursangous (12678), Shadumbra (12677) and Sharptalon (12676) had
--  Darnassus faction templates (79 and 1097). Factionless.PeaceFactions makes
--  every race Friendly with Darnassus, so they were green to everyone and
--  nobody could kill them for the trophies that start quests 23, 24 and 2.
--  The war list (factionless_war_creature) needs a quest against the
--  creature in the log, and this chain has none: the trophy starts the quest.
--
--  Each beast gets a stock hostile beast template with no reputation bar:
--    12678 Ursangous   44  Beast - Bear  (the Ashenvale bears, Ursol'lok)
--    12677 Shadumbra   66  Leopard       (the Teldrassil and Darkshore nightsabers)
--    12676 Sharptalon  16  Monster       (Frayfeather Hippogryph)
--  Ashenvale has no stock hostile nightsabers or hippogryphs.
--
--  The Family Name (40800, Alleana Moonfeather) needs the player to talk to
--  Sharptalon (gossip_scripts 41389 gives the credit for 60031). A player
--  who has 40800 in the log is at peace with him through
--  factionless_peace_creature (FactionlessWar.h): he is green to that
--  player and offers his gossip. mangosd probes information_schema before it
--  reads, so a binary that meets a database without this table runs on.
--
--  Re-runnable. Undo: set faction 79 on 12678 and 1097 on 12676 and 12677,
--  and delete the 12676 row from factionless_peace_creature.
-- ============================================================================

UPDATE creature_template SET faction = 44 WHERE entry = 12678;
UPDATE creature_template SET faction = 66 WHERE entry = 12677;
UPDATE creature_template SET faction = 16 WHERE entry = 12676;

CREATE TABLE IF NOT EXISTS factionless_peace_creature (
    entry   INT UNSIGNED NOT NULL,
    quest   INT UNSIGNED NOT NULL,
    comment VARCHAR(255) NOT NULL DEFAULT '',
    PRIMARY KEY (entry, quest)
) ENGINE=MyISAM DEFAULT CHARSET=utf8;

DELETE FROM factionless_peace_creature WHERE entry = 12676;
INSERT INTO factionless_peace_creature (entry, quest, comment) VALUES
(12676, 40800, 'Sharptalon: The Family Name');
