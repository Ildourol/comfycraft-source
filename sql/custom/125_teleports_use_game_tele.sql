-- ============================================================================
--  "Teleports somewhere" names .tele places, not .dest lists        (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\125_teleports_use_game_tele.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then, in the mangosd console (or .\Reload-Server.ps1 game_tele, gossip_scripts):
--
--    reload game_tele
--    reload gossip_scripts
--
--  APPLY AFTER THE BINARY. The old binary loads dialog_dest at startup, and a
--  missing table is ASSERT(false) and a crash dump, so the DROPs at the end
--  must not reach a server that still reads them. Until this runs, the new
--  binary reads an old row's list id as a game_tele id and sends the player to
--  whatever Turtle place has that number.
--
--  WHAT CHANGED. SCRIPT_COMMAND_TELEPORT_RANDOM (100) held one dialog_dest_list
--  id in `datalong`. It now holds up to eight game_tele ids, in `datalong` to
--  `datalong4` and `dataint` to `dataint4`, and picks one at random. `.tele add`
--  already did what `.dest` did, next to 654 places Turtle ships, so `.dest`
--  and both of its tables are gone. docs/ai/content/dialogs.md has the rest.
--
--  RE-RUNNABLE. The place copy skips names game_tele already has, and the
--  script rewrite matches only rows still carrying the old comment, which the
--  editor never writes any more.
-- ============================================================================

-- Only while the old tables are still here; a re-run after the DROP skips it.
SET @has_dest := (SELECT COUNT(*) FROM information_schema.tables
                   WHERE table_schema = DATABASE() AND table_name = 'dialog_dest');

-- 1. Every destination becomes a .tele place, under its own name.
SET @copy := IF(@has_dest > 0,
  'INSERT INTO game_tele (position_x, position_y, position_z, orientation, map, name)
   SELECT d.x, d.y, d.z, d.o, d.map, d.name FROM dialog_dest d
    WHERE NOT EXISTS (SELECT 1 FROM game_tele t WHERE t.name = d.name)',
  'DO 0');
PREPARE s FROM @copy; EXECUTE s; DEALLOCATE PREPARE s;

-- 2. Every old-form row: its list's places, in list order, into the 8 slots.
--    Through a copy of the list ids, because an UPDATE reads the columns it
--    has already set, and slot 2 would look up the list named by slot 1.
-- A plain table: MySQL cannot open a TEMPORARY one twice in one statement.
DROP TABLE IF EXISTS mig_tele;
CREATE TABLE mig_tele (sid INT UNSIGNED, list INT UNSIGNED, n TINYINT UNSIGNED, tele INT UNSIGNED);

SET @fill := IF(@has_dest > 0,
  'INSERT INTO mig_tele (sid, list, n, tele)
   SELECT gs.id, gs.datalong,
          (SELECT COUNT(*) FROM dialog_dest d2 WHERE d2.list = d.list AND d2.id < d.id),
          t.id
     FROM gossip_scripts gs
     JOIN dialog_dest d ON d.list = gs.datalong
     JOIN game_tele t ON t.name = d.name
    WHERE gs.command = 100 AND gs.comments LIKE ''Dialog: teleports to one of list %''',
  'DO 0');
PREPARE s FROM @fill; EXECUTE s; DEALLOCATE PREPARE s;

UPDATE gossip_scripts gs
   SET gs.datalong  = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.list = gs.datalong AND m.n = 0), 0),
       gs.comments  = CONCAT('Dialog: teleports to ',
                        COALESCE((SELECT GROUP_CONCAT(t.name ORDER BY m.n SEPARATOR ', ')
                                    FROM mig_tele m JOIN game_tele t ON t.id = m.tele
                                   WHERE m.sid = gs.id AND m.n < 8), 'nowhere'))
 WHERE gs.command = 100 AND gs.comments LIKE 'Dialog: teleports to one of list %'
   AND EXISTS (SELECT 1 FROM mig_tele m WHERE m.sid = gs.id);

-- Slots 2 to 8, keyed on the script id alone now that datalong has moved.
UPDATE gossip_scripts gs
   SET gs.datalong2 = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 1), 0),
       gs.datalong3 = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 2), 0),
       gs.datalong4 = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 3), 0),
       gs.dataint   = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 4), 0),
       gs.dataint2  = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 5), 0),
       gs.dataint3  = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 6), 0),
       gs.dataint4  = COALESCE((SELECT tele FROM mig_tele m WHERE m.sid = gs.id AND m.n = 7), 0)
 WHERE gs.command = 100 AND gs.id IN (SELECT sid FROM mig_tele);

DROP TABLE mig_tele;

-- 3. The old tables. Nothing reads them after the binary this goes with.
DROP TABLE IF EXISTS dialog_dest;
DROP TABLE IF EXISTS dialog_dest_list;
