-- ============================================================================
--  Bael Modan: golems and Bael'dun dwarves fight on sight  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\236_bael_modan_open_fights.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. Spawn rows (creature, creature_addon) and the template
--  faction load at startup. A golem already spawned keeps its old faction
--  until it respawns.
--
--  Bug 140, after sql/custom/231 and 235. 235 paired each of the 14 Bael
--  Modan Golems (60442) with one Bael'dun Excavator (3374) and started each
--  fight from a golem EventAI timer. The owner: "They don't need to be paired
--  specifically with a dwarf, the dwarfs should start attacking if they get
--  close enough." This file retires the pairing and the timer. The fight now
--  comes from faction hostility alone.
--
--  THE FACTION. creature_template 60442: faction 415 -> 1394.
--    415  (faction 311 Titan):    our mask 8 (monster), hostile mask 1
--         (players). Neutral to Bael'dun template 122 both ways.
--    1394 (faction 80 Brood of Deathwing, the stock template of the Rogue
--         Black Drake 14388): our mask 8, hostile mask 1, enemy factions 689
--         and 47 (Ironforge). Faction 80 has no reputation bar
--         (reputation_list_id -1), and 60442 has no
--         creature_onkill_reputation row, so a kill gives no reputation.
--    122  (faction 47 Ironforge, every Bael'dun dwarf): our mask 2, friendly
--         mask 2, hostile mask 4, no enemy factions.
--  The rows in tw_world.faction_template (what the core loads) and in
--  server\dbc\FactionTemplate.dbc are the same for 122, 415 and 1394.
--
--  Hostility, both directions (FactionTemplateEntry::IsHostileTo, reached
--  through WorldObject::GetReactionTo -> GetFactionReactionTo; no player is
--  involved, so the factionless war and peace lists do not apply):
--    golem 1394 -> dwarf 122: enemy faction 47 = 122's faction -> HOSTILE.
--    dwarf 122 -> golem 1394: enemy factions none; hostile mask 4 & our mask
--         8 = 0 -> neutral.
--    golem 1394 -> player: faction 80 has no reputation, hostile mask 1 &
--         player our mask (3 or 5) -> HOSTILE. Same as 415.
--    player -> golem: no reputation branch; player hostile mask 12 or 10 &
--         our mask 8 -> HOSTILE. Same as 415. Both sides can pull a golem.
--  No stock template is hostile to 122 in both directions and still hostile
--  to both sides' players (a template with our mask 4 would be friendly to
--  Horde players). One direction is enough:
--    - CreatureCreatureRelocationWorker calls MoveInLineOfSight on BOTH
--      creatures when either one moves. So a dwarf that walks up to a golem
--      starts the fight as surely as a golem that walks up to a dwarf.
--    - WorldObject::IsValidAttackTarget (CvC) needs only one side hostile,
--      so the dwarf fights back with its stock EventAI.
--  The dwarf does not join a fight on its own. It joins when attacked or
--  called for help (call_for_help_range 5).
--
--  Changing the dwarves' side instead was rejected: 3374 to 3378, 3392 and
--  6668 are on factionless_war_creature (sql/custom/205), where a camp is one
--  template in one zone. A new template for 3374 would split the Bael'dun
--  camp in The Barrens.
--
--  AGGRO RANGE, golem against dwarf (Creature::GetAttackDistance):
--  detection_range 18 minus (dwarf level - golem level), at least 5, times
--  Rate.Creature.Aggro (1). Golem 60442 is level 19-20.
--    Excavator 3374 (21-22):          15 to 17 yd
--    Foreman 3375 (22-23):            14 to 16 yd
--    Prospector Khazgorm 3392 (24):   13 to 14 yd
--  Measured centre to centre in 3D (SizeFactor::None), in line of sight, and
--  only when the height difference less both bounding radii is 3 yd or less
--  (CREATURE_Z_ATTACK_RANGE).
--
--  WHO ELSE THE GOLEMS NOW FIGHT. 1394 is hostile to faction 47 and 689 only,
--  besides players. Within 160 yd of the pit every faction 47 creature is a
--  Bael'dun on template 122. Those on the pit floor are the targets. The
--  others stand 14 yd or more above every golem point and are out of reach:
--    rim (z 119-129): 6 Soldiers 3376, 7 Riflemen 3377, 1 Officer 3378
--    ledges (z 69-90): 4 Excavators 3374 (13593 13605 13606 13607) and
--                      Lord Cyrik Blackforge 6668
--  Not hostile either way: the Dig Rats (3444, template 7, no masks),
--  Digger Flameforge 5849 and Heggin Stonewhisker 5847 (template 14 since
--  sql/custom/233). No creature near the pit has 80 among its enemies.
--  60442 spawns only in this pit (source/sql/base/tw_world_creature.sql:
--  the 14 live golems and the two dead ones).
--
--  GOLEM POINTS. Same 14 guids. Each point:
--    - terrain height from server\maps\0013936.map, +0.1,
--    - on the navmesh (nearest polygon 0.5 yd or less sideways and within
--      0.5 yd of that height) with a path from Silverhand (-3993 -2301 91.8)
--      and from Kex (-4033 -2002 91.7) (a Detour probe like
--      tools/navmesh/pathbatch),
--    - terrain slope 0.3 or less (largest rise over 1 yd),
--    - 12.5 yd or more from every other golem,
--    - 6.7 yd or more from every crate of 231 (5000037-5000046), 3 yd or more
--      from every stock object, 6 yd or more from the dead spawns,
--    - outside the patrol band of the foreman 13628.
--  The floor is crowded: 34 live Bael'dun stand on it. At home, 10 golems are
--  17.5 to 23.9 yd from the nearest dwarf home (outside every range above).
--  4 are 16.1 to 16.3 yd from one (2568988, 2568993, 2568999, 2569003):
--  outside the range unless the golem rolls level 20 and the excavator level
--  21. The free ground is in a few places: 3 golems stand in the north west
--  corner, 1 at the north end, 2 in the middle and 8 in the south arm.
--  The golems wander (movement_type 1, wander_distance 5). With the dwarves
--  that wander 5 yd, a golem and a dwarf come within range now and then, and
--  a fight starts. The golems face the middle of the pit (-4140 -2200).
--
--  EXCAVATORS. The 14 that 235 changed get their stock orientation,
--  movement_type and wander_distance back (source/sql/base), and the four
--  that dug get emote_state 173 back.
--
--  Not changed: the dead golems 2568977 and 2568978 (spawn_flags 128); the
--  8 crates and the Mechagnome Head; creature_template 60442 ai_name (235 did
--  not change it; it stays 'EventAI' with no events).
--
--  Loot and XP from a golem: Unit::Kill gives them only if players did more
--  than 35% of the damage (Creature::IsLootAllowedDueToDamageOrigin). The
--  count resets at combat stop. Quest credit goes to the player with the
--  killing blow either way.
--
--  Re-runnable. Undo: the old values are in the comment on each line.
-- ============================================================================

-- The paired fight timer of 235.
DELETE FROM creature_ai_events  WHERE id = 6044201;
DELETE FROM creature_ai_scripts WHERE id = 6044201;

-- Bael Modan Golem: hostile to players and to Ironforge (the Bael'dun).
UPDATE creature_template SET faction = 1394 WHERE entry = 60442;  -- was 415

-- Golems (60442), spread over the pit floor, north to south. They wander.
UPDATE creature SET position_x = -4101.0, position_y = -2196.0, position_z = 52.31, orientation = 3.2438, movement_type = 1, wander_distance = 5 WHERE guid = 2568988 AND id = 60442;  -- was -4117.3 -2186.5 50.28, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4104.0, position_y = -2135.0, position_z = 51.87, orientation = 4.2066, movement_type = 1, wander_distance = 5 WHERE guid = 2568976 AND id = 60442;  -- was -4067.8 -2140.8 51.53, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4105.0, position_y = -2153.0, position_z = 50.29, orientation = 4.0723, movement_type = 1, wander_distance = 5 WHERE guid = 2568985 AND id = 60442;  -- was -4129.3 -2160.2 50.89, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4105.0, position_y = -2122.0, position_z = 54.46, orientation = 4.2906, movement_type = 1, wander_distance = 5 WHERE guid = 2568987 AND id = 60442;  -- was -4118.8 -2175.8 50.28, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4136.0, position_y = -2183.0, position_z = 50.32, orientation = 4.4813, movement_type = 1, wander_distance = 5 WHERE guid = 2568993 AND id = 60442;  -- was -4152.4 -2182.8 50.31, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4141.0, position_y = -2222.0, position_z = 50.89, orientation = 1.5254, movement_type = 1, wander_distance = 5 WHERE guid = 2568997 AND id = 60442;  -- was -4159.8 -2225.1 51.52, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4167.0, position_y = -2208.0, position_z = 50.28, orientation = 0.2881, movement_type = 1, wander_distance = 5 WHERE guid = 2568999 AND id = 60442;  -- was -4177.0 -2196.7 50.28, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4176.0, position_y = -2241.0, position_z = 50.92, orientation = 0.8502, movement_type = 1, wander_distance = 5 WHERE guid = 2568990 AND id = 60442;  -- was -4124.0 -2239.5 51.03, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4182.0, position_y = -2230.0, position_z = 51.22, orientation = 0.6202, movement_type = 1, wander_distance = 5 WHERE guid = 2568979 AND id = 60442;  -- was -4072.0 -2179.1 50.28, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4185.0, position_y = -2214.0, position_z = 51.07, orientation = 0.3016, movement_type = 1, wander_distance = 5 WHERE guid = 2568994 AND id = 60442;  -- was -4143.9 -2162.2 51.44, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4185.0, position_y = -2182.0, position_z = 50.80, orientation = 5.9027, movement_type = 1, wander_distance = 5 WHERE guid = 2569003 AND id = 60442;  -- was -4171.1 -2179.1 50.28, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4197.0, position_y = -2206.0, position_z = 54.79, orientation = 0.1049, movement_type = 1, wander_distance = 5 WHERE guid = 2568980 AND id = 60442;  -- was -4063.6 -2170.5 51.22, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4198.0, position_y = -2274.0, position_z = 50.91, orientation = 0.9060, movement_type = 1, wander_distance = 5 WHERE guid = 2569005 AND id = 60442;  -- was -4216.9 -2282.9 52.17, movement_type 0, wander_distance 0
UPDATE creature SET position_x = -4204.0, position_y = -2259.0, position_z = 50.28, orientation = 0.7448, movement_type = 1, wander_distance = 5 WHERE guid = 2569006 AND id = 60442;  -- was -4185.4 -2259.4 50.65, movement_type 0, wander_distance 0

-- Excavators (3374): stock orientation, movement_type and wander_distance.
-- Position stays (235 did not move them).
UPDATE creature SET orientation = 3.8863,   movement_type = 0, wander_distance = 0 WHERE guid = 13584 AND id = 3374;  -- 235: orientation 5.9450
UPDATE creature SET orientation = 0.220147, movement_type = 1, wander_distance = 5 WHERE guid = 13594 AND id = 3374;  -- 235: orientation 0.0040, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 3.16957,  movement_type = 1, wander_distance = 5 WHERE guid = 13598 AND id = 3374;  -- 235: orientation 2.5961, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 5.3058,   movement_type = 0, wander_distance = 0 WHERE guid = 13599 AND id = 3374;  -- 235: orientation 3.9958
UPDATE creature SET orientation = 2.88804,  movement_type = 1, wander_distance = 5 WHERE guid = 13600 AND id = 3374;  -- 235: orientation 3.8585, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 5.61996,  movement_type = 0, wander_distance = 0 WHERE guid = 13604 AND id = 3374;  -- 235: orientation 1.7321
UPDATE creature SET orientation = 5.81195,  movement_type = 0, wander_distance = 0 WHERE guid = 13608 AND id = 3374;  -- 235: orientation 1.7372
UPDATE creature SET orientation = 0.690689, movement_type = 0, wander_distance = 0 WHERE guid = 13610 AND id = 3374;  -- 235: orientation 2.1034
UPDATE creature SET orientation = 0.15708,  movement_type = 1, wander_distance = 5 WHERE guid = 13611 AND id = 3374;  -- 235: orientation 2.6262, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 2.42294,  movement_type = 1, wander_distance = 5 WHERE guid = 13612 AND id = 3374;  -- 235: orientation 0.1703, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 5.87214,  movement_type = 1, wander_distance = 5 WHERE guid = 13613 AND id = 3374;  -- 235: orientation 5.4206, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 5.74213,  movement_type = 1, wander_distance = 5 WHERE guid = 13614 AND id = 3374;  -- 235: orientation 3.3279, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 4.45059,  movement_type = 1, wander_distance = 5 WHERE guid = 13615 AND id = 3374;  -- 235: orientation 5.7735, movement_type 0, wander_distance 0
UPDATE creature SET orientation = 5.18363,  movement_type = 0, wander_distance = 0 WHERE guid = 13616 AND id = 3374;  -- 235: orientation 4.5422

-- The four excavators that dig again (emote_state 173, work). The stock rows.
DELETE FROM creature_addon WHERE guid IN (13599, 13604, 13608, 13610);
INSERT INTO creature_addon (guid, display_id, mount_display_id, equipment_id, stand_state, sheath_state, emote_state, auras) VALUES
(13599, 0, 0, -1, 0, 1, 173, NULL),  -- 235: emote_state 0
(13604, 0, 0, -1, 0, 1, 173, NULL),  -- 235: emote_state 0
(13608, 0, 0, -1, 0, 1, 173, NULL),  -- 235: emote_state 0
(13610, 0, 0, -1, 0, 1, 173, NULL);  -- 235: emote_state 0
