-- ============================================================================
--  OKUL IN HAMMERFALL, JAMA'NALI IN UN'GORO: TWO QUEST GIVERS WITH NO SPAWN
--  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\228_okul_jamanali_spawns.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  A spawn loads at start-up only: restart the server.
--
--  Upstream ships both creature templates and their quest links, but no
--  `creature` row, so nobody can take the quests (bug 125).
--
--  Okul (62638), Horde, faction template 85 (Orgrimmar, the same as the
--  Hammerfall Guardians), gives and ends Cavernweb Extract (41752). He stands
--  on the floor of the Hammerfall yard at height 71, between Zengu and the
--  spot where the bug reporter stood, and faces into the yard. The ground
--  under the yard is at 57 to 69: the floor is part of a building model, the
--  same floor that Zengu, Drum Fel and Uttnar stand on. Spawn time and
--  movement are copied from Tharlidun, the stable master beside him.
--
--  Jama'nali (62700), faction 35, gives and ends the five Ravasaur quests
--  (41769-41773). The quest text puts her "at the eastern entrance" and "at
--  the southeastern entrance" of Un'Goro Crater: the foot of the ramp from
--  Tanaris, where Torwa Pathfinder (9619) stands. She stands 7 yards south of
--  Torwa on flat ground (height -254.3 from the server's map tile), facing
--  the same way he does. Spawn time is copied from Torwa.
--
--  Guids are from our quest-fix block (2949000-2949099). The DELETE by id also
--  removes a copy someone placed with `.npc add`.
-- ============================================================================

DELETE FROM creature WHERE id IN (62638, 62700);
DELETE FROM creature WHERE guid IN (2949002, 2949003);

INSERT INTO creature (guid, id, map, position_x, position_y, position_z, orientation,
                      spawntimesecsmin, spawntimesecsmax, wander_distance, movement_type)
VALUES
(2949002, 62638, 0, -941.0,  -3526.0,  71.03,  5.39,    400, 400, 0, 0),
(2949003, 62700, 1, -7843.3, -2123.3, -254.32, 1.36136, 300, 300, 0, 0);
