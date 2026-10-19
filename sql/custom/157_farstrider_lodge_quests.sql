-- ============================================================================
--  FARSTRIDER LODGE: DARK IRON AGGRESSION AND NO MERCY FOR THE WICKED  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\157_farstrider_lodge_quests.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  Then, in the mangosd console:
--    reload creature_loot_template
--    reload game_tele
--  The captive spawns need a server restart. A reload does not add spawns.
--
--  Dark Iron Aggression (41905) asks for 8 Refugee Supplies (42072). Upstream
--  gives the item only to the Stolen Crate (2020263), and the crate has no
--  spawns. The Shadowforge Prowler (80207, 17 spawns at the Dark Iron camp in
--  Loch Modan) now drops it as a quest drop.
--
--  No Mercy For The Wicked (41906) asks the player to open the Shadowforge
--  Cage (3000203). The cage was empty. Arisha Sunblade (80210) and two High
--  Elf Refugees (80208, 80209) now stand in it. go_shadowforge_cage moves
--  them out when the cage opens, and they respawn after 60 s.
--
--  Creature guids 2950100-2950199 are ours, for spawns that fix a quest.
--  Re-runnable.

DELETE FROM creature_loot_template WHERE entry = 80207 AND item = 42072;
INSERT INTO creature_loot_template (entry, item, ChanceOrQuestChance, groupid, mincountOrRef, maxcount, condition_id) VALUES
  (80207, 42072, -75, 0, 1, 1, 0);

DELETE FROM `creature` WHERE `guid` BETWEEN 2950100 AND 2950102;
INSERT INTO `creature` (`guid`, `id`, `id2`, `id3`, `id4`, `map`, `position_x`, `position_y`, `position_z`, `orientation`, `spawntimesecsmin`, `spawntimesecsmax`, `wander_distance`, `health_percent`, `mana_percent`, `movement_type`, `spawn_flags`, `visibility_mod`) VALUES
(2950100, 80210, 0, 0, 0, 0, -5887.87, -4132.60, 388.41, 0.30, 60, 60, 0, 100, 100, 0, 0, 0),
(2950101, 80208, 0, 0, 0, 0, -5888.50, -4132.00, 388.41, 0.30, 60, 60, 0, 100, 100, 0, 0, 0),
(2950102, 80209, 0, 0, 0, 0, -5887.30, -4133.20, 388.41, 0.30, 60, 60, 0, 100, 100, 0, 0, 0);

-- `.tele darkironcamp`: the east edge of the camp, where the captives run to.
-- Needs `reload game_tele`.
DELETE FROM game_tele WHERE name = 'darkironcamp';
INSERT INTO game_tele (position_x, position_y, position_z, orientation, map, name) VALUES
  (-5831.14, -4119.65, 388.5, 3.0, 0, 'darkironcamp');
