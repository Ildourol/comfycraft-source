-- ============================================================================
--  Bael Modan: fewer golems, each one fighting a Bael'dun Excavator  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\235_bael_modan_golem_fights.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. Spawn rows (creature, creature_addon) load only at
--  startup. `reload creature_ai_events` reloads the EventAI rows and their
--  scripts, but a golem already spawned keeps its old event list.
--
--  Bug 140, after sql/custom/231. 231 put 30 live Bael Modan Golems (60442)
--  on the pit floor. The owner: too many, and they should fight the dwarves.
--  Quest 40367 needs 16 kills and a golem respawns in 300 s, so 14 stay.
--  Each kept golem stands 2.5 to 3.1 yd from one Bael'dun Excavator (3374)
--  and faces it. The excavator stands still and faces the golem.
--
--  THE FIGHT IS REAL. A creature can attack another creature only if one of
--  the two is hostile to the other (WorldObject::IsValidAttackTarget, the CvC
--  case). Golem faction 415 and excavator faction 122 are neutral to each
--  other. So a golem's out-of-combat timer, when its own excavator is alive
--  within 4 yd and not in combat:
--    1. sets the golem's faction to 554 for this fight (stock template:
--       monster, hostile to players, Alliance and Horde, no reputation).
--       Flags 7: the golem gets 415 back at combat stop, on reaching home
--       and at respawn.
--    2. attacks that excavator. EventAI AttackStart puts both in combat at
--       once; the excavator hits back with its stock EventAI.
--    3. if the golem is still not in combat (the attack did not start),
--       clears the faction at once, so a hostile golem never stands idle.
--  While a golem has a victim it does not pick a second one in the open world
--  (CreatureEventAI::MoveInLineOfSight), so each fight stays one against one.
--  Excavators can die. A player can step in and pull the golem.
--
--  Loot and XP from a golem: Unit::Kill gives them only if players did more
--  than 35% of the damage (Creature::IsLootAllowedDueToDamageOrigin). The
--  count resets at combat stop. Quest credit goes to the player with the
--  killing blow either way.
--
--  An EventAI change acts on every spawn of the entry. In
--  source/sql/base/tw_world_creature.sql, 60442 spawns only here (these 14,
--  the 16 deleted below and the two dead ones). 3374 gets no EventAI change.
--
--  Points: terrain height from server\maps\0013936.map (+0.1), on the navmesh
--  within 1 yd of it with a path from Silverhand (-3993 -2301 91.8) and Kex
--  (-4033 -2002 91.7) (tools/navmesh gridprobe), slope under 0.4, 3 yd or
--  more from every stock object. Two fights are 10.6 yd or more apart
--  (closest members). No crate of 231 is within 6 yd of a fight (7.3 yd is
--  the closest), so the crates stay.
--
--  Not changed: the dead golems 2568977 and 2568978 (spawn_flags 128). The
--  scripts search for live excavators only, and a dead golem runs no AI.
--
--  Re-runnable. Undo: the old values are in the comment on each line; the
--  deleted golems are in source/sql/base/tw_world_creature.sql, at the
--  positions of sql/custom/231.
-- ============================================================================

-- Golems not kept. No creature_addon, creature_movement, pool or linking row
-- uses these guids (checked in source/sql/base); the DELETEs are a guard.
DELETE FROM creature WHERE id = 60442 AND guid IN (2568981, 2568982, 2568983, 2568984, 2568986, 2568989, 2568991, 2568992, 2568995, 2568996, 2568998, 2569000, 2569001, 2569002, 2569004, 2569307);
DELETE FROM creature_addon    WHERE guid IN (2568981, 2568982, 2568983, 2568984, 2568986, 2568989, 2568991, 2568992, 2568995, 2568996, 2568998, 2569000, 2569001, 2569002, 2569004, 2569307);
DELETE FROM creature_movement WHERE id   IN (2568981, 2568982, 2568983, 2568984, 2568986, 2568989, 2568991, 2568992, 2568995, 2568996, 2568998, 2569000, 2569001, 2569002, 2569004, 2569307);
DELETE FROM pool_creature     WHERE guid IN (2568981, 2568982, 2568983, 2568984, 2568986, 2568989, 2568991, 2568992, 2568995, 2568996, 2568998, 2569000, 2569001, 2569002, 2569004, 2569307);

-- Kept golems (60442): each 2.5 to 3.1 yd from its excavator, facing it.
UPDATE creature SET position_x = -4063.6, position_y = -2170.5, position_z = 51.22, orientation = 5.2449 WHERE guid = 2568980 AND id = 60442;  -- fights 13610; 231 had -4092.0 -2146.0 50.62
UPDATE creature SET position_x = -4067.8, position_y = -2140.8, position_z = 51.53, orientation = 4.8788 WHERE guid = 2568976 AND id = 60442;  -- fights 13608; 231 had -4074.0 -2136.0 50.40
UPDATE creature SET position_x = -4072.0, position_y = -2179.1, position_z = 50.28, orientation = 5.7376 WHERE guid = 2568979 AND id = 60442;  -- fights 13598; 231 had -4080.0 -2182.0 50.28
UPDATE creature SET position_x = -4117.3, position_y = -2186.5, position_z = 50.28, orientation = 0.1863 WHERE guid = 2568988 AND id = 60442;  -- fights 13614; 231 had -4112.0 -2200.0 50.48
UPDATE creature SET position_x = -4118.8, position_y = -2175.8, position_z = 50.28, orientation = 3.1456 WHERE guid = 2568987 AND id = 60442;  -- fights 13594; 231 had -4112.0 -2172.0 50.28
UPDATE creature SET position_x = -4124.0, position_y = -2239.5, position_z = 51.03, orientation = 0.8542 WHERE guid = 2568990 AND id = 60442;  -- fights 13599; 231 had -4134.0 -2236.0 51.12
UPDATE creature SET position_x = -4129.3, position_y = -2160.2, position_z = 50.89, orientation = 5.7678 WHERE guid = 2568985 AND id = 60442;  -- fights 13611; 231 had -4108.0 -2146.0 50.71
UPDATE creature SET position_x = -4143.9, position_y = -2162.2, position_z = 51.44, orientation = 2.6319 WHERE guid = 2568994 AND id = 60442;  -- fights 13615; 231 had -4142.0 -2172.0 50.84
UPDATE creature SET position_x = -4152.4, position_y = -2182.8, position_z = 50.31, orientation = 1.4006 WHERE guid = 2568993 AND id = 60442;  -- fights 13616; 231 had -4140.0 -2190.0 50.28
UPDATE creature SET position_x = -4159.8, position_y = -2225.1, position_z = 51.52, orientation = 2.2790 WHERE guid = 2568997 AND id = 60442;  -- fights 13613; 231 had -4166.0 -2234.0 52.10
UPDATE creature SET position_x = -4171.1, position_y = -2179.1, position_z = 50.28, orientation = 0.7169 WHERE guid = 2569003 AND id = 60442;  -- fights 13600; 231 had -4186.0 -2182.0 51.01
UPDATE creature SET position_x = -4177.0, position_y = -2196.7, position_z = 50.28, orientation = 3.3119 WHERE guid = 2568999 AND id = 60442;  -- fights 13612; 231 had -4174.0 -2188.0 50.28
UPDATE creature SET position_x = -4185.4, position_y = -2259.4, position_z = 50.65, orientation = 4.8737 WHERE guid = 2569006 AND id = 60442;  -- fights 13604; 231 had -4196.0 -2258.0 50.28
UPDATE creature SET position_x = -4216.9, position_y = -2282.9, position_z = 52.17, orientation = 2.8034 WHERE guid = 2569005 AND id = 60442;  -- fights 13584; 231 had -4194.0 -2272.0 50.88

-- Their excavators (3374): stand still and face the golem. Position stays.
UPDATE creature SET orientation = 5.9450 WHERE guid = 13584 AND id = 3374;  -- was orientation 3.8863
UPDATE creature SET orientation = 0.0040, movement_type = 0, wander_distance = 0 WHERE guid = 13594 AND id = 3374;  -- was orientation 0.2201, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 2.5961, movement_type = 0, wander_distance = 0 WHERE guid = 13598 AND id = 3374;  -- was orientation 3.1696, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 3.9958 WHERE guid = 13599 AND id = 3374;  -- was orientation 5.3058
UPDATE creature SET orientation = 3.8585, movement_type = 0, wander_distance = 0 WHERE guid = 13600 AND id = 3374;  -- was orientation 2.8880, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 1.7321 WHERE guid = 13604 AND id = 3374;  -- was orientation 5.6200
UPDATE creature SET orientation = 1.7372 WHERE guid = 13608 AND id = 3374;  -- was orientation 5.8120
UPDATE creature SET orientation = 2.1034 WHERE guid = 13610 AND id = 3374;  -- was orientation 0.6907
UPDATE creature SET orientation = 2.6262, movement_type = 0, wander_distance = 0 WHERE guid = 13611 AND id = 3374;  -- was orientation 0.1571, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 0.1703, movement_type = 0, wander_distance = 0 WHERE guid = 13612 AND id = 3374;  -- was orientation 2.4229, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 5.4206, movement_type = 0, wander_distance = 0 WHERE guid = 13613 AND id = 3374;  -- was orientation 5.8721, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 3.3279, movement_type = 0, wander_distance = 0 WHERE guid = 13614 AND id = 3374;  -- was orientation 5.7421, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 5.7735, movement_type = 0, wander_distance = 0 WHERE guid = 13615 AND id = 3374;  -- was orientation 4.4506, movement_type 1, wander_distance 5
UPDATE creature SET orientation = 4.5422 WHERE guid = 13616 AND id = 3374;  -- was orientation 5.1836

-- Excavators that dug with emote_state 173 (work) now stand ready. Every
-- other column is the stock row.
DELETE FROM creature_addon WHERE guid IN (13599, 13604, 13608, 13610);
INSERT INTO creature_addon (guid, display_id, mount_display_id, equipment_id, stand_state, sheath_state, emote_state, auras) VALUES
(13599, 0, 0, -1, 0, 1, 0, NULL),  -- was emote_state 173
(13604, 0, 0, -1, 0, 1, 0, NULL),  -- was emote_state 173
(13608, 0, 0, -1, 0, 1, 0, NULL),  -- was emote_state 173
(13610, 0, 0, -1, 0, 1, 0, NULL);  -- was emote_state 173

-- Golem EventAI (60442 had no events). Out of combat, every 15 to 30 s
-- (first time 5 to 10 s after spawn): start the fight with its excavator.
DELETE FROM creature_ai_events WHERE id = 6044201;
INSERT INTO creature_ai_events (id, creature_id, condition_id, event_type, event_inverse_phase_mask, event_chance, event_flags, event_param1, event_param2, event_param3, event_param4, action1_script, action2_script, action3_script, comment) VALUES
(6044201, 60442, 0, 1, 0, 100, 1, 5000, 10000, 15000, 30000, 6044201, 0, 0, 'Bael Modan Golem - OOC - Attack the Bael\'dun Excavator beside it');

-- EventAI runs every row of the script at once, in `priority` order; `delay`
-- is not used. target_type 8 = nearest LIVE creature 3374 within 4 yd.
-- data_flags 16 = skip the row with no error when that excavator is missing.
-- Condition 117 (stock, type 43 reversed): the target is not in combat.
DELETE FROM creature_ai_scripts WHERE id = 6044201;
INSERT INTO creature_ai_scripts (id, delay, priority, command, datalong, datalong2, datalong3, datalong4, target_param1, target_param2, target_type, data_flags, dataint, dataint2, dataint3, dataint4, x, y, z, o, condition_id, comments) VALUES
(6044201, 0, 0, 22, 554, 7, 0, 0, 3374, 4, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 117, 'Bael Modan Golem - Set Faction 554 (hostile to all) until combat stop'),
(6044201, 0, 1, 26,   5, 0, 0, 0, 3374, 4, 8, 16, 0, 0, 0, 0, 0, 0, 0, 0, 117, 'Bael Modan Golem - Attack the Bael\'dun Excavator'),
-- target = self (data_flags 4): clear the faction if the attack did not start.
(6044201, 0, 2, 22,   0, 0, 0, 0,    0, 0, 0,  4, 0, 0, 0, 0, 0, 0, 0, 0, 117, 'Bael Modan Golem - Restore faction if not in combat');
