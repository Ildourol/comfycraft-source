-- ============================================================================
--  COPPER VEINS GIVE 1 TO 3 COPPER ORE  (tw_world)
-- ============================================================================
--  Applied with:
--
--    Get-Content sql\custom\170_copper_ore_1_to_3.sql | `
--      DB\bin\mariadb.exe -h 127.0.0.1 -P 3307 -u root tw_world
--
--  then `reload gameobject_loot_template` in the console.
--
--  Each loot of a Copper Vein gives 1, 2 or 3 Copper Ore (2770), not 1.
--  Three loot tables: 5000052 (gameobjects 1731 and 103713, 1198 spawns),
--  3763 (16 spawns) and 2055 (2 spawns). Stone and gems do not change.
--
--  Re-runnable. To undo: the same UPDATE with maxcount = 1.

UPDATE gameobject_loot_template SET maxcount = 3
WHERE item = 2770 AND mincountOrRef = 1 AND entry IN (5000052, 3763, 2055);
