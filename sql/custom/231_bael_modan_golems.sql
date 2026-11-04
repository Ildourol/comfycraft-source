-- ============================================================================
--  Bael Modan golems and crates move out of the missing cave  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\231_bael_modan_golems.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then a restart. Creature and gameobject spawn positions load only at
--  startup; no reload command reaches them.
--
--  Bug 140. "A Pebble to Pick with Them" (40367, Junior Foreman Silverhand
--  66000: kill 16 Bael Modan Golem 60442) and "A Profitable Acquisition"
--  (40130, Kex Blowmaster 60443: 2 Automaton Arm 60192, 2 Automaton Leg
--  60193, 1 Automaton Head 60194) could not be done. The spawns are from an
--  older Turtle map with a deep cave under the Bael Modan dig site (map 1).
--  The 1.18.1 client and the server's maps have no cave: the terrain at the
--  old points is 48 to 132 yd above them, with no navmesh near their height.
--  The one point with a floor (-4215.1 -2217.6, golem 2568976) is a small
--  closed room whose navmesh has no path from the crater.
--
--  The quest items come only from gameobjects, each 100% (-100) for one item:
--    2010825 "Crate labelled B"  -> 60192 Automaton Arm  (4 spawns)
--    2010826 "Crate labelled A"  -> 60193 Automaton Leg  (3 spawns)
--    2010827 "Mechagnome Head"   -> 60194 Automaton Head (1 spawn, 30 s)
--  The golems' loot has no quest item.
--
--  This file moves the 30 live golems and all 8 objects onto the floor of
--  the Bael Modan excavation pit, east of the closed room. The guids stay;
--  only the position (and a creature's orientation) changes. Each point:
--    - terrain height from server\maps\0013936.map (+0.1 for a creature),
--    - on the navmesh within 0.5 yd of that height, with a path from
--      Silverhand's camp (-3993 -2301 91.8) and from Kex (-4033 -2002 91.7),
--    - terrain slope under 0.4 (golems) or 0.2 (objects),
--    - golems 12 yd or more apart, 9.5 yd or more from every Bael'dun spawn
--      point and from the path of the patrolling foreman (guid 13628),
--    - objects 6 yd or more from every creature and every stock object.
--  The golems face the middle of the pit (-4140 -2200).
--
--  Not moved:
--    - 2568977 and 2568978, two golems with spawn_flags 128 (spawn dead).
--      They stand on the pit floor at the closed room, as corpses between
--      pillars, and give no kill credit.
--    - The cave props (5000043 hologram base, 5001340 Ulduar door, 5001491
--      lunchbox). They are scenery under the ground.
--    - Every Bael'dun creature, Prospector Khazgorm and the Dig Rats in the
--      area. Each one stands on reachable navmesh within 1.1 yd.
--
--  Re-runnable. Undo: the old position is in the comment on each line; for a
--  creature, the old orientation is in source/sql/base/tw_world_creature.sql.
-- ============================================================================

-- Bael Modan Golem (60442), north end of the pit to the south arm.
UPDATE creature SET position_x = -4074.0, position_y = -2136.0, position_z = 50.40, orientation = 3.9116 WHERE guid = 2568976 AND id = 60442;  -- was -4215.06 -2217.57 54.17
UPDATE creature SET position_x = -4080.0, position_y = -2182.0, position_z = 50.28, orientation = 3.4330 WHERE guid = 2568979 AND id = 60442;  -- was -4235.83 -2196.10 52.81
UPDATE creature SET position_x = -4092.0, position_y = -2146.0, position_z = 50.62, orientation = 3.9857 WHERE guid = 2568980 AND id = 60442;  -- was -4256.23 -2189.81 52.14
UPDATE creature SET position_x = -4098.0, position_y = -2134.0, position_z = 51.16, orientation = 4.1457 WHERE guid = 2568981 AND id = 60442;  -- was -4255.36 -2210.28 51.99
UPDATE creature SET position_x = -4098.0, position_y = -2198.0, position_z = 52.54, orientation = 3.1892 WHERE guid = 2568982 AND id = 60442;  -- was -4283.75 -2214.97 49.80
UPDATE creature SET position_x = -4100.0, position_y = -2156.0, position_z = 50.71, orientation = 3.9746 WHERE guid = 2568983 AND id = 60442;  -- was -4286.68 -2235.95 50.36
UPDATE creature SET position_x = -4100.0, position_y = -2186.0, position_z = 50.83, orientation = 3.4783 WHERE guid = 2568984 AND id = 60442;  -- was -4306.03 -2203.48 48.75
UPDATE creature SET position_x = -4108.0, position_y = -2146.0, position_z = 50.71, orientation = 4.1774 WHERE guid = 2568985 AND id = 60442;  -- was -4308.08 -2230.34 40.55
UPDATE creature SET position_x = -4110.0, position_y = -2134.0, position_z = 53.69, orientation = 4.2858 WHERE guid = 2568986 AND id = 60442;  -- was -4348.91 -2229.91 48.71
UPDATE creature SET position_x = -4112.0, position_y = -2172.0, position_z = 50.28, orientation = 3.9270 WHERE guid = 2568987 AND id = 60442;  -- was -4335.12 -2222.62 50.47
UPDATE creature SET position_x = -4112.0, position_y = -2200.0, position_z = 50.48, orientation = 3.1416 WHERE guid = 2568988 AND id = 60442;  -- was -4346.95 -2224.25 32.13
UPDATE creature SET position_x = -4116.0, position_y = -2222.0, position_z = 50.42, orientation = 2.3996 WHERE guid = 2568989 AND id = 60442;  -- was -4359.92 -2234.28 30.78
UPDATE creature SET position_x = -4134.0, position_y = -2236.0, position_z = 51.12, orientation = 1.7359 WHERE guid = 2568990 AND id = 60442;  -- was -4370.42 -2209.21 29.23
UPDATE creature SET position_x = -4136.0, position_y = -2202.0, position_z = 50.28, orientation = 2.6779 WHERE guid = 2568991 AND id = 60442;  -- was -4400.30 -2248.22 23.83
UPDATE creature SET position_x = -4138.0, position_y = -2224.0, position_z = 51.34, orientation = 1.6539 WHERE guid = 2568992 AND id = 60442;  -- was -4408.35 -2306.17 8.87
UPDATE creature SET position_x = -4140.0, position_y = -2190.0, position_z = 50.28, orientation = 4.7124 WHERE guid = 2568993 AND id = 60442;  -- was -4433.81 -2252.61 2.86
UPDATE creature SET position_x = -4142.0, position_y = -2172.0, position_z = 50.84, orientation = 4.7837 WHERE guid = 2568994 AND id = 60442;  -- was -4402.47 -2274.13 6.80
UPDATE creature SET position_x = -4150.0, position_y = -2232.0, position_z = 50.91, orientation = 1.2679 WHERE guid = 2568995 AND id = 60442;  -- was -4453.47 -2328.79 12.82
UPDATE creature SET position_x = -4162.0, position_y = -2164.0, position_z = 52.81, orientation = 5.2609 WHERE guid = 2568996 AND id = 60442;  -- was -4454.35 -2347.02 12.82
UPDATE creature SET position_x = -4166.0, position_y = -2234.0, position_z = 52.10, orientation = 0.9179 WHERE guid = 2568997 AND id = 60442;  -- was -4494.39 -2309.27 10.06
UPDATE creature SET position_x = -4170.0, position_y = -2210.0, position_z = 50.48, orientation = 0.3218 WHERE guid = 2568998 AND id = 60442;  -- was -4358.54 -2290.47 18.46
UPDATE creature SET position_x = -4174.0, position_y = -2188.0, position_z = 50.28, orientation = 5.9439 WHERE guid = 2568999 AND id = 60442;  -- was -4328.80 -2293.32 23.09
UPDATE creature SET position_x = -4174.0, position_y = -2222.0, position_z = 52.05, orientation = 0.5743 WHERE guid = 2569000 AND id = 60442;  -- was -4284.35 -2274.66 22.43
UPDATE creature SET position_x = -4178.0, position_y = -2234.0, position_z = 51.12, orientation = 0.7299 WHERE guid = 2569001 AND id = 60442;  -- was -4307.53 -2321.20 18.04
UPDATE creature SET position_x = -4180.0, position_y = -2246.0, position_z = 50.45, orientation = 0.8551 WHERE guid = 2569002 AND id = 60442;  -- was -4304.47 -2357.10 18.60
UPDATE creature SET position_x = -4186.0, position_y = -2182.0, position_z = 51.01, orientation = 5.9102 WHERE guid = 2569003 AND id = 60442;  -- was -4333.68 -2339.57 19.34
UPDATE creature SET position_x = -4186.0, position_y = -2208.0, position_z = 51.16, orientation = 0.1722 WHERE guid = 2569004 AND id = 60442;  -- was -4413.21 -2244.20 6.20
UPDATE creature SET position_x = -4194.0, position_y = -2272.0, position_z = 50.88, orientation = 0.9273 WHERE guid = 2569005 AND id = 60442;  -- was -4423.48 -2301.64 8.01
UPDATE creature SET position_x = -4196.0, position_y = -2258.0, position_z = 50.28, orientation = 0.8029 WHERE guid = 2569006 AND id = 60442;  -- was -4440.54 -2269.17 5.71
UPDATE creature SET position_x = -4208.0, position_y = -2256.0, position_z = 50.60, orientation = 0.6889 WHERE guid = 2569307 AND id = 60442;  -- was -4457.71 -2294.49 9.58

-- Crate labelled B (2010825, arms), Crate labelled A (2010826, legs),
-- Mechagnome Head (2010827). Orientation and rotation stay.
UPDATE gameobject SET position_x = -4154.0, position_y = -2170.0, position_z = 52.00 WHERE guid = 5000037 AND id = 2010825;  -- was -4307.10 -2362.36 18.59
UPDATE gameobject SET position_x = -4092.0, position_y = -2162.0, position_z = 50.97 WHERE guid = 5000038 AND id = 2010826;  -- was -4252.10 -2186.78 51.88
UPDATE gameobject SET position_x = -4140.0, position_y = -2212.0, position_z = 50.18 WHERE guid = 5000039 AND id = 2010826;  -- was -4343.79 -2220.41 49.45
UPDATE gameobject SET position_x = -4188.0, position_y = -2240.0, position_z = 50.28 WHERE guid = 5000040 AND id = 2010826;  -- was -4407.15 -2250.71 5.46
UPDATE gameobject SET position_x = -4116.0, position_y = -2212.0, position_z = 50.19 WHERE guid = 5000041 AND id = 2010825;  -- was -4440.31 -2266.91 5.28
UPDATE gameobject SET position_x = -4192.0, position_y = -2264.0, position_z = 50.18 WHERE guid = 5000044 AND id = 2010827;  -- was -4455.77 -2337.47 12.82
UPDATE gameobject SET position_x = -4070.0, position_y = -2152.0, position_z = 50.18 WHERE guid = 5000045 AND id = 2010825;  -- was -4281.71 -2278.36 21.48
UPDATE gameobject SET position_x = -4130.0, position_y = -2186.0, position_z = 50.76 WHERE guid = 5000046 AND id = 2010825;  -- was -4323.92 -2327.97 11.96
